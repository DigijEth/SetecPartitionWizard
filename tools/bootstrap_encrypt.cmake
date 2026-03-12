# bootstrap_encrypt.cmake — First-time encryption of secret sources.
#
# Run this ONCE after cloning to generate the .enc files from plaintext:
#   cmake -P tools/bootstrap_encrypt.cmake
#
# After this, commit the .enc files and the plaintext sources will be
# gitignored. Subsequent builds only use the .enc files.
#
# Prerequisites: build spw_src_cipher first:
#   cmake --preset default && cmake --build build/default --target spw_src_cipher

cmake_minimum_required(VERSION 3.25)

set(PASSPHRASE "SQ-1.0.0-WilcoAlpha7")
set(CIPHER_TOOL "${CMAKE_CURRENT_LIST_DIR}/../build/default/tools/spw_src_cipher")
set(ENC_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/ui/tabs/encrypted_src")

# Check tool exists
if(NOT EXISTS "${CIPHER_TOOL}" AND NOT EXISTS "${CIPHER_TOOL}.exe")
    message(FATAL_ERROR
        "spw_src_cipher not found. Build it first:\n"
        "  cmake --preset default\n"
        "  cmake --build build/default --target spw_src_cipher")
endif()

# Fix extension on Windows
if(EXISTS "${CIPHER_TOOL}.exe")
    set(CIPHER_TOOL "${CIPHER_TOOL}.exe")
endif()

set(FILES_TO_ENCRYPT
    "${CMAKE_CURRENT_LIST_DIR}/../src/ui/tabs/StarGenerator.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/ui/tabs/StarGenerator.h"
    "${CMAKE_CURRENT_LIST_DIR}/../src/ui/dialogs/AstroChicken.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/ui/dialogs/AstroChicken.h"
    "${CMAKE_CURRENT_LIST_DIR}/../src/ui/dialogs/Vohaul.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/ui/dialogs/Vohaul.h"
    "${CMAKE_CURRENT_LIST_DIR}/../src/ui/dialogs/Arnoid.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/ui/dialogs/Arnoid.h"
    "${CMAKE_CURRENT_LIST_DIR}/../src/core/security/OratDecoder.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/core/security/OratDecoder.h"
)

file(MAKE_DIRECTORY "${ENC_DIR}")

foreach(SRC_FILE ${FILES_TO_ENCRYPT})
    get_filename_component(BASENAME "${SRC_FILE}" NAME)
    set(ENC_FILE "${ENC_DIR}/${BASENAME}.enc")

    message(STATUS "Encrypting ${BASENAME} -> ${BASENAME}.enc")
    execute_process(
        COMMAND "${CIPHER_TOOL}" encrypt "${PASSPHRASE}" "${SRC_FILE}" "${ENC_FILE}"
        RESULT_VARIABLE RESULT
    )
    if(NOT RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to encrypt ${BASENAME}")
    endif()
endforeach()

message(STATUS "")
message(STATUS "All secret sources encrypted to ${ENC_DIR}/")
message(STATUS "You can now commit the .enc files and remove plaintext from git.")
message(STATUS "The plaintext files are gitignored and will not be tracked.")
