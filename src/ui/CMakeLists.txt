set(UI_SOURCES
    MainWindow.cpp
    tabs/DiskPartitionTab.cpp
    tabs/RecoveryTab.cpp
    tabs/ImagingTab.cpp
    tabs/DiagnosticsTab.cpp
    tabs/SecurityTab.cpp
    tabs/MaintenanceTab.cpp
    widgets/DiskMapWidget.cpp
)

set(UI_HEADERS
    MainWindow.h
    tabs/DiskPartitionTab.h
    tabs/RecoveryTab.h
    tabs/ImagingTab.h
    tabs/DiagnosticsTab.h
    tabs/SecurityTab.h
    tabs/MaintenanceTab.h
    widgets/DiskMapWidget.h
)

add_library(spw_ui STATIC ${UI_SOURCES} ${UI_HEADERS})

target_include_directories(spw_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(spw_ui PUBLIC
    spw_core
    Qt6::Widgets
)

# Link the pre-built hardware diagnostics vendor library.
# This is a pre-compiled static library — no source code needed.
target_include_directories(spw_ui PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/hwdiag/include
)

find_library(HWDIAG_LIB
    NAMES spw_hwdiag
    PATHS "${CMAKE_SOURCE_DIR}/third_party/hwdiag/lib"
    NO_DEFAULT_PATH
)
if(HWDIAG_LIB)
    target_link_libraries(spw_ui PRIVATE ${HWDIAG_LIB})
else()
    message(WARNING "libspw_hwdiag not found — run third_party/hwdiag/build_library.bat to build it")
endif()
