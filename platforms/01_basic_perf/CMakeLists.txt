include("${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/cmake/TinyCpuSimConfig.cmake")

set(PERF_FIB_ELF "${CMAKE_CURRENT_BINARY_DIR}/fibonacci.elf")
add_custom_command(
    OUTPUT "${PERF_FIB_ELF}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/test_fibonacci.elf" "${PERF_FIB_ELF}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/test_fibonacci.elf"
    VERBATIM)
add_custom_target(perf_platform_workloads DEPENDS "${PERF_FIB_ELF}")

add_executable(platform_01_perf_demo demo/main.cpp)
target_link_libraries(platform_01_perf_demo PRIVATE TinyCpuSim::Core)
target_include_directories(platform_01_perf_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_definitions(platform_01_perf_demo PRIVATE PERF_WORKLOAD_ELF="${PERF_FIB_ELF}")
add_dependencies(platform_01_perf_demo perf_platform_workloads)

add_executable(test_perf_platform tests/test_perf_platform.cpp)
target_link_libraries(test_perf_platform PRIVATE TinyCpuSim::Core gtest_main)
target_include_directories(test_perf_platform PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_definitions(test_perf_platform PRIVATE PERF_WORKLOAD_ELF="${PERF_FIB_ELF}")
add_dependencies(test_perf_platform perf_platform_workloads)
add_test(NAME test_perf_platform COMMAND test_perf_platform)
set_tests_properties(test_perf_platform PROPERTIES LABELS "perf")
