// The shortest program: print the phone's accelerometer.
//
//     ./quickstart "http://192.168.1.20:8765/?token=abcd"
#include <allsense/allsense.hpp>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) return std::puts("usage: quickstart <link>"), 1;
    auto phone = allsense::connect(argv[1]);  // the link from the app
    auto accel = phone.subscribe("accelerometer");
    for (;;) {
        allsense::Sample s = accel.get();
        std::printf("%.3f %.3f %.3f\n", s[0], s[1], s[2]);
        std::fflush(stdout);  // show each line at once, also when the output goes to a file or another program
    }
}
