#include <iostream>
#include <fstream>
#include "platforms/03_coprocessor_accel/topology.hpp"

int main() {
    std::cout << "============================================================\n";
    std::cout << " [03_coprocessor_accel] Heterogeneous Co-Simulation Demo\n";
    std::cout << "============================================================\n";
    std::cout << " Host CPU Config:  configs/host_cpu.cfg\n";
    std::cout << " Accel CPU Config: configs/accel_cpu.cfg\n";

    tinyarmsim::platform::McuConvPlatform platform;
    
    #ifdef CONV_HOST_ELF
    const char* host_elf = CONV_HOST_ELF;
    #else
    const char* host_elf = "conv_host.elf";
    #endif

    #ifdef CONV_ACCEL_ELF
    const char* accel_elf = CONV_ACCEL_ELF;
    #else
    const char* accel_elf = "conv_accelerator.elf";
    #endif

    try {
        platform.load_host_firmware(host_elf);
        platform.load_accel_firmware(accel_elf);
        std::cout << " Loaded Host ELF:  " << host_elf << "\n";
        std::cout << " Loaded Accel ELF: " << accel_elf << "\n";
        std::cout << " Starting dual-core co-simulation...\n";
        uint64_t steps = platform.run(200000, 500);
        std::cout << " Co-simulation completed in " << steps << " steps.\n";
        uint32_t result = platform.host_bus().read32(0x0002FFFC);
        std::cout << " Token result at 0x0002FFFC: 0x" << std::hex << result << std::dec << "\n";
        if (result == 0xCAFECAFE) {
            std::cout << " [PASS] 2D Convolution acceleration verified successfully!\n";
            return 0;
        } else {
            std::cout << " [FAIL] Unexpected token result!\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Simulation error: " << e.what() << "\n";
        return 1;
    }
}
