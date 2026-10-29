cmake_minimum_required(VERSION 3.15)
project(TinyCpuSim
    VERSION 0.1.0
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings
if (MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Core Library
add_library(tinyarmsim_core INTERFACE)
target_include_directories(tinyarmsim_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Main Executable CLIs
add_executable(tinycpusim src/main.cpp)
target_link_libraries(tinycpusim PRIVATE tinyarmsim_core)

add_executable(tinyarmsim src/main.cpp)
target_link_libraries(tinyarmsim PRIVATE tinyarmsim_core)

# GoogleTest via FetchContent
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Enable CTest
enable_testing()

# Standard-library Python behavior tests; the simulator toolchain remains optional.
find_package(Python3 QUIET COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    add_test(NAME byte_size_test
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s tests -p test_byte_sizes.py
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# Smoke Test
add_executable(smoke_test tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE tinyarmsim_core gtest_main)
include(GoogleTest)
gtest_discover_tests(smoke_test)

# State and Memory Test
add_executable(state_and_memory_test tests/state_and_memory_test.cpp)
target_link_libraries(state_and_memory_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(state_and_memory_test)

# Basic Data Processing Test
add_executable(basic_data_processing_test tests/basic_data_processing_test.cpp)
target_link_libraries(basic_data_processing_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(basic_data_processing_test)

# Extended Data Processing Test
add_executable(extended_data_processing_test tests/extended_data_processing_test.cpp)
target_link_libraries(extended_data_processing_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(extended_data_processing_test)

# Branching and Control Flow Test
add_executable(branching_and_control_flow_test tests/branching_and_control_flow_test.cpp)
target_link_libraries(branching_and_control_flow_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(branching_and_control_flow_test)

# Load/Store and Stack Test
add_executable(load_store_and_stack_test tests/load_store_and_stack_test.cpp)
target_link_libraries(load_store_and_stack_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(load_store_and_stack_test)

# System Control and SVC Test
add_executable(system_control_and_svc_test tests/system_control_and_svc_test.cpp)
target_link_libraries(system_control_and_svc_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(system_control_and_svc_test)

# Loader Test
add_executable(loader_test tests/loader_test.cpp)
target_link_libraries(loader_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(loader_test)

# Disassembler Test
add_executable(disassembler_test tests/disassembler_test.cpp)
target_link_libraries(disassembler_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(disassembler_test)

# Spike Trace Test
add_executable(trace_test tests/trace_test.cpp)
target_link_libraries(trace_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(trace_test)

# ARM GCC Cross-Compiler Detection for Assembly Tests
find_program(ARM_NONE_EABI_GCC arm-none-eabi-gcc)
if (ARM_NONE_EABI_GCC)
    message(STATUS "Found arm-none-eabi-gcc: ${ARM_NONE_EABI_GCC}")
    file(GLOB ASM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/asm/*.s")
    foreach(ASM_SRC ${ASM_SOURCES})
        get_filename_component(ASM_NAME ${ASM_SRC} NAME_WE)
        set(ELF_OUT "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/${ASM_NAME}.elf")
        add_custom_command(
            OUTPUT ${ELF_OUT}
            COMMAND ${ARM_NONE_EABI_GCC} -mcpu=cortex-m3 -mthumb -nostdlib -T "${CMAKE_CURRENT_SOURCE_DIR}/tests/asm/linker.ld" "${ASM_SRC}" -o "${ELF_OUT}"
            DEPENDS "${ASM_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/asm/linker.ld"
            COMMENT "Assembling ${ASM_NAME}.s -> ${ASM_NAME}.elf"
        )
        list(APPEND ASM_ELFS ${ELF_OUT})
    endforeach()
    add_custom_target(build_asm_fixtures ALL DEPENDS ${ASM_ELFS})
endif()

# Assembly Self Test (Integration Test)
add_executable(assembly_self_test tests/assembly_self_test.cpp)
target_link_libraries(assembly_self_test PRIVATE tinyarmsim_core gtest_main)
if (TARGET build_asm_fixtures)
    add_dependencies(assembly_self_test build_asm_fixtures)
endif()
gtest_discover_tests(assembly_self_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# uArch Config and Stats Test
add_executable(config_and_stats_test tests/uarch/config_and_stats_test.cpp)
target_link_libraries(config_and_stats_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(config_and_stats_test)

# Unit Test Config
add_executable(test_config tests/unit/test_config.cpp)
target_link_libraries(test_config PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(test_config)

# Unit Test MMIO Bus
add_executable(test_mmio_bus tests/unit/test_mmio_bus.cpp)
target_link_libraries(test_mmio_bus PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(test_mmio_bus TEST_PREFIX "test_mmio_bus.")

# Unit Test NVIC Interrupt
add_executable(test_nvic_interrupt tests/unit/test_nvic_interrupt.cpp)
target_link_libraries(test_nvic_interrupt PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(test_nvic_interrupt TEST_PREFIX "test_nvic_interrupt.")

# Unit Test Core MMIO Peripherals (UART0, Timer0)
add_executable(test_uart_timer tests/unit/test_uart_timer.cpp)
target_link_libraries(test_uart_timer PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(test_uart_timer TEST_PREFIX "test_uart_timer.")

# Unit Test Peripherals (I2C, SPI, CRC32, Custom)
add_executable(test_peripherals tests/unit/test_peripherals.cpp)
target_link_libraries(test_peripherals PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(test_peripherals TEST_PREFIX "test_peripherals.")

# uArch Cache Test
add_executable(cache_test tests/uarch/cache_test.cpp)
target_link_libraries(cache_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(cache_test)

# uArch MESI Coherence Test
add_executable(coherence_test tests/uarch/coherence_test.cpp)
target_link_libraries(coherence_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(coherence_test)

# uArch Phase 1 Cache Speedup and MESI Demo Test
add_executable(cache_speedup_demo_test tests/uarch/cache_speedup_demo_test.cpp)
target_link_libraries(cache_speedup_demo_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(cache_speedup_demo_test)

# uArch Branch Predictor Test (Bimodal, GShare, TAGE, BTB, RAS)
add_executable(branch_predictor_test tests/uarch/branch_predictor_test.cpp)
target_link_libraries(branch_predictor_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(branch_predictor_test)

# uArch UOp Decoder Test (Store splitting STA/STD, LDM/STM, Branches)
add_executable(uop_decoder_test tests/uarch/uop_decoder_test.cpp)
target_link_libraries(uop_decoder_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(uop_decoder_test)

# uArch Fetch Unit Test (Pre-decode, L1I integration, Queue, Redirection)
add_executable(fetch_unit_test tests/uarch/fetch_unit_test.cpp)
target_link_libraries(fetch_unit_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(fetch_unit_test)

# uArch PRF, FreeList, and RAT Checkpointing Test
add_executable(rat_prf_test tests/uarch/rat_prf_test.cpp)
target_link_libraries(rat_prf_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(rat_prf_test)

# uArch ReorderBuffer and IssueQueue Test
add_executable(rob_issue_queue_test tests/uarch/rob_issue_queue_test.cpp)
target_link_libraries(rob_issue_queue_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(rob_issue_queue_test)

# uArch LSU and Store-to-Load Forwarding Test
add_executable(lsu_test tests/uarch/lsu_test.cpp)
target_link_libraries(lsu_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(lsu_test)

# uArch OoOCore End-to-End Pipeline Test
add_executable(ooo_core_test tests/uarch/ooo_core_test.cpp)
target_link_libraries(ooo_core_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(ooo_core_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# uArch MultiCoreSystem Concurrent Execution Test
add_executable(multicore_system_test tests/uarch/multicore_system_test.cpp)
target_link_libraries(multicore_system_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(multicore_system_test)

# uArch Top-Down Profiler Test
add_executable(topdown_profiler_test tests/topdown_profiler_test.cpp)
target_link_libraries(topdown_profiler_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(topdown_profiler_test)

# Dynamic ROI and m5ops Test
add_executable(roi_m5ops_test tests/roi_m5ops_test.cpp)
target_link_libraries(roi_m5ops_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(roi_m5ops_test)

# uArch BPU and Frontend Isolated Microbenchmarks
add_executable(bpu_frontend_ubench_test tests/uarch/bpu_frontend_ubench_test.cpp)
target_link_libraries(bpu_frontend_ubench_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(bpu_frontend_ubench_test)

# uArch Execution Engine and LSU Isolated Microbenchmarks
add_executable(exec_lsu_ubench_test tests/uarch/exec_lsu_ubench_test.cpp)
target_link_libraries(exec_lsu_ubench_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(exec_lsu_ubench_test)

# uArch ROB and Top-Down Profiler Isolated Microbenchmarks
add_executable(rob_topdown_ubench_test tests/uarch/rob_topdown_ubench_test.cpp)
target_link_libraries(rob_topdown_ubench_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(rob_topdown_ubench_test)

# uArch Cache and Memory Hierarchy Isolated Microbenchmarks
add_executable(cache_ubench_test tests/uarch/cache_ubench_test.cpp)
target_link_libraries(cache_ubench_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(cache_ubench_test)

# uArch LSU and Store Sets Disambiguator Isolated Microbenchmarks
add_executable(lsu_ubench_test tests/uarch/lsu_ubench_test.cpp)
target_link_libraries(lsu_ubench_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(lsu_ubench_test)

# uArch Performance Slicing Test
add_executable(perf_slice_test tests/uarch/perf_slice_test.cpp)
target_link_libraries(perf_slice_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(perf_slice_test)

# uArch Core Microbenchmarks
add_executable(core_ubench_test tests/uarch/core_ubench_test.cpp)
target_link_libraries(core_ubench_test PRIVATE tinyarmsim_core gtest_main)
gtest_discover_tests(core_ubench_test)

# Baremetal Firmware Platform Test
option(ENABLE_BAREMETAL_FIRMWARE "Build and run the Cortex-M firmware acceptance fixture" ON)
if (ENABLE_BAREMETAL_FIRMWARE)
    if (NOT ARM_NONE_EABI_GCC)
        unset(ARM_NONE_EABI_GCC CACHE)
        find_program(ARM_NONE_EABI_GCC arm-none-eabi-gcc)
    endif()
    if (NOT ARM_NONE_EABI_GCC OR NOT EXISTS "${ARM_NONE_EABI_GCC}" OR IS_DIRECTORY "${ARM_NONE_EABI_GCC}")
        message(FATAL_ERROR "ENABLE_BAREMETAL_FIRMWARE requires ARM_NONE_EABI_GCC to name an existing arm-none-eabi-gcc executable; got '${ARM_NONE_EABI_GCC}'")
    endif()
    set(BAREMETAL_ACCEPTANCE_ELF "${CMAKE_CURRENT_BINARY_DIR}/isa_boot_acceptance.elf")
    add_custom_command(
        OUTPUT "${BAREMETAL_ACCEPTANCE_ELF}"
        COMMAND "${ARM_NONE_EABI_GCC}" -mcpu=cortex-m3 -mthumb -O1 -fno-if-conversion -fno-if-conversion2 -ffreestanding -fno-builtin -fdata-sections -ffunction-sections -Wall -Wextra -Werror -nostdlib
            -Wl,--gc-sections -Wl,--fatal-warnings -T "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/linker.ld"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/startup.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/apps/isa_boot_acceptance.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/apps/isa_boot_acceptance.s"
            -o "${BAREMETAL_ACCEPTANCE_ELF}"
        DEPENDS firmware/bsp/startup.s firmware/bsp/linker.ld
            firmware/apps/isa_boot_acceptance.c firmware/apps/isa_boot_acceptance.s
        VERBATIM
        COMMENT "Compiling Cortex-M3 bare-metal C boot acceptance ELF")
    add_custom_target(baremetal_firmware_elf DEPENDS "${BAREMETAL_ACCEPTANCE_ELF}")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_dependencies(tinycpusim baremetal_firmware_elf)
    add_test(NAME cli_firmware_test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_firmware_test.py"
            "$<TARGET_FILE:tinycpusim>" "$<TARGET_FILE:tinyarmsim>" "${BAREMETAL_ACCEPTANCE_ELF}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/test_arithmetic.elf")
    add_test(NAME natural_firmware_test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/natural_firmware_test.py"
            "$<TARGET_FILE:tinycpusim>" "${ARM_NONE_EABI_GCC}"
            "${CMAKE_CURRENT_SOURCE_DIR}")

    set(SCHEDULER_ACCEPTANCE_ELF "${CMAKE_CURRENT_BINARY_DIR}/scheduler_acceptance.elf")
    set(SCHEDULER_ACCEPTANCE_DEBUG_ELF "${CMAKE_CURRENT_BINARY_DIR}/scheduler_acceptance_debug.elf")
    add_custom_command(
        OUTPUT "${SCHEDULER_ACCEPTANCE_ELF}"
        COMMAND "${ARM_NONE_EABI_GCC}" -mcpu=cortex-m3 -mthumb -O1 -fno-if-conversion -fno-if-conversion2 -ffreestanding -fno-builtin -fdata-sections -ffunction-sections -Wall -Wextra -Werror -nostdlib
            -Wl,--gc-sections -Wl,--fatal-warnings -T "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/linker.ld"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/startup.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/uart.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/timer.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/scheduler.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/apps/scheduler_acceptance.c"
            -o "${SCHEDULER_ACCEPTANCE_ELF}"
        DEPENDS firmware/bsp/startup.s firmware/bsp/linker.ld
            firmware/bsp/uart.c firmware/bsp/timer.c firmware/bsp/scheduler.c firmware/bsp/scheduler.h
            firmware/apps/scheduler_acceptance.c
        VERBATIM
        COMMENT "Compiling scheduler acceptance ELF")

    add_custom_command(
        OUTPUT "${SCHEDULER_ACCEPTANCE_DEBUG_ELF}"
        COMMAND "${ARM_NONE_EABI_GCC}" -mcpu=cortex-m3 -mthumb -O1 -DDEBUG -fno-if-conversion -fno-if-conversion2 -ffreestanding -fno-builtin -fdata-sections -ffunction-sections -Wall -Wextra -Werror -nostdlib
            -Wl,--gc-sections -Wl,--fatal-warnings -T "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/linker.ld"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/startup.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/uart.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/timer.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/scheduler.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/apps/scheduler_acceptance.c"
            -o "${SCHEDULER_ACCEPTANCE_DEBUG_ELF}"
        DEPENDS firmware/bsp/startup.s firmware/bsp/linker.ld
            firmware/bsp/uart.c firmware/bsp/timer.c firmware/bsp/scheduler.c firmware/bsp/scheduler.h
            firmware/apps/scheduler_acceptance.c
        VERBATIM
        COMMENT "Compiling scheduler acceptance debug ELF")

    add_custom_target(scheduler_acceptance_elf DEPENDS "${SCHEDULER_ACCEPTANCE_ELF}" "${SCHEDULER_ACCEPTANCE_DEBUG_ELF}")
    add_dependencies(tinycpusim scheduler_acceptance_elf)

    set(SHELL_ACCEPTANCE_ELF "${CMAKE_CURRENT_BINARY_DIR}/shell_acceptance.elf")
    add_custom_command(
        OUTPUT "${SHELL_ACCEPTANCE_ELF}"
        COMMAND "${ARM_NONE_EABI_GCC}" -mcpu=cortex-m3 -mthumb -O1 -fno-if-conversion -fno-if-conversion2 -ffreestanding -fno-builtin -fdata-sections -ffunction-sections -Wall -Wextra -Werror -nostdlib
            -Wl,--gc-sections -Wl,--fatal-warnings -T "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/linker.ld"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/startup.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/uart.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/shell.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/apps/shell_acceptance.c"
            -o "${SHELL_ACCEPTANCE_ELF}"
        DEPENDS firmware/bsp/startup.s firmware/bsp/linker.ld
            firmware/bsp/uart.c firmware/bsp/uart.h firmware/bsp/ring_buffer.h firmware/bsp/shell.c firmware/bsp/shell.h
            firmware/apps/shell_acceptance.c
        VERBATIM
        COMMENT "Compiling shell acceptance ELF")

    add_custom_target(shell_acceptance_elf DEPENDS "${SHELL_ACCEPTANCE_ELF}")
    add_dependencies(tinycpusim shell_acceptance_elf)

    set(REFERENCE_APP_ELF "${CMAKE_CURRENT_BINARY_DIR}/reference_app.elf")
    set(REFERENCE_APP_DEBUG_ELF "${CMAKE_CURRENT_BINARY_DIR}/reference_app_debug.elf")
    add_custom_command(
        OUTPUT "${REFERENCE_APP_ELF}"
        COMMAND "${ARM_NONE_EABI_GCC}" -mcpu=cortex-m3 -mthumb -O1 -fno-if-conversion -fno-if-conversion2 -ffreestanding -fno-builtin -fdata-sections -ffunction-sections -Wall -Wextra -Werror -nostdlib
            -Wl,--gc-sections -Wl,--fatal-warnings -T "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/linker.ld"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/startup.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/uart.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/timer.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/scheduler.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/shell.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/apps/reference_app.c"
            -o "${REFERENCE_APP_ELF}"
        DEPENDS firmware/bsp/startup.s firmware/bsp/linker.ld
            firmware/bsp/uart.c firmware/bsp/uart.h firmware/bsp/timer.c firmware/bsp/scheduler.c
            firmware/bsp/shell.c firmware/bsp/shell.h firmware/bsp/ring_buffer.h
            firmware/apps/reference_app.c
        VERBATIM
        COMMENT "Compiling reference app ELF")

    add_custom_command(
        OUTPUT "${REFERENCE_APP_DEBUG_ELF}"
        COMMAND "${ARM_NONE_EABI_GCC}" -mcpu=cortex-m3 -mthumb -O1 -DDEBUG -fno-if-conversion -fno-if-conversion2 -ffreestanding -fno-builtin -fdata-sections -ffunction-sections -Wall -Wextra -Werror -nostdlib
            -Wl,--gc-sections -Wl,--fatal-warnings -T "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/linker.ld"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/startup.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/uart.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/timer.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/scheduler.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/bsp/shell.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/apps/reference_app.c"
            -o "${REFERENCE_APP_DEBUG_ELF}"
        DEPENDS firmware/bsp/startup.s firmware/bsp/linker.ld
            firmware/bsp/uart.c firmware/bsp/uart.h firmware/bsp/timer.c firmware/bsp/scheduler.c
            firmware/bsp/shell.c firmware/bsp/shell.h firmware/bsp/ring_buffer.h
            firmware/apps/reference_app.c
        VERBATIM
        COMMENT "Compiling reference app debug ELF")

    add_custom_target(reference_app_elf DEPENDS "${REFERENCE_APP_ELF}" "${REFERENCE_APP_DEBUG_ELF}")
    add_dependencies(tinycpusim reference_app_elf)

    add_test(NAME fw_scheduler_test
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_fw_scheduler.py" -v)
    add_test(NAME fw_shell_test
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_fw_shell.py" -v)
    add_test(NAME fw_sustained_test
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_fw_sustained.py" -v)
    add_test(NAME fw_debug_walkthrough_test
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_fw_debug_walkthrough.py" -v)
    add_test(NAME fw_build_matrix_test
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_fw_build_matrix.py" -v)
    add_test(NAME fw_uart_pty_test
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_fw_uart_pty.py" -v)
    add_test(NAME fw_watchdog_test
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_fw_watchdog.py" -v)
    add_test(NAME fw_launcher_test
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_fw_launcher.py" -v)
endif()
add_executable(baremetal_firmware_test tests/baremetal_firmware_test.cpp)
target_link_libraries(baremetal_firmware_test PRIVATE tinyarmsim_core gtest_main)
if (ENABLE_BAREMETAL_FIRMWARE)
    add_dependencies(baremetal_firmware_test baremetal_firmware_elf)
    target_compile_definitions(baremetal_firmware_test PRIVATE
        BAREMETAL_ACCEPTANCE_ELF="${BAREMETAL_ACCEPTANCE_ELF}")
endif()
gtest_discover_tests(baremetal_firmware_test TEST_PREFIX "baremetal_firmware_test.")

