cmake_minimum_required(VERSION 3.22)
project(fc_probe LANGUAGES C)
set(CMSIS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.deps/CMSIS-NN")
execute_process(COMMAND git -C "${CMSIS_ROOT}" rev-parse HEAD OUTPUT_VARIABLE REV OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT REV STREQUAL "13c97dbb6f781d4aab38ed34e6e441f42b79aff4")
  message(FATAL_ERROR "Incorrect CMSIS-NN revision")
endif()
add_executable(fc_probe probe.c
  ${CMSIS_ROOT}/Source/FullyConnectedFunctions/arm_fully_connected_s8.c
  ${CMSIS_ROOT}/Source/FullyConnectedFunctions/arm_fully_connected_get_buffer_sizes_s8.c
  ${CMSIS_ROOT}/Source/NNSupportFunctions/arm_nn_vec_mat_mult_t_s8.c)
target_include_directories(fc_probe PRIVATE ${CMSIS_ROOT}/Include)
target_compile_options(fc_probe PRIVATE -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
  -O3 -g3 -ffunction-sections -fdata-sections -fno-lto -fstack-usage)
target_link_options(fc_probe PRIVATE -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
  -fno-lto -nostartfiles -Wl,--gc-sections -Wl,-Map=fc_probe.map -T${CMAKE_CURRENT_LIST_DIR}/probe.ld)
set_target_properties(fc_probe PROPERTIES SUFFIX ".elf")
