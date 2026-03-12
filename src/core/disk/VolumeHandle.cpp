#include "VolumeHandle.h"

#include <sstream>

namespace spw
{

// ---------------------------------------------------------------------------
static ErrorInfo makeWin32Error(ErrorCode code, const std::string& context)
{
    const DWORD lastErr = ::GetLastError();
    std::ostringstream oss;
    oss << context << " (Win32 error " << lastErr << ")";
    return ErrorInfo::fromWin32(code, lastErr, oss.str());
}

// ---------------------------------------------------------------------------
// Destructor — unlock if locked, then close
// ---------------------------------------------------------------------------
VolumeHandle::~VolumeHandle()
{
    // Best-effort unlock before close; ignore errors in destructor
    if (m_locked && m_handle != INVALID_HANDLE_VALUE)
    {
        DWORD bytesReturned = 0;
        ::DeviceIoControl(m_handle, FSCTL_UNLOCK_VOLUME,
                          nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
    }
    close();
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------
VolumeHandle::VolumeHandle(VolumeHandle&& other) noexcept
    : m_handle(other.m_handle)
    , m_locked(other.m_locked)
    , m_path(std::move(other.m_path))
{
    other.m_handle = INVALID_HANDLE_VALUE;
    other.m_locked = false;
}

VolumeHandle& VolumeHandle::operator=(VolumeHandle&& other) noexcept
{
    if (this != &other)
    {
        // Clean up current state
        if (m_locked && m_handle != INVALID_HANDLE_VALUE)
        {
            DWORD bytesReturned = 0;
            ::DeviceIoControl(m_handle, FSCTL_UNLOCK_VOLUME,
                              nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
        }
        close();

        m_handle = other.m_handle;
        m_locked = other.m_locked;
        m_path = std::move(other.m_path);

        other.m_handle = INVALID_HANDLE_VALUE;
        other.m_locked = false;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Open by drive letter: builds \\.\X: path
// ---------------------------------------------------------------------------
Result<VolumeHandle> VolumeHandle::openByLetter(wchar_t driveLetter, DiskAccessMode mode)
{
    wchar_t path[] = L"\\\\.\\X:";
    path[4] = driveLetter;
    return openPath(path, mode);
}

// ---------------------------------------------------------------------------
// Open by GUID path. The path typically looks like:
//   \\?\Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\
// For CreateFileW we need to strip the trailing backslash if present.
// ---------------------------------------------------------------------------
Result<VolumeHandle> VolumeHandle::openByGuid(const std::wstring& volumeGuidPath, DiskAccessMode mode)
{
    std::wstring path = volumeGuidPath;

    // CreateFileW requires no trailing backslash for raw volume access
    if (!path.empty() && path.back() == L'\\')
    {
        path.pop_back();
    }

    return openPath(path, mode);
}

// ---------------------------------------------------------------------------
// Internal open helper
// ---------------------------------------------------------------------------
Result<VolumeHandle> VolumeHandle::openPath(const std::wstring& path, DiskAccessMode mode)
{
    DWORD desiredAccess = GENERIC_READ;
    if (mode == DiskAccessMode::ReadWrite)
    {
        desiredAccess |= GENERIC_WRITE;
    }

    HANDLE handle = ::CreateFileW(
        path.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
        const DWORD lastErr = ::GetLastError();
        if (lastErr == ERROR_ACCESS_DENIED)
        {
            return ErrorInfo::fromWin32(ErrorCode::DiskAccessDenied, lastErr,
                "Access denied opening volume. Run as Administrator.");
        }
        return makeWin32Error(ErrorCode::DiskNotFound, "Failed to open volume");
    }

    VolumeHandle vol;
    vol.m_handle = handle;
    vol.m_path = path;
    return vol;
}

// ---------------------------------------------------------------------------
bool VolumeHandle::isValid() const
{
    return m_handle != INVALID_HANDLE_VALUE;
}

void VolumeHandle::close()
{
    if (m_handle != INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        m_locked = false;
    }
}

// ---------------------------------------------------------------------------
// FSCTL_LOCK_VOLUME — exclusive access
// ---------------------------------------------------------------------------
Result<void> VolumeHandle::lock()
{
    if (!isValid())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskLockFailed, "Invalid volume handle");
    }

    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(
        m_handle,
        FSCTL_LOCK_VOLUME,
        nullptr, 0,
        nullptr, 0,
        &bytesReturned,
        nullptr);

    if (!ok)
    {
        return makeWin32Error(ErrorCode::DiskLockFailed,
            "FSCTL_LOCK_VOLUME failed — volume may be in use");
    }

    m_locked = true;
    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// FSCTL_UNLOCK_VOLUME
// ---------------------------------------------------------------------------
Result<void> VolumeHandle::unlock()
{
    if (!isValid())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskLockFailed, "Invalid volume handle");
    }

    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(
        m_handle,
        FSCTL_UNLOCK_VOLUME,
        nullptr, 0,
        nullptr, 0,
        &bytesReturned,
        nullptr);

    if (!ok)
    {
        return makeWin32Error(ErrorCode::DiskLockFailed, "FSCTL_UNLOCK_VOLUME failed");
    }

    m_locked = false;
    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// FSCTL_DISMOUNT_VOLUME — volume must be locked first
// ---------------------------------------------------------------------------
Result<void> VolumeHandle::dismount()
{
    if (!isValid())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskDismountFailed, "Invalid volume handle");
    }

    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(
        m_handle,
        FSCTL_DISMOUNT_VOLUME,
        nullptr, 0,
        nullptr, 0,
        &bytesReturned,
        nullptr);

    if (!ok)
    {
        return makeWin32Error(ErrorCode::DiskDismountFailed, "FSCTL_DISMOUNT_VOLUME failed");
    }

    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// Read raw bytes from the volume at a specific byte offset.
// The offset and byteCount should be sector-aligned for raw volume access.
// ---------------------------------------------------------------------------
Result<std::vector<uint8_t>> VolumeHandle::readBytes(uint64_t byteOffset, uint32_t byteCount) const
{
    if (!isValid())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Invalid volume handle");
    }
    if (byteCount == 0)
    {
        return std::vector<uint8_t>{};
    }

    std::vector<uint8_t> buffer(byteCount);

    OVERLAPPED ov = {};
    ov.Offset = static_cast<DWORD>(byteOffset & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(byteOffset >> 32);

    DWORD bytesRead = 0;
    BOOL ok = ::ReadFile(m_handle, buffer.data(), byteCount, &bytesRead, &ov);
    if (!ok)
    {
        return makeWin32Error(ErrorCode::DiskReadError, "ReadFile failed on volume");
    }

    if (bytesRead != byteCount)
    {
        buffer.resize(bytesRead);
    }

    return buffer;
}

// ---------------------------------------------------------------------------
// Write raw bytes to the volume at a specific byte offset.
// ---------------------------------------------------------------------------
Result<void> VolumeHandle::writeBytes(uint64_t byteOffset, const uint8_t* data, uint32_t byteCount) const
{
    if (!isValid())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError, "Invalid volume handle");
    }
    if (byteCount == 0)
    {
        return Result<void>::ok();
    }
    if (!data)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Null data pointer");
    }

    OVERLAPPED ov = {};
    ov.Offset = static_cast<DWORD>(byteOffset & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(byteOffset >> 32);

    DWORD bytesWritten = 0;
    BOOL ok = ::WriteFile(m_handle, data, byteCount, &bytesWritten, &ov);
    if (!ok)
    {
        return makeWin32Error(ErrorCode::DiskWriteError, "WriteFile failed on volume");
    }

    if (bytesWritten != byteCount)
    {
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError, "Partial write to volume");
    }

    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// GetVolumeInformationW by drive letter
// ---------------------------------------------------------------------------
Result<VolumeFilesystemInfo> VolumeHandle::getFilesystemInfo(wchar_t driveLetter)
{
    wchar_t rootPath[] = L"X:\\";
    rootPath[0] = driveLetter;

    wchar_t volumeName[MAX_PATH + 1] = {};
    wchar_t fsName[MAX_PATH + 1] = {};
    DWORD serialNumber = 0;
    DWORD maxComponentLen = 0;
    DWORD fsFlags = 0;

    BOOL ok = ::GetVolumeInformationW(
        rootPath,
        volumeName, MAX_PATH,
        &serialNumber,
        &maxComponentLen,
        &fsFlags,
        fsName, MAX_PATH);

    if (!ok)
    {
        return makeWin32Error(ErrorCode::DiskReadError, "GetVolumeInformationW failed");
    }

    VolumeFilesystemInfo info;
    info.volumeLabel = volumeName;
    info.filesystemName = fsName;
    info.serialNumber = serialNumber;
    info.maxComponentLength = maxComponentLen;
    info.filesystemFlags = fsFlags;
    return info;
}

// ---------------------------------------------------------------------------
// GetVolumeInformationW by GUID path.
// The GUID path MUST have a trailing backslash for this API.
// ---------------------------------------------------------------------------
Result<VolumeFilesystemInfo> VolumeHandle::getFilesystemInfoByGuid(const std::wstring& volumeGuidPath)
{
    std::wstring path = volumeGuidPath;
    // Ensure trailing backslash
    if (!path.empty() && path.back() != L'\\')
    {
        path.push_back(L'\\');
    }

    wchar_t volumeName[MAX_PATH + 1] = {};
    wchar_t fsName[MAX_PATH + 1] = {};
    DWORD serialNumber = 0;
    DWORD maxComponentLen = 0;
    DWORD fsFlags = 0;

    BOOL ok = ::GetVolumeInformationW(
        path.c_str(),
        volumeName, MAX_PATH,
        &serialNumber,
        &maxComponentLen,
        &fsFlags,
        fsName, MAX_PATH);

    if (!ok)
    {
        return makeWin32Error(ErrorCode::DiskReadError,
            "GetVolumeInformationW failed for GUID path");
    }

    VolumeFilesystemInfo info;
    info.volumeLabel = volumeName;
    info.filesystemName = fsName;
    info.serialNumber = serialNumber;
    info.maxComponentLength = maxComponentLen;
    info.filesystemFlags = fsFlags;
    return info;
}

// ---------------------------------------------------------------------------
// GetDiskFreeSpaceExW by drive letter
// ---------------------------------------------------------------------------
Result<VolumeSpaceInfo> VolumeHandle::getSpaceInfo(wchar_t driveLetter)
{
    wchar_t rootPath[] = L"X:\\";
    rootPath[0] = driveLetter;

    ULARGE_INTEGER freeBytesAvailable = {};
    ULARGE_INTEGER totalBytes = {};
    ULARGE_INTEGER totalFreeBytes = {};

    BOOL ok = ::GetDiskFreeSpaceExW(
        rootPath,
        &freeBytesAvailable,
        &totalBytes,
        &totalFreeBytes);

    if (!ok)
    {
        return makeWin32Error(ErrorCode::DiskReadError, "GetDiskFreeSpaceExW failed");
    }

    VolumeSpaceInfo info;
    info.totalBytes = totalBytes.QuadPart;
    info.freeBytes = totalFreeBytes.QuadPart;
    info.availableBytes = freeBytesAvailable.QuadPart;
    return info;
}

// ---------------------------------------------------------------------------
// DeleteVolumeMountPointW
// ---------------------------------------------------------------------------
Result<void> VolumeHandle::deleteMountPoint(const std::wstring& mountPoint)
{
    if (!::DeleteVolumeMountPointW(mountPoint.c_str()))
    {
        return makeWin32Error(ErrorCode::DiskWriteError, "DeleteVolumeMountPointW failed");
    }
    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
Result<void> VolumeHandle::flushBuffers() const
{
    if (!isValid())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError, "Invalid volume handle");
    }

    if (!::FlushFileBuffers(m_handle))
    {
        return makeWin32Error(ErrorCode::DiskWriteError, "FlushFileBuffers failed on volume");
    }

    return Result<void>::ok();
}

} // namespace spw
