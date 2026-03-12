# Compiler warning settings for MSVC and GCC/Clang
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    # Treat warnings as errors in CI builds
    if(DEFINED ENV{CI})
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(DEFINED ENV{CI})
        add_compile_options(-Werror)
    endif()
endif()
