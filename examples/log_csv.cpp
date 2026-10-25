// Record several sensors to one CSV file for 10 seconds.
//
//     ./log_csv "http://…/?token=…" accelerometer gyroscope magnetic_field
#include <allsense/allsense.hpp>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 2) return std::puts("usage: log_csv <link> [stream…]"), 1;
    std::vector<std::string> names(argv + 2, argv + argc);
    if (names.empty()) names = {"accelerometer"};

    auto phone = allsense::connect(argv[1]);
    auto subs = phone.subscribe(names, 100);

    std::ofstream out("allsense_log.csv");
    out << "time,name,seq,values\n";
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    char time[32];
    while (std::chrono::steady_clock::now() < end) {
        for (auto& sub : subs) {
            for (auto& s : sub.read()) {
                std::snprintf(time, sizeof time, "%.6f", s.time);
                out << time << "," << sub.info().name << "," << s.seq;
                for (double v : s.values) out << "," << v;
                out << "\n";
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    for (auto& sub : subs) std::cout << sub.info().name << ": dropped " << sub.dropped() << "\n";
    std::cout << "Saved allsense_log.csv" << std::endl;
}
