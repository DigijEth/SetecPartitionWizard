set(CORE_SOURCES
    # Common
    common/Logging.cpp

    # Disk (stubs — will be implemented in Phase 2)
    # disk/RawDiskHandle.cpp
    # disk/VolumeHandle.cpp
    # disk/DiskEnumerator.cpp
    # disk/PartitionTable.cpp
    # disk/DiskGeometry.cpp
    # disk/VolumeManager.cpp

    # Filesystem (stubs — will be implemented in Phase 4)
    # filesystem/FilesystemFactory.cpp
    # filesystem/FilesystemDetector.cpp
    # filesystem/NtfsDriver.cpp
    # filesystem/Fat32Driver.cpp

    # Operations (stubs — will be implemented in Phase 3)
    # operations/OperationQueue.cpp
    # operations/OperationRunner.cpp
)

set(CORE_HEADERS
    common/Types.h
    common/Result.h
    common/Error.h
    common/Constants.h
    common/Logging.h
    common/Version.h
)

add_library(spw_core STATIC ${CORE_SOURCES} ${CORE_HEADERS})

target_include_directories(spw_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(spw_core PUBLIC
    Qt6::Core
)

# Windows system libraries
if(WIN32)
    target_link_libraries(spw_core PUBLIC
        setupapi
        wbemuuid
        ole32
        oleaut32
    )
endif()
