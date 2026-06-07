/**
 * @file tle_manager.h
 * @brief TLE catalog management for the SDR++ Satellite Tracker module.
 *
 * Responsibilities:
 *   - download a TLE set from CelesTrak (or any URL returning 3LE/TLE text),
 *   - parse it into a registry (NORAD / name / line1 / line2), with the same
 *     line-checksum validation used by SatDump's tle.cpp,
 *   - persist the registry on disk (one .tle file in the SDR++ config dir) so
 *     the catalogue survives restarts and is available offline,
 *   - fetch a single object by NORAD id.
 *
 * The HTTP transfer uses libcurl (CelesTrak is HTTPS-only). Downloads run on a
 * caller-provided worker thread so the GUI thread is never blocked.
 *
 * Parsing/validation logic adapted from SatDump (GPLv3),
 *   src-core/common/tracking/tle.cpp  (parseTLEStream).
 */
#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <optional>

namespace sattrack {

struct TLE {
    int         norad = -1;
    std::string name;
    std::string line1;
    std::string line2;
};

class TLEManager {
public:
    TLEManager() = default;

    // Path to the on-disk catalogue (SDR++ config dir + "/satellite_tracker_tles.txt").
    void setStorePath(const std::string& path) {
        std::lock_guard<std::mutex> lck(mtx);
        storePath = path;
    }

    // Load the on-disk catalogue (if any). Returns the number of TLEs loaded.
    int loadFromDisk();

    // Persist the current registry to storePath. Returns true on success.
    bool saveToDisk();

    // Download from a URL returning TLE/3LE text (e.g. a CelesTrak gp.php query)
    // and *replace* the registry with the parsed result. Returns the number of
    // valid TLEs parsed, or -1 on transfer error (registry left untouched).
    // Blocking; call from a worker thread.
    int downloadReplace(const std::string& url);

    // Download from a URL and *merge* the result into the existing registry
    // (existing NORADs are updated, new ones appended). Returns count parsed,
    // or -1 on transfer error.
    int downloadMerge(const std::string& url);

    // Snapshot copies (thread-safe).
    std::vector<TLE> all();
    size_t           size();

    // Lookup by NORAD; nullopt if absent.
    std::optional<TLE> byNorad(int norad);

    // Last successful update (unix seconds), 0 if never.
    int64_t lastUpdate();

    // Human-readable diagnostic from the last download attempt (HTTP code,
    // body snippet, "rate-limited", ...). Empty if the last attempt succeeded.
    std::string lastError();

    // Parse a TLE/3LE text blob into out. Returns number of valid TLEs.
    // Static so it can be unit-tested without an instance.
    static int parseStream(const std::string& text, std::vector<TLE>& out);

    // Helper: build a CelesTrak group URL, e.g. group="amateur".
    static std::string celestrakGroupURL(const std::string& group) {
        return "https://celestrak.org/NORAD/elements/gp.php?GROUP=" + group + "&FORMAT=tle";
    }
    // Helper: build a CelesTrak single-object URL by catalog number.
    static std::string celestrakCatnrURL(int norad) {
        return "https://celestrak.org/NORAD/elements/gp.php?CATNR=" + std::to_string(norad) + "&FORMAT=tle";
    }

private:
    // Blocking HTTP GET into out. Returns true on success (HTTP 2xx, non-empty).
    // On return, httpCode holds the HTTP status (0 if the transfer never
    // completed) and err holds a human-readable reason when the result is false.
    static bool httpGet(const std::string& url, std::string& out, long& httpCode, std::string& err);

    std::mutex        mtx;
    std::vector<TLE>  registry;
    std::string       storePath;
    int64_t           lastUpdateTime = 0;
    std::string       lastErr;
};

} // namespace sattrack
