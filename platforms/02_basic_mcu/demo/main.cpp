#include <iostream>
#include "platforms/02_basic_mcu/topology.hpp"

int main() {
    tinyarmsim::platform::BasicMcuPlatform mcu;
    std::cout << "[02_basic_mcu] Initialized Basic MCU SoC Platform.\n";
    std::cout << "[02_basic_mcu] Bus memory size: " << mcu.bus().size() << " bytes\n";
    std::cout << "[02_basic_mcu] Peripheral devices registered: UART, Timer, GPIO, NVIC.\n";
    return 0;
}
