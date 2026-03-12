#pragma once

#include <cstdint>
#include <string>

namespace spw
{

enum class ErrorCode
{
    Success = 0,

    // General
    Unknown,
    InvalidArgument,
    NotImplemented,
    OperationCanceled,
    InsufficientPrivileges,

    // Disk I/O
    DiskNotFound,
    DiskAccessDenied,
    DiskReadError,
    DiskWriteError,
    DiskLockFailed,
    DiskDismountFailed,
    DiskBusy,
    SectorOutOfRange,

    // Partition
    PartitionNotFound,
    PartitionTableCorrupt,
    PartitionTableFull,
    PartitionOverlap,
    PartitionTooSmall,
    PartitionTooLarge,
    InvalidPartitionType,
    AlignmentError,

    // Filesystem
    FilesystemNotSupported,
    FilesystemCorrupt,
    FormatFailed,
    ResizeFailed,
    FilesystemTooSmallToShrink,
    FilesystemCheckFailed,

    // Recovery
    NoPartitionsFound,
    NoFilesRecovered,
    RecoveryScanFailed,

    // Imaging
    ImageCorrupt,
    ImageChecksumMismatch,
    ImageReadError,
    ImageWriteError,
    IsoParseError,
    InsufficientDiskSpace,

    // Security
    Fido2DeviceNotFound,
    Fido2PinRequired,
    Fido2AuthFailed,
    EncryptionFailed,
    DecryptionFailed,
    KeyGenerationFailed,

    // SMART / Diagnostics
    SmartNotSupported,
    SmartReadFailed,
    BenchmarkFailed,

    // Boot
    BootRepairFailed,
    BcdNotFound,
    MbrRepairFailed,

    // System
    OutOfMemory,
    FileNotFound,
    FileCreateFailed,
    WmiQueryFailed,
};

struct ErrorInfo
{
    ErrorCode code = ErrorCode::Success;
    std::string message;
    uint32_t win32Error = 0; // GetLastError() value
    long hresult = 0;        // COM HRESULT if applicable

    bool isOk() const { return code == ErrorCode::Success; }
    bool isError() const { return code != ErrorCode::Success; }

    static ErrorInfo ok() { return {}; }

    static ErrorInfo fromCode(ErrorCode code, const std::string& msg = "")
    {
        return {code, msg, 0, 0};
    }

    static ErrorInfo fromWin32(ErrorCode code, uint32_t win32Err, const std::string& msg = "")
    {
        return {code, msg, win32Err, 0};
    }

    static ErrorInfo fromHResult(ErrorCode code, long hr, const std::string& msg = "")
    {
        return {code, msg, 0, hr};
    }
};

} // namespace spw
