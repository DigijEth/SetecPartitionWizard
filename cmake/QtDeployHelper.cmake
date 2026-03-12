# QtDeployHelper.cmake - Post-build windeployqt integration
function(spw_deploy_qt TARGET)
    if(WIN32)
        find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
        if(WINDEPLOYQT)
            add_custom_command(TARGET ${TARGET} POST_BUILD
                COMMAND "${WINDEPLOYQT}"
                    --no-translations
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    "$<TARGET_FILE:${TARGET}>"
                COMMENT "Running windeployqt on ${TARGET}..."
            )
        endif()
    endif()
endfunction()
