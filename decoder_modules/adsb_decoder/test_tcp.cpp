// test_tcp.cpp — exercise tcp_sender.h against the real SDR++ net:: utility.
// Connects to a local server, sends a few ADS-B JSON lines, exits.
#include "tcp_sender.h"
#include <cstdio>
#include <chrono>

int main(int argc, char** argv) {
    const char* host = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? atoi(argv[2]) : 10100;

    TCPSender tx;
    tx.start(host, port);

    const char* lines[] = {
        R"({"name":"AFR1234","icao":"3C6DD2","date":"2026-06-16","time":"21:00:00","lat":43.2950,"lon":5.3700,"type":"ADSB","speed":420.0,"category":"A3","info":"hdg=270 alt_ft=38000"})",
        R"({"name":"ICAO:40621D","icao":"40621D","date":"2026-06-16","time":"21:00:01","lat":52.2572,"lon":3.9194,"type":"ADSB","speed":null,"category":null,"info":"alt_ft=38000"})",
        R"({"name":"KLM1023","icao":"4840D6","date":"2026-06-16","time":"21:00:02","lat":51.9900,"lon":4.3750,"type":"ADSB","speed":159.0,"category":"A3","info":"hdg=183"})",
    };
    for (auto l : lines) tx.send(l);

    // Give the worker time to connect + flush.
    for (int i = 0; i < 50 && tx.isConnected() == false; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    printf("connected=%d\n", (int)tx.isConnected());
    tx.stop();
    return 0;
}
