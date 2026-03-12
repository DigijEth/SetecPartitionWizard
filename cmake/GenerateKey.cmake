# GenerateKey.cmake — Build-time 1337-bit key generation
# Compiles and runs the keygen tool to produce:
#   1. generated/EmbeddedKey.h (compiled into the app)
#   2. resources/garbage.xtx   (distributed alongside the app)
#
# garbage.xtx is regenerated on EVERY build — it is unique to each build
# and must match the EmbeddedKey.h baked into that specific binary.

set(KEYGEN_SOURCE "${CMAKE_SOURCE_DIR}/tools/keygen.cpp")
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(GENERATED_KEY_HEADER "${GENERATED_DIR}/EmbeddedKey.h")
set(GARBAGE_XTX "${CMAKE_SOURCE_DIR}/resources/garbage.xtx")

file(MAKE_DIRECTORY ${GENERATED_DIR})
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tools")

# Step 1: Compile keygen tool
add_executable(spw_keygen EXCLUDE_FROM_ALL "${KEYGEN_SOURCE}")
if(WIN32)
    target_link_libraries(spw_keygen PRIVATE bcrypt)
endif()
set_target_properties(spw_keygen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)

# Step 2: Always-run target — regenerates both EmbeddedKey.h and garbage.xtx
# every build so the key and .xtx file are always in sync with the binary.
# Using add_custom_target (not add_custom_command) so it runs unconditionally.
add_custom_target(generate_key ALL
    COMMAND $<TARGET_FILE:spw_keygen> "${GENERATED_KEY_HEADER}" "${GARBAGE_XTX}"
    DEPENDS spw_keygen
    COMMENT "Generating build-specific 1337-bit key and garbage.xtx..."
    VERBATIM
)
