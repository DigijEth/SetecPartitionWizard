# GenerateKey.cmake — Build-time 1337-bit key generation
# Compiles and runs the keygen tool to produce:
#   1. generated/EmbeddedKey.h (compiled into the app)
#   2. resources/garbage.xtx   (distributed read-only alongside the app)

set(KEYGEN_SOURCE "${CMAKE_SOURCE_DIR}/tools/keygen.cpp")
set(KEYGEN_BINARY "${CMAKE_BINARY_DIR}/tools/keygen${CMAKE_EXECUTABLE_SUFFIX}")
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(GENERATED_KEY_HEADER "${GENERATED_DIR}/EmbeddedKey.h")
set(GARBAGE_XTX "${CMAKE_SOURCE_DIR}/resources/garbage.xtx")

file(MAKE_DIRECTORY ${GENERATED_DIR})
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tools")

# Step 1: Compile keygen tool (runs on host at configure/build time)
add_executable(spw_keygen EXCLUDE_FROM_ALL "${KEYGEN_SOURCE}")
if(WIN32)
    target_link_libraries(spw_keygen PRIVATE bcrypt)
endif()
set_target_properties(spw_keygen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)

# Step 2: Run keygen to produce header + garbage.xtx
add_custom_command(
    OUTPUT "${GENERATED_KEY_HEADER}" "${GARBAGE_XTX}"
    COMMAND spw_keygen "${GENERATED_KEY_HEADER}" "${GARBAGE_XTX}"
    DEPENDS spw_keygen "${KEYGEN_SOURCE}"
    COMMENT "Generating 1337-bit cryptographic key and garbage.xtx..."
    VERBATIM
)

add_custom_target(generate_key DEPENDS "${GENERATED_KEY_HEADER}" "${GARBAGE_XTX}")
