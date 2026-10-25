// Print live accelerometer values.
//
//     ./print_values "http://192.168.1.20:8765/?token=abcd"
#include <allsense/allsense.hpp>
#include <cstdio>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) return std::puts("usage: print_values <link>"), 1;
    try {
        auto phone = allsense::connect(argv[1]);
        std::cout << "Connected to " << phone.device() << ". Streams:";
        for (auto& s : phone.streams()) std::cout << " " << s.name;
        std::cout << std::endl;

        auto accel = phone.subscribe("accelerometer", 20);
        for (;;) {
            allsense::Sample s = accel.get();
            std::printf("x=%+7.3f  y=%+7.3f  z=%+7.3f  m/s²\n", s["x"], s["y"], s["z"]);
            std::fflush(stdout);
        }
    } catch (const allsense::Error& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
