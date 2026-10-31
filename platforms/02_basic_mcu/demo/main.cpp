#include <iostream>
#include "platforms/02_basic_mcu/topology.hpp"

int main() {
    std::cout << "============================================================\n";
    std::cout << " [02_basic_mcu] Cortex-M3 SoC & Baremetal Firmware Demo\n";
    std::cout << "============================================================\n";
    std::cout << " CPU Config: configs/cpu.cfg (In-Order Cortex-M3)\n";

    tinyarmsim::platform::BasicMcuPlatform mcu;
    
    #ifdef MCU_FIRMWARE_ELF
    const char* fw_elf = MCU_FIRMWARE_ELF;
    #else
    const char* fw_elf = "uart_echo.elf";
    #endif

    try {
        mcu.load_firmware(fw_elf, 0x00000000);
        std::cout << " Loaded MCU Firmware: " << fw_elf << "\n";
        std::cout << " Booting Cortex-M3 and stepping execution...\n";
        uint64_t steps = mcu.run(50000, 500);
        std::cout << " Executed " << steps << " instructions.\n";
        std::cout << " [PASS] MCU SoC firmware booted and executed successfully!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "MCU runtime error: " << e.what() << "\n";
        return 1;
    }
}
