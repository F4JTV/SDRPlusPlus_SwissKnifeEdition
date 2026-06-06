// Standalone validation of the ported CW decoder.
// Synthesizes the keying envelope of a known message at 8 kHz and checks decode.
#include "../cw_decoder.h"
#include <cstdio>
#include <map>
#include <random>
#include <string>

static std::map<char,std::string> code = {
    {'A',".-"},{'B',"-..."},{'C',"-.-."},{'D',"-.."},{'E',"."},{'F',"..-."},
    {'G',"--."},{'H',"...."},{'I',".."},{'J',".---"},{'K',"-.-"},{'L',".-.."},
    {'M',"--"},{'N',"-."},{'O',"---"},{'P',".--."},{'Q',"--.-"},{'R',".-."},
    {'S',"..."},{'T',"-"},{'U',"..-"},{'V',"...-"},{'W',".--"},{'X',"-..-"},
    {'Y',"-.--"},{'Z',"--.."},
    {'0',"-----"},{'1',".----"},{'2',"..---"},{'3',"...--"},{'4',"....-"},
    {'5',"....."},{'6',"-...."},{'7',"--..."},{'8',"---.."},{'9',"----."},
    {'/',"-..-."}
};

int main(int argc, char** argv) {
    const int SR = cw::CW_SAMPLERATE;
    int wpm = (argc > 1) ? atoi(argv[1]) : 20;
    std::string msg = (argc > 2) ? argv[2] : "CQ CQ DE F4JTV F4JTV K";

    double unit = 1.2 / wpm;             // dot duration in seconds
    int    usamp = (int)(unit * SR + 0.5);

    std::mt19937 rng(1234);
    std::normal_distribution<double> noise(0.0, 0.06);

    cw::Decoder dec;
    dec.setSpeed(wpm);
    dec.setRange(10);
    dec.setTracking(true);

    std::string out;
    dec.onChar = [&](const std::string& s){ out += s; };

    // Build the full envelope (0/1 keying) with the standard Morse timing.
    std::vector<float> env;
    auto key = [&](int units, double level){
        int n = units * usamp;
        for (int i = 0; i < n; i++) env.push_back((float)level);
    };

    // 0.4 s of silence so the AGC/noise floor settles.
    for (int i = 0; i < SR * 4 / 10; i++) env.push_back(0.0f);

    for (size_t c = 0; c < msg.size(); c++) {
        char ch = toupper(msg[c]);
        if (ch == ' ') { key(7, 0.0); continue; }
        auto it = code.find(ch);
        if (it == code.end()) continue;
        const std::string& cd = it->second;
        for (size_t e = 0; e < cd.size(); e++) {
            key(cd[e] == '.' ? 1 : 3, 1.0);   // element on
            if (e + 1 < cd.size()) key(1, 0.0); // intra-character gap
        }
        key(3, 0.0); // inter-character gap
    }
    key(7, 0.0); // trailing

    // Add gaussian noise and soft rise/fall to mimic a real channel.
    for (size_t i = 0; i < env.size(); i++) {
        double v = env[i] + 0.08 + std::fabs(noise(rng)); // noise floor + key
        env[i] = (float)v;
    }
    // Simple one-pole smoothing for realistic edges.
    float prev = 0.0f;
    for (size_t i = 0; i < env.size(); i++) {
        prev = 0.85f * prev + 0.15f * env[i];
        env[i] = prev;
    }

    // Feed in blocks (as SDR++ would).
    int pos = 0, total = (int)env.size();
    while (pos < total) {
        int n = std::min(1024, total - pos);
        dec.process(&env[pos], n);
        pos += n;
    }

    printf("WPM sent      : %d\n", wpm);
    printf("RX speed est. : %d WPM\n", dec.getReceiveSpeed());
    printf("Sent    : '%s'\n", msg.c_str());
    printf("Decoded : '%s'\n", out.c_str());
    return 0;
}
