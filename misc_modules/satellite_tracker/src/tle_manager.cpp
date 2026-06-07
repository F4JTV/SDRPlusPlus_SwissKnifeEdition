/**
 * @file tle_manager.cpp
 * @brief Implementation of the TLE catalogue manager (see tle_manager.h).
 *
 * Parser adapted from SatDump src-core/common/tracking/tle.cpp.
 */
#include "tle_manager.h"

#include <utils/flog.h>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>

namespace sattrack {

// Initialise libcurl exactly once, before the first easy handle is created.
static void ensureCurlGlobalInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
static size_t curlWriteString(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    std::string* str = reinterpret_cast<std::string*>(userp);
    str->append(reinterpret_cast<char*>(contents), realsize);
    return realsize;
}

bool TLEManager::httpGet(const std::string& url, std::string& out, long& httpCode, std::string& err) {
    out.clear();
    httpCode = 0;
    err.clear();

    ensureCurlGlobalInit();

    CURL* curl = curl_easy_init();
    if (!curl) {
        err = "curl init failed";
        flog::error("[SatTracker] {0}", err);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SDR++ satellite_tracker/1.0");
    // Accept gzip/deflate: CelesTrak supports it and the big "active" set drops
    // from ~2 MB to a few hundred kB, which avoids transfer timeouts. Empty
    // string = offer every encoding libcurl was built with.
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
#ifdef CURLSSLOPT_NATIVE_CA
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    }
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        err = std::string("transfer error: ") + curl_easy_strerror(res);
        flog::error("[SatTracker] TLE download {0}", err);
        return false;
    }
    if (httpCode < 200 || httpCode >= 300) {
        err = "HTTP " + std::to_string((int)httpCode) +
              (httpCode == 403 || httpCode == 429 || httpCode == 503
                   ? " (rate-limited / blocked by CelesTrak)" : "");
        flog::error("[SatTracker] TLE download {0}", err);
        return false;
    }
    if (out.empty()) {
        err = "empty response";
        flog::warn("[SatTracker] TLE download returned empty body");
        return false;
    }
    return true;
}

// Build a short, human-readable diagnostic when a 2xx body parses to 0 TLEs.
// CelesTrak (behind Cloudflare) answers throttled/invalid requests with an
// HTML page or a short text message rather than a non-2xx code.
static std::string describeNonTLEBody(const std::string& body) {
    // First non-blank line, trimmed and clipped.
    std::string first;
    {
        std::istringstream ss(body);
        while (std::getline(ss, first)) {
            size_t a = first.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) { first.clear(); continue; }
            size_t b = first.find_last_not_of(" \t\r\n");
            first = first.substr(a, b - a + 1);
            if (!first.empty()) { break; }
        }
    }
    std::string lower = first;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    if (lower.rfind("<!doctype", 0) == 0 || lower.rfind("<html", 0) == 0 ||
        lower.find("cloudflare") != std::string::npos ||
        lower.find("rate") != std::string::npos ||
        lower.find("limit") != std::string::npos ||
        lower.find("invalid query") != std::string::npos ||
        lower.find("error") != std::string::npos) {
        return "blocked / rate-limited (HTTP 200): " +
               first.substr(0, 60);
    }
    if (first.empty()) { return "response had no usable lines"; }
    return "no TLE lines in response: " + first.substr(0, 60);
}

// ---------------------------------------------------------------------------
// Parsing (adapted from SatDump parseTLEStream)
// ---------------------------------------------------------------------------
int TLEManager::parseStream(const std::string& text, std::vector<TLE>& out) {
    std::istringstream inputStream(text);
    std::string this_line;
    std::deque<std::string> tle_lines;
    int total = 0;

    while (std::getline(inputStream, this_line)) {
        // trim whitespace
        this_line.erase(0, this_line.find_first_not_of(" \t\n\r"));
        size_t last = this_line.find_last_not_of(" \t\n\r");
        if (last != std::string::npos) { this_line.erase(last + 1); }
        if (!this_line.empty()) { tle_lines.push_back(this_line); }

        if (tle_lines.size() == 3) {
            bool checksum_good = true;
            for (int i = 1; i < 3; i++) {
                if (!isdigit((unsigned char)tle_lines[i].back()) ||
                    !isdigit((unsigned char)tle_lines[i].front()) ||
                    tle_lines[i].front() - '0' != i) {
                    checksum_good = false;
                    break;
                }
                int checksum = tle_lines[i].back() - '0';
                int actualsum = 0;
                for (int j = 0; j < (int)tle_lines[i].size() - 1; j++) {
                    if (tle_lines[i][j] == '-') {
                        actualsum++;
                    }
                    else if (isdigit((unsigned char)tle_lines[i][j])) {
                        actualsum += tle_lines[i][j] - '0';
                    }
                }
                checksum_good = (checksum == actualsum % 10);
                if (!checksum_good) { break; }
            }
            if (!checksum_good) {
                tle_lines.pop_front();
                continue;
            }

            int norad = -1;
            try {
                std::string sub = tle_lines[2].substr(2, tle_lines[2].substr(2, tle_lines[2].size() - 1).find(' '));
                norad = std::stoi(sub);
            }
            catch (const std::exception&) {
                tle_lines.pop_front();
                continue;
            }

            out.push_back(TLE{ norad, tle_lines[0], tle_lines[1], tle_lines[2] });
            tle_lines.clear();
            total++;
        }
    }

    // Sort by name and de-duplicate by NORAD.
    std::sort(out.begin(), out.end(), [](const TLE& a, const TLE& b) { return a.name < b.name; });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const TLE& a, const TLE& b) { return a.norad == b.norad; }),
              out.end());
    return total;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
int TLEManager::downloadReplace(const std::string& url) {
    std::string body;
    long code = 0;
    std::string err;
    if (!httpGet(url, body, code, err)) {
        { std::lock_guard<std::mutex> lck(mtx); lastErr = err; }
        return -1;
    }

    std::vector<TLE> parsed;
    int n = parseStream(body, parsed);
    if (n <= 0) {
        std::string diag = describeNonTLEBody(body);
        { std::lock_guard<std::mutex> lck(mtx); lastErr = diag; }
        flog::warn("[SatTracker] {0} (from {1})", diag, url);
        return 0;
    }

    {
        std::lock_guard<std::mutex> lck(mtx);
        registry = std::move(parsed);
        lastUpdateTime = (int64_t)std::time(nullptr);
        lastErr.clear();
    }
    saveToDisk();
    flog::info("[SatTracker] loaded {0} TLEs", n);
    return n;
}

int TLEManager::downloadMerge(const std::string& url) {
    std::string body;
    long code = 0;
    std::string err;
    if (!httpGet(url, body, code, err)) {
        { std::lock_guard<std::mutex> lck(mtx); lastErr = err; }
        return -1;
    }

    std::vector<TLE> parsed;
    int n = parseStream(body, parsed);
    if (n <= 0) {
        std::string diag = describeNonTLEBody(body);
        { std::lock_guard<std::mutex> lck(mtx); lastErr = diag; }
        return 0;
    }

    {
        std::lock_guard<std::mutex> lck(mtx);
        for (auto& t : parsed) {
            auto it = std::find_if(registry.begin(), registry.end(),
                                   [&](const TLE& e) { return e.norad == t.norad; });
            if (it != registry.end()) { *it = t; }
            else { registry.push_back(t); }
        }
        std::sort(registry.begin(), registry.end(),
                  [](const TLE& a, const TLE& b) { return a.name < b.name; });
        lastUpdateTime = (int64_t)std::time(nullptr);
        lastErr.clear();
    }
    saveToDisk();
    return n;
}

int TLEManager::loadFromDisk() {
    std::string path;
    {
        std::lock_guard<std::mutex> lck(mtx);
        path = storePath;
    }
    if (path.empty()) { return 0; }

    std::ifstream f(path, std::ios::binary);
    if (!f.good()) { return 0; }

    std::stringstream ss;
    ss << f.rdbuf();
    std::string body = ss.str();

    std::vector<TLE> parsed;
    int n = parseStream(body, parsed);

    std::lock_guard<std::mutex> lck(mtx);
    registry = std::move(parsed);
    return n;
}

bool TLEManager::saveToDisk() {
    std::string path;
    std::vector<TLE> snap;
    {
        std::lock_guard<std::mutex> lck(mtx);
        path = storePath;
        snap = registry;
    }
    if (path.empty()) { return false; }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.good()) {
        flog::error("[SatTracker] could not write TLE store {0}", path);
        return false;
    }
    for (auto& t : snap) {
        f << t.name << "\n" << t.line1 << "\n" << t.line2 << "\n";
    }
    return true;
}

std::vector<TLE> TLEManager::all() {
    std::lock_guard<std::mutex> lck(mtx);
    return registry;
}

size_t TLEManager::size() {
    std::lock_guard<std::mutex> lck(mtx);
    return registry.size();
}

std::optional<TLE> TLEManager::byNorad(int norad) {
    std::lock_guard<std::mutex> lck(mtx);
    auto it = std::find_if(registry.begin(), registry.end(),
                           [&](const TLE& e) { return e.norad == norad; });
    if (it != registry.end()) { return *it; }
    return std::nullopt;
}

int64_t TLEManager::lastUpdate() {
    std::lock_guard<std::mutex> lck(mtx);
    return lastUpdateTime;
}

std::string TLEManager::lastError() {
    std::lock_guard<std::mutex> lck(mtx);
    return lastErr;
}

} // namespace sattrack
