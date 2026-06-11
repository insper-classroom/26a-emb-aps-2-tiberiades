
if (NOT FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    message("Skipping FreeRTOS examples as FREERTOS_KERNEL_PATH not defined")
    return()
endif()

include(FreeRTOS_Kernel_import.cmake)

# DIAGNÓSTICO: WITH_EI=OFF compila o firmware SEM o Edge Impulse (nem fontes,
# nem includes, nem init estático) para isolar se a IA quebra o boot.
option(WITH_EI "Compilar a IA Edge Impulse" OFF)

add_executable(pico_emb
    main.cpp
    hc06.c
)

if(WITH_EI)
    # --- Edge Impulse (reconhecimento de gesto da MPU) ---
    set(MODEL_FOLDER  ${CMAKE_SOURCE_DIR}/ei-model)
    set(EI_SDK_FOLDER ${MODEL_FOLDER}/edge-impulse-sdk)
    include(${EI_SDK_FOLDER}/cmake/utils.cmake)

    # Não definir FREERTOS_ENABLED: o porting do EI usa #ifdef (não #if), então
    # defini-lo (mesmo =0) faz o porting redefinir pvPortCalloc e colidir com o
    # heap_4.c do FreeRTOS. Os wrappers fracos ei_malloc/ei_calloc/ei_free são
    # sobrescritos em main.cpp para usar o heap do FreeRTOS.
    add_definitions(-DEIDSP_QUANTIZE_FILTERBANK=0
                    -DARM_MATH_LOOPUNROLL
                    -DEI_CLASSIFIER_ALLOCATION_STATIC)

    # Fontes do SDK do Edge Impulse + modelo compilado
    RECURSIVE_FIND_FILE(EI_CPP   "${EI_SDK_FOLDER}" "*.cpp")
    RECURSIVE_FIND_FILE(EI_CC    "${EI_SDK_FOLDER}" "*.cc")
    RECURSIVE_FIND_FILE(EI_C     "${EI_SDK_FOLDER}" "*.c")
    RECURSIVE_FIND_FILE(EI_S     "${EI_SDK_FOLDER}" "*.s")
    RECURSIVE_FIND_FILE(EI_MODEL "${MODEL_FOLDER}/tflite-model" "*.cpp")
    target_sources(pico_emb PRIVATE ${EI_CPP} ${EI_CC} ${EI_C} ${EI_S} ${EI_MODEL})

    target_include_directories(pico_emb PRIVATE
        ${MODEL_FOLDER}
        ${MODEL_FOLDER}/tflite-model
        ${MODEL_FOLDER}/model-parameters
        ${EI_SDK_FOLDER}
        ${EI_SDK_FOLDER}/third_party
        ${EI_SDK_FOLDER}/third_party/ruy
        ${EI_SDK_FOLDER}/third_party/gemmlowp
        ${EI_SDK_FOLDER}/third_party/flatbuffers/include
        ${EI_SDK_FOLDER}/tensorflow
        ${EI_SDK_FOLDER}/dsp
        ${EI_SDK_FOLDER}/classifier
        ${EI_SDK_FOLDER}/anomaly
        ${EI_SDK_FOLDER}/CMSIS/NN/Include
        ${EI_SDK_FOLDER}/CMSIS/DSP/Include
        ${EI_SDK_FOLDER}/CMSIS/DSP/PrivateInclude
        ${EI_SDK_FOLDER}/CMSIS/Core/Include
        ${EI_SDK_FOLDER}/porting
    )
endif()

target_include_directories(pico_emb PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(pico_emb PRIVATE
    pico_async_context_freertos
    FreeRTOS-Kernel-Heap4
    pico_stdlib
    pico_unique_id
    hardware_uart
    hardware_irq
    hardware_i2c
    hardware_pwm
    hardware_dma
    hardware_flash
)

target_compile_definitions(pico_emb PRIVATE
    configNUMBER_OF_CORES=1
    WITH_EI=$<IF:$<BOOL:${WITH_EI}>,1,0>
)

pico_enable_stdio_usb(pico_emb 0)
pico_enable_stdio_uart(pico_emb 1)

pico_add_extra_outputs(pico_emb)

set_target_properties(pico_emb PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
