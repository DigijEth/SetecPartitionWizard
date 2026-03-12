set(UI_SOURCES
    MainWindow.cpp
    tabs/DiskPartitionTab.cpp
    tabs/RecoveryTab.cpp
    tabs/ImagingTab.cpp
    tabs/DiagnosticsTab.cpp
    tabs/SecurityTab.cpp
    tabs/MaintenanceTab.cpp
)

set(UI_HEADERS
    MainWindow.h
    tabs/DiskPartitionTab.h
    tabs/RecoveryTab.h
    tabs/ImagingTab.h
    tabs/DiagnosticsTab.h
    tabs/SecurityTab.h
    tabs/MaintenanceTab.h
)

add_library(spw_ui STATIC ${UI_SOURCES} ${UI_HEADERS})

target_include_directories(spw_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(spw_ui PUBLIC
    spw_core
    Qt6::Widgets
)
