#include <iostream>
#include "packet_capture.h"
#include "packet_filter.h"
#include "bypass_io.h"

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " <interface>\n";
    std::cout << "Example: " << prog_name << " eth0\n";
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string interface = argv[1];

    // Initialize packet capture
    if (!init_packet_capture(interface)) {
        std::cerr << "Error: Failed to initialize packet capture on " << interface << "\n";
        return 1;
    }

    // Initialize kernel-bypass techniques (optional, depends on system capabilities)
    if (init_bypass_io(interface)) {
        std::cout << "[INFO] Kernel-bypass enabled for " << interface << "\n";
    } else {
        std::cerr << "[WARNING] Kernel-bypass not available, falling back to standard capture.\n";
    }

    std::cout << "[INFO] Starting packet processing on " << interface << "...\n";

    // Start the packet capture loop
    start_packet_processing(interface);

    return 0;
}
