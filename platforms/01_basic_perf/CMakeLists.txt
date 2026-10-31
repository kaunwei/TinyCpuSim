include("${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/cmake/TinyCpuSimConfig.cmake")

set(PERF_FIB_ELF "${CMAKE_CURRENT_BINARY_DIR}/fibonacci.elf")
set(PERF_MM_ELF "${CMAKE_CURRENT_BINARY_DIR}/matrix_mul.elf")

if (ARM_NONE_EABI_GCC)
    add_custom_command(
        OUTPUT "${PERF_FIB_ELF}"
        COMMAND "${ARM_NONE_EABI_GCC}" -mcpu=cortex-m3 -mthumb -O2 -ffreestanding -nostdlib -Wall -Wextra
            -T "${CMAKE_CURRENT_SOURCE_DIR}/../common/bsp/linker.ld"
            "${CMAKE_CURRENT_SOURCE_DIR}/../common/bsp/startup.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/fibonacci/main.c"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/../common/bsp"
            -o "${PERF_FIB_ELF}"
        DEPENDS ../common/bsp/startup.s ../common/bsp/linker.ld firmware/fibonacci/main.c
        VERBATIM)

    add_custom_command(
        OUTPUT "${PERF_MM_ELF}"
        COMMAND "${ARM_NONE_EABI_GCC}" -mcpu=cortex-m3 -mthumb -O2 -ffreestanding -nostdlib -Wall -Wextra
            -T "${CMAKE_CURRENT_SOURCE_DIR}/../common/bsp/linker.ld"
            "${CMAKE_CURRENT_SOURCE_DIR}/../common/bsp/startup.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/matrix_mul/main.c"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/../common/bsp"
            -o "${PERF_MM_ELF}"
        DEPENDS ../common/bsp/startup.s ../common/bsp/linker.ld firmware/matrix_mul/main.c
        VERBATIM)

    add_custom_target(perf_platform_elves DEPENDS "${PERF_FIB_ELF}" "${PERF_MM_ELF}")
else()
    add_custom_command(
        OUTPUT "${PERF_FIB_ELF}"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/test_fibonacci.elf" "${PERF_FIB_ELF}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/test_fibonacci.elf"
        VERBATIM)
    add_custom_target(perf_platform_elves DEPENDS "${PERF_FIB_ELF}")
endif()

add_executable(platform_01_perf_demo demo/main.cpp)
target_link_libraries(platform_01_perf_demo PRIVATE TinyCpuSim::Core)
target_include_directories(platform_01_perf_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_definitions(platform_01_perf_demo PRIVATE PERF_WORKLOAD_ELF="${PERF_FIB_ELF}")
add_dependencies(platform_01_perf_demo perf_platform_elves)

add_executable(test_perf_platform tests/test_perf_platform.cpp)
target_link_libraries(test_perf_platform PRIVATE TinyCpuSim::Core gtest_main)
target_include_directories(test_perf_platform PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_definitions(test_perf_platform PRIVATE PERF_WORKLOAD_ELF="${PERF_FIB_ELF}")
add_dependencies(test_perf_platform perf_platform_elves)
add_test(NAME test_perf_platform COMMAND test_perf_platform)
set_tests_properties(test_perf_platform PROPERTIES LABELS "perf")
