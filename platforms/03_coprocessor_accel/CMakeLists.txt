include("${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/cmake/TinyCpuSimConfig.cmake")

set(ACCEL_CONV_HOST_ELF "${CMAKE_CURRENT_BINARY_DIR}/conv_host.elf")
set(ACCEL_CONV_ACCEL_ELF "${CMAKE_CURRENT_BINARY_DIR}/conv_accelerator.elf")

if (ARM_NONE_EABI_GCC)
    add_custom_command(
        OUTPUT "${ACCEL_CONV_HOST_ELF}"
        COMMAND "${ARM_NONE_EABI_GCC}" -mcpu=cortex-m3 -mthumb -O1 -ffreestanding -nostdlib -Wall -Wextra
            -T "${CMAKE_CURRENT_SOURCE_DIR}/../common/bsp/linker.ld"
            "${CMAKE_CURRENT_SOURCE_DIR}/../common/bsp/startup.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/conv_host/main.c"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/../common/bsp"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
            -o "${ACCEL_CONV_HOST_ELF}"
        DEPENDS ../common/bsp/startup.s ../common/bsp/linker.ld ../common/bsp/mailbox.h firmware/conv_host/main.c
        VERBATIM)

    add_custom_command(
        OUTPUT "${ACCEL_CONV_ACCEL_ELF}"
        COMMAND "${ARM_NONE_EABI_GCC}" -mcpu=cortex-m3 -mthumb -O1 -ffreestanding -nostdlib -Wall -Wextra
            -T "${CMAKE_CURRENT_SOURCE_DIR}/../common/bsp/linker.ld"
            "${CMAKE_CURRENT_SOURCE_DIR}/../common/bsp/startup.s"
            "${CMAKE_CURRENT_SOURCE_DIR}/firmware/conv_accelerator/main.c"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/../common/bsp"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
            -o "${ACCEL_CONV_ACCEL_ELF}"
        DEPENDS ../common/bsp/startup.s ../common/bsp/linker.ld ../common/bsp/mailbox.h firmware/conv_accelerator/main.c
        VERBATIM)

    add_custom_target(accel_platform_elves DEPENDS "${ACCEL_CONV_HOST_ELF}" "${ACCEL_CONV_ACCEL_ELF}")

    add_executable(platform_03_accel_demo demo/main.cpp)
    target_link_libraries(platform_03_accel_demo PRIVATE TinyCpuSim::Core)
    target_include_directories(platform_03_accel_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_compile_definitions(platform_03_accel_demo PRIVATE
        CONV_HOST_ELF="${ACCEL_CONV_HOST_ELF}"
        CONV_ACCEL_ELF="${ACCEL_CONV_ACCEL_ELF}")
    add_dependencies(platform_03_accel_demo accel_platform_elves)

    add_executable(test_coprocessor_accel tests/test_coprocessor_accel.cpp)
    target_link_libraries(test_coprocessor_accel PRIVATE TinyCpuSim::Core gtest_main)
    target_include_directories(test_coprocessor_accel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_compile_definitions(test_coprocessor_accel PRIVATE
        CONV_HOST_ELF="${ACCEL_CONV_HOST_ELF}"
        CONV_ACCEL_ELF="${ACCEL_CONV_ACCEL_ELF}")
    add_dependencies(test_coprocessor_accel accel_platform_elves)
    add_test(NAME test_coprocessor_accel COMMAND test_coprocessor_accel)
    set_tests_properties(test_coprocessor_accel PROPERTIES LABELS "accel")
endif()
