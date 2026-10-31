#include <iostream>
#include "platforms/03_coprocessor_accel/topology.hpp"

int main() {
    tinyarmsim::platform::McuConvPlatform platform;
    std::cout << "[03_coprocessor_accel] Initialized McuConvPlatform (Host + Conv Accelerator).\n";
    std::cout << "[03_coprocessor_accel] Host Memory: " << platform.host_bus().size() << " bytes\n";
    std::cout << "[03_coprocessor_accel] Accel Mailbox: 0x50000000 (IRQ 4)\n";
    return 0;
}
