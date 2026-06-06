# Grab all standard source files
file(GLOB_RECURSE SOURCES "*.c" "*.cc" "*.cpp" "*.S")

# Remove all porting directories to avoid compiling code for other architectures (Zephyr, Mbed, etc.)
list(FILTER SOURCES EXCLUDE REGEX "edge-impulse-sdk/porting/")

# Explicitly add back all source files from the official Espressif port (including hardware acceleration)
file(GLOB_RECURSE ESPRESSIF_PORTING 
    "edge-impulse-sdk/porting/espressif/*.c" 
    "edge-impulse-sdk/porting/espressif/*.cpp" 
    "edge-impulse-sdk/porting/espressif/*.cc"
    "edge-impulse-sdk/porting/espressif/*.S"
)
list(APPEND SOURCES ${ESPRESSIF_PORTING})

# Register the Edge Impulse component
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
                 "model-parameters"
                 "tflite-model"
                 "edge-impulse-sdk"
                 "edge-impulse-sdk/porting/espressif"
)

# Apply performance optimizations specifically for the ML inference
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -w -Wno-error)