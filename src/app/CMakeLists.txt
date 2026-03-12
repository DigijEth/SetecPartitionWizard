set(APP_SOURCES
    main.cpp
    SingleInstance.cpp
)

set(APP_HEADERS
    SingleInstance.h
)

qt_add_resources(APP_RESOURCES ${CMAKE_SOURCE_DIR}/resources/resources.qrc)

add_executable(SetecPartitionWizard WIN32
    ${APP_SOURCES}
    ${APP_HEADERS}
    ${APP_RESOURCES}
    ${CMAKE_SOURCE_DIR}/resources/setec.rc
)

target_include_directories(SetecPartitionWizard PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(SetecPartitionWizard PRIVATE
    spw_ui
)

# Copy garbage.xtx to build output directory
add_custom_command(TARGET SetecPartitionWizard POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/garbage.xtx"
        "$<TARGET_FILE_DIR:SetecPartitionWizard>/garbage.xtx"
    COMMENT "Copying garbage.xtx to build directory..."
)

# Deploy Qt DLLs on install
include(${CMAKE_SOURCE_DIR}/cmake/QtDeployHelper.cmake)
spw_deploy_qt(SetecPartitionWizard)
