cmake_minimum_required(VERSION 3.16)

# The Edge Impulse export lives at the repository root, two levels above this
# component. Only the sources the compiled model actually needs are compiled:
# the EON graph uses RESHAPE, CONV_2D, MAX_POOL_2D, FULLY_CONNECTED, SOFTMAX.
set(EI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

set(EI_SOURCES
    # Edge Impulse DSP + classifier runtime
    "${EI_ROOT}/edge-impulse-sdk/classifier/ei_run_classifier_c.cpp"
    "${EI_ROOT}/edge-impulse-sdk/dsp/memory.cpp"
    "${EI_ROOT}/edge-impulse-sdk/dsp/dct/fast-dct-fft.cpp"
    "${EI_ROOT}/edge-impulse-sdk/dsp/kissfft/kiss_fft.cpp"
    "${EI_ROOT}/edge-impulse-sdk/dsp/kissfft/kiss_fftr.cpp"
    "${EI_ROOT}/edge-impulse-sdk/porting/espressif/ei_classifier_porting.cpp"
    "${EI_ROOT}/edge-impulse-sdk/porting/espressif/debug_log.cpp"

    # The compiled (EON) model itself
    "${EI_ROOT}/tflite-model/tflite_learn_1127922_3_compiled.cpp"

    # TensorFlow Lite Micro
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/c/common.c"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/core/api/common.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/micro_allocator.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/micro_allocation_info.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/micro_context.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/micro_error_reporter.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/micro_graph.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/micro_interpreter.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/micro_profiler.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/micro_resource_variable.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/micro_string.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/micro_time.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/micro_utils.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/micro_log.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/memory_helpers.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/fake_micro_context.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/single_arena_buffer_allocator.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/non_persistent_arena_buffer_allocator.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/memory_planner/greedy_memory_planner.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/memory_planner/non_persistent_buffer_planner_shim.cc"

    # Kernels used by the graph
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/kernels/activations.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/kernels/activations_common.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/kernels/conv.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/kernels/conv_common.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/kernels/fully_connected.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/kernels/fully_connected_common.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/kernels/pooling.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/kernels/pooling_common.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/kernels/reshape.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/kernels/softmax.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/kernels/softmax_common.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/kernels/kernel_util_micro.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/micro/kernels/micro_tensor_utils.cc"

    # Shared quantisation helpers the kernels link against
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/kernels/kernel_util_lite.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/kernels/internal/quantization_util.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/kernels/internal/portable_tensor_utils.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/kernels/internal/reference_portable_tensor_utils.cc"
    "${EI_ROOT}/edge-impulse-sdk/tensorflow/lite/kernels/internal/tensor_utils.cc"
)

idf_component_register(
    SRCS ${EI_SOURCES}
    INCLUDE_DIRS
        "${EI_ROOT}"
        "${EI_ROOT}/edge-impulse-sdk"
        "${EI_ROOT}/edge-impulse-sdk/classifier"
        "${EI_ROOT}/edge-impulse-sdk/porting"
        "${EI_ROOT}/model-parameters"
        "${EI_ROOT}/tflite-model"
    REQUIRES
        freertos
        esp_timer
        esp_system
        heap
        log
        driver
)

# CRITICAL: the classic ESP32 is an Xtensa LX6 with no vector instructions.
# ESP-NN SIMD kernels only exist for the LX7 (ESP32-S3) and LX9 (ESP32-P4).
# Leaving this on will fail to link or produce wrong results.
#
# ESP-DSP is the same story: its optimized kernels target the S3/P4 vector
# units. Disabling it makes the MFCC front end fall back to the bundled
# kissfft, which is what the classic ESP32 needs.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=0
    EIDSP_USE_ESP_DSP=0
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-strict-aliasing
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-unused-function
    -Wno-error=uninitialized
    -Wno-error=maybe-uninitialized
    -Wno-error=deprecated-declarations
    -Wno-error=type-limits
    -Wno-error=array-bounds
    -Wno-error=stringop-overflow
    -Wno-error=nonnull
    -Wno-error=restrict
)
