set(CORE_SOURCES
    # Common
    common/Types.cpp
    common/Logging.cpp

    # Disk I/O and enumeration
    disk/RawDiskHandle.cpp
    disk/VolumeHandle.cpp
    disk/DiskEnumerator.cpp
    disk/DiskGeometry.cpp
    disk/SmartReader.cpp
    disk/PartitionTable.cpp
    disk/FilesystemDetector.cpp
    disk/FilesystemInfo.cpp

    # Filesystem
    filesystem/FormatEngine.cpp

    # Operations
    operations/OperationQueue.cpp
    operations/PartitionOperations.cpp

    # Recovery
    recovery/PartitionRecovery.cpp
    recovery/FileRecovery.cpp
    recovery/BootRepair.cpp

    # Diagnostics
    diagnostics/Benchmark.cpp
    diagnostics/SurfaceScan.cpp

    # Imaging
    imaging/Checksums.cpp
    imaging/DiskCloner.cpp
    imaging/ImageCreator.cpp
    imaging/ImageRestorer.cpp
    imaging/IsoFlasher.cpp

    # Maintenance
    maintenance/SecureErase.cpp
    maintenance/SdCardRecovery.cpp

    # Security
    security/EncryptedVault.cpp
    security/Fido2Manager.cpp
    security/BootAuthenticator.cpp
)

set(CORE_HEADERS
    common/Types.h
    common/Result.h
    common/Error.h
    common/Constants.h
    common/Logging.h
    common/Version.h
    common/Obfuscate.h
    disk/RawDiskHandle.h
    disk/VolumeHandle.h
    disk/DiskEnumerator.h
    disk/DiskGeometry.h
    disk/SmartReader.h
    disk/PartitionTable.h
    disk/FilesystemDetector.h
    disk/FilesystemInfo.h
    filesystem/FormatEngine.h
    operations/Operation.h
    operations/OperationQueue.h
    operations/PartitionOperations.h
    recovery/PartitionRecovery.h
    recovery/FileRecovery.h
    recovery/BootRepair.h
    diagnostics/Benchmark.h
    diagnostics/SurfaceScan.h
    imaging/Checksums.h
    imaging/DiskCloner.h
    imaging/ImageCreator.h
    imaging/ImageRestorer.h
    imaging/IsoFlasher.h
    maintenance/SecureErase.h
    maintenance/SdCardRecovery.h
    security/EncryptedVault.h
    security/Fido2Manager.h
    security/BootAuthenticator.h
)

add_library(spw_core STATIC ${CORE_SOURCES} ${CORE_HEADERS})

# Depend on build-time key generation
add_dependencies(spw_core generate_key)

target_include_directories(spw_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_BINARY_DIR}/generated
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
        bcrypt
        ntdll
        virtdisk
        hid
    )
endif()
