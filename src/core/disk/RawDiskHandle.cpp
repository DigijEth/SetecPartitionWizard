#include "RawDiskHandle.h"

#include <sstream>
#include <cstring>

namespace spw
{

// ---------------------------------------------------------------------------
// Helper: Build a Win32 error message string incorporating GetLastError().
// ---------------------------------------------------------------------------
static ErrorInfo makeWin32Error(ErrorCode code, const std::string& context)
{
    const DWORD lastErr = ::GetLastError();
    std::ostringstream oss;
    oss << context << " (Win32 error " << lastErr << ")";
    return ErrorInfo::fromWin32(code, lastErr, oss.str());
}

// ---------------------------------------------------------------------------
// Destructor — RAII close
// ---------------------------------------------------------------------------
RawDiskHandle::~RawDiskHandle()
{
    close();
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------
RawDiskHandle::RawDiskHandle(RawDiskHandle&& other) noexcept
    : m_handle(other.m_handle)
    , m_diskId(other.m_diskId)
    , m_accessMode(other.m_accessMode)
{
    other.m_handle = INVALID_HANDLE_VALUE;
    other.m_diskId = -1;
}

RawDiskHandle& RawDiskHandle::operator=(RawDiskHandle&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_handle = other.m_handle;
        m_diskId = other.m_diskId;
        m_accessMode = other.m_accessMode;
        other.m_handle = INVALID_HANDLE_VALUE;
        other.m_diskId = -1;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Open by disk index
// ---------------------------------------------------------------------------
Result<RawDiskHandle> RawDiskHandle::open(DiskId diskIndex, DiskAccessMode mode)
{
    // Build \\.\PhysicalDriveN path
    std::wostringstream pathStream;
    pathStream << L"\\\\.\\PhysicalDrive" << diskIndex;
    std::wstring path = pathStream.str();

    auto result = openPath(path, mode);
    if (result.isOk())
    {
        result.value().m_diskId = diskIndex;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Open by explicit device path
// ---------------------------------------------------------------------------
Result<RawDiskHandle> RawDiskHandle::openPath(const std::wstring& devicePath, DiskAccessMode mode)
{
    DWORD desiredAccess = GENERIC_READ;
    if (mode == DiskAccessMode::ReadWrite)
    {
        desiredAccess |= GENERIC_WRITE;
    }

    // FILE_SHARE_READ | FILE_SHARE_WRITE is required for physical drives so other
    // processes (including Windows itself) can still access the disk.
    HANDLE handle = ::CreateFileW(
        devicePath.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,    // No FILE_FLAG_OVERLAPPED; we use OVERLAPPED only for offset
        nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
        const DWORD lastErr = ::GetLastError();
        if (lastErr == ERROR_ACCESS_DENIED)
        {
            return ErrorInfo::fromWin32(ErrorCode::DiskAccessDenied, lastErr,
                "Access denied opening disk. Run as Administrator.");
        }
        if (lastErr == ERROR_FILE_NOT_FOUND || lastErr == ERROR_PATH_NOT_FOUND)
        {
            return ErrorInfo::fromWin32(ErrorCode::DiskNotFound, lastErr,
                "Physical disk not found");
        }
        return makeWin32Error(ErrorCode::DiskReadError, "Failed to open disk handle");
    }

    RawDiskHandle diskHandle;
    diskHandle.m_handle = handle;
    diskHandle.m_diskId = -1;    // Caller (open()) sets this
    diskHandle.m_accessMode = mode;
    return diskHandle;
}

// ---------------------------------------------------------------------------
bool RawDiskHandle::isValid() const
{
    return m_handle != INVALID_HANDLE_VALUE;
}

void RawDiskHandle::close()
{
    if (m_handle != INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
}

// ---------------------------------------------------------------------------
// Read sectors at a given LBA using an OVERLAPPED struct to specify the offset.
// ---------------------------------------------------------------------------
Result<std::vector<uint8_t>> RawDiskHandle::readSectors(
    SectorOffset lba, SectorCount count, uint32_t sectorSize) const
{
    if (!isValid())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Invalid disk handle");
    }
    if (count == 0)
    {
        return std::vector<uint8_t>{};
    }

    const uint64_t byteOffset = lba * sectorSize;
    const uint64_t totalBytes = count * sectorSize;

    // Win32 ReadFile length is a DWORD (32-bit), so cap per-call reads.
    // For very large reads we would loop, but typical sector reads are well under 4 GiB.
    if (totalBytes > static_cast<uint64_t>(MAXDWORD))
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Read request exceeds maximum single ReadFile size");
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(totalBytes));

    // Use OVERLAPPED to set the file offset without calling SetFilePointerEx.
    // Even though we did NOT open with FILE_FLAG_OVERLAPPED, Windows still uses
    // the Offset/OffsetHigh fields when you pass an OVERLAPPED to ReadFile on
    // a synchronous handle — the call blocks and reads from the specified offset.
    OVERLAPPED ov = {};
    ov.Offset = static_cast<DWORD>(byteOffset & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(byteOffset >> 32);

    DWORD bytesRead = 0;
    BOOL ok = ::ReadFile(m_handle, buffer.data(), static_cast<DWORD>(totalBytes),
                         &bytesRead, &ov);
    if (!ok)
    {
        return makeWin32Error(ErrorCode::DiskReadError, "ReadFile failed on physical disk");
    }

    if (bytesRead != static_cast<DWORD>(totalBytes))
    {
        // Partial read — resize buffer to what we actually got
        buffer.resize(bytesRead);
    }

    return buffer;
}

// ---------------------------------------------------------------------------
// Write sectors at a given LBA.
// ---------------------------------------------------------------------------
Result<void> RawDiskHandle::writeSectors(
    SectorOffset lba, const uint8_t* data, SectorCount count, uint32_t sectorSize) const
{
    if (!isValid())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError, "Invalid disk handle");
    }
    if (m_accessMode != DiskAccessMode::ReadWrite)
    {
        return ErrorInfo::fromCode(ErrorCode::DiskAccessDenied,
            "Handle opened read-only, cannot write");
    }
    if (count == 0)
    {
        return Result<void>::ok();
    }
    if (!data)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Null data pointer");
    }

    const uint64_t byteOffset = lba * sectorSize;
    const uint64_t totalBytes = count * sectorSize;

    if (totalBytes > static_cast<uint64_t>(MAXDWORD))
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Write request exceeds maximum single WriteFile size");
    }

    OVERLAPPED ov = {};
    ov.Offset = static_cast<DWORD>(byteOffset & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(byteOffset >> 32);

    DWORD bytesWritten = 0;
    BOOL ok = ::WriteFile(m_handle, data, static_cast<DWORD>(totalBytes),
                          &bytesWritten, &ov);
    if (!ok)
    {
        return makeWin32Error(ErrorCode::DiskWriteError, "WriteFile failed on physical disk");
    }

    if (bytesWritten != static_cast<DWORD>(totalBytes))
    {
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError,
            "Partial write: not all sectors were written");
    }

    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// IOCTL_DISK_GET_DRIVE_GEOMETRY_EX
// ---------------------------------------------------------------------------
Result<DiskGeometryInfo> RawDiskHandle::getGeometry() const
{
    if (!isValid())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Invalid disk handle");
    }

    // DISK_GEOMETRY_EX is a variable-length struct; allocate enough space for the
    // base structure plus detection/partition info that Windows may append.
    uint8_t buffer[256] = {};
    DWORD bytesReturned = 0;

    BOOL ok = ::DeviceIoControl(
        m_handle,
        IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
        nullptr, 0,
        buffer, sizeof(buffer),
        &bytesReturned,
        nullptr);

    if (!ok)
    {
        return makeWin32Error(ErrorCode::DiskReadError,
            "IOCTL_DISK_GET_DRIVE_GEOMETRY_EX failed");
    }

    const auto* geomEx = reinterpret_cast<const DISK_GEOMETRY_EX*>(buffer);
    const DISK_GEOMETRY& geom = geomEx->Geometry;

    DiskGeometryInfo info;
    info.totalBytes = static_cast<uint64_t>(geomEx->DiskSize.QuadPart);
    info.bytesPerSector = geom.BytesPerSector;
    info.sectorsPerTrack = geom.SectorsPerTrack;
    info.tracksPerCylinder = geom.TracksPerCylinder;
    info.cylinders = static_cast<uint64_t>(geom.Cylinders.QuadPart);
    info.mediaType = geom.MediaType;

    return info;
}

// ---------------------------------------------------------------------------
// IOCTL_DISK_GET_DRIVE_LAYOUT_EX
// ---------------------------------------------------------------------------
Result<DriveLayoutInfo> RawDiskHandle::getDriveLayout() const
{
    if (!isValid())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Invalid disk handle");
    }

    // The output is DRIVE_LAYOUT_INFORMATION_EX followed by a variable number of
    // PARTITION_INFORMATION_EX entries. Allocate generously.
    constexpr size_t kBufferSize = sizeof(DRIVE_LAYOUT_INFORMATION_EX)
                                   + 128 * sizeof(PARTITION_INFORMATION_EX);
    std::vector<uint8_t> buffer(kBufferSize, 0);

    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(
        m_handle,
        IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
        nullptr, 0,
        buffer.data(), static_cast<DWORD>(buffer.size()),
        &bytesReturned,
        nullptr);

    if (!ok)
    {
        return makeWin32Error(ErrorCode::DiskReadError,
            "IOCTL_DISK_GET_DRIVE_LAYOUT_EX failed");
    }

    const auto* layout = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buffer.data());

    DriveLayoutInfo result;
    result.partitionCount = layout->PartitionCount;

    switch (layout->PartitionStyle)
    {
    case PARTITION_STYLE_MBR:
        result.partitionStyle = PartitionTableType::MBR;
        result.mbrSignature = layout->Mbr.Signature;
        break;
    case PARTITION_STYLE_GPT:
        result.partitionStyle = PartitionTableType::GPT;
        std::memcpy(result.gptDiskId.data, &layout->Gpt.DiskId, 16);
        break;
    default:
        result.partitionStyle = PartitionTableType::Unknown;
        break;
    }

    for (DWORD i = 0; i < layout->PartitionCount; ++i)
    {
        const PARTITION_INFORMATION_EX& partEx = layout->PartitionEntry[i];

        // Windows may return entries with zero length for "empty" slots
        if (partEx.PartitionLength.QuadPart == 0)
            continue;

        DriveLayoutPartition part;
        part.partitionNumber = partEx.PartitionNumber;
        part.startingOffset = static_cast<uint64_t>(partEx.StartingOffset.QuadPart);
        part.partitionLength = static_cast<uint64_t>(partEx.PartitionLength.QuadPart);
        part.rewritePartition = (partEx.RewritePartition != FALSE);
        part.isRecognized = (partEx.PartitionStyle == PARTITION_STYLE_GPT)
                            || IsRecognizedPartition(partEx.Mbr.PartitionType);

        if (partEx.PartitionStyle == PARTITION_STYLE_MBR)
        {
            part.mbrPartitionType = partEx.Mbr.PartitionType;
            part.mbrBootIndicator = (partEx.Mbr.BootIndicator != FALSE);
        }
        else if (partEx.PartitionStyle == PARTITION_STYLE_GPT)
        {
            std::memcpy(part.gptPartitionType.data, &partEx.Gpt.PartitionType, 16);
            std::memcpy(part.gptPartitionId.data, &partEx.Gpt.PartitionId, 16);
            part.gptAttributes = partEx.Gpt.Attributes;
            part.gptName = partEx.Gpt.Name;
        }

        result.partitions.push_back(std::move(part));
    }

    return result;
}

// ---------------------------------------------------------------------------
// Lock a volume by its drive letter. Opens \\.\X: and sends FSCTL_LOCK_VOLUME.
// Returns the handle (caller must close it or pass to unlockVolume).
// ---------------------------------------------------------------------------
Result<HANDLE> RawDiskHandle::lockVolume(wchar_t volumeLetter)
{
    wchar_t path[] = L"\\\\.\\X:";
    path[4] = volumeLetter;

    HANDLE hVolume = ::CreateFileW(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hVolume == INVALID_HANDLE_VALUE)
    {
        return makeWin32Error(ErrorCode::DiskAccessDenied, "Failed to open volume for locking");
    }

    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(
        hVolume,
        FSCTL_LOCK_VOLUME,
        nullptr, 0,
        nullptr, 0,
        &bytesReturned,
        nullptr);

    if (!ok)
    {
        DWORD err = ::GetLastError();
        ::CloseHandle(hVolume);
        return ErrorInfo::fromWin32(ErrorCode::DiskLockFailed, err,
            "FSCTL_LOCK_VOLUME failed — volume may be in use");
    }

    return hVolume;
}

// ---------------------------------------------------------------------------
Result<void> RawDiskHandle::unlockVolume(HANDLE volumeHandle)
{
    if (volumeHandle == INVALID_HANDLE_VALUE)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Invalid volume handle");
    }

    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(
        volumeHandle,
        FSCTL_UNLOCK_VOLUME,
        nullptr, 0,
        nullptr, 0,
        &bytesReturned,
        nullptr);

    if (!ok)
    {
        return makeWin32Error(ErrorCode::DiskLockFailed, "FSCTL_UNLOCK_VOLUME failed");
    }

    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
Result<void> RawDiskHandle::dismountVolume(wchar_t volumeLetter)
{
    wchar_t path[] = L"\\\\.\\X:";
    path[4] = volumeLetter;

    HANDLE hVolume = ::CreateFileW(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hVolume == INVALID_HANDLE_VALUE)
    {
        return makeWin32Error(ErrorCode::DiskAccessDenied, "Failed to open volume for dismount");
    }

    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(
        hVolume,
        FSCTL_DISMOUNT_VOLUME,
        nullptr, 0,
        nullptr, 0,
        &bytesReturned,
        nullptr);

    DWORD err = ::GetLastError();
    ::CloseHandle(hVolume);

    if (!ok)
    {
        return ErrorInfo::fromWin32(ErrorCode::DiskDismountFailed, err,
            "FSCTL_DISMOUNT_VOLUME failed");
    }

    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
Result<void> RawDiskHandle::flushBuffers() const
{
    if (!isValid())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError, "Invalid disk handle");
    }

    if (!::FlushFileBuffers(m_handle))
    {
        return makeWin32Error(ErrorCode::DiskWriteError, "FlushFileBuffers failed");
    }

    return Result<void>::ok();
}

} // namespace spw
