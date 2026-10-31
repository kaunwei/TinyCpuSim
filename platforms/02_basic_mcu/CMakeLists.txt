include("${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/cmake/TinyCpuSimConfig.cmake")

set(MCU_UART_ECHO_ELF "${CMAKE_CURRENT_BINARY_DIR}/uart_echo.elf")

if (ARM_NONE_EABI_GCC)
    add_custom_command(
        OUTPUT "${MCU_UART_ECHO_ELF}"
        COMMAND "${ARM_NONE_EABI_GCC}" -mcpu=cortex-m3 -mthumb -O2 -ffreestanding -nostdlib -Wall -Wextra
            -T "${CMAKE_CURRENT_SOURCE_DIR}/bsp/linker.ld"
            "${CMAKE_CURRENT_SOURCE_DIR}/bsp/startup.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/bsp/uart.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/uart_echo/main.c"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/bsp"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
            -o "${MCU_UART_ECHO_ELF}"
        DEPENDS bsp/startup.s bsp/linker.ld bsp/uart.c bsp/uart.h firmware/uart_echo/main.c
        VERBATIM)

    add_custom_target(mcu_platform_elves DEPENDS "${MCU_UART_ECHO_ELF}")

    add_executable(platform_02_mcu_demo demo/main.cpp)
    target_link_libraries(platform_02_mcu_demo PRIVATE TinyCpuSim::Core)
    target_include_directories(platform_02_mcu_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_compile_definitions(platform_02_mcu_demo PRIVATE MCU_FIRMWARE_ELF="${MCU_UART_ECHO_ELF}")
    add_dependencies(platform_02_mcu_demo mcu_platform_elves)

    add_executable(test_mcu_platform tests/test_mcu_platform.cpp)
    target_link_libraries(test_mcu_platform PRIVATE TinyCpuSim::Core gtest_main)
    target_include_directories(test_mcu_platform PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_compile_definitions(test_mcu_platform PRIVATE MCU_FIRMWARE_ELF="${MCU_UART_ECHO_ELF}")
    add_dependencies(test_mcu_platform mcu_platform_elves)
    add_test(NAME test_mcu_platform COMMAND test_mcu_platform)
    set_tests_properties(test_mcu_platform PROPERTIES LABELS "mcu")
endif()
