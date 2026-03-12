set(APP_SOURCES
    main.cpp
    SingleInstance.cpp
)

set(APP_HEADERS
    SingleInstance.h
)

add_executable(SetecPartitionWizard WIN32
    ${APP_SOURCES}
    ${APP_HEADERS}
    ${CMAKE_SOURCE_DIR}/resources/setec.rc
)

target_include_directories(SetecPartitionWizard PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(SetecPartitionWizard PRIVATE
    spw_ui
)

# Deploy Qt DLLs on install
include(${CMAKE_SOURCE_DIR}/cmake/QtDeployHelper.cmake)
spw_deploy_qt(SetecPartitionWizard)
