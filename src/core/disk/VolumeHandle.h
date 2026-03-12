#pragma once

// VolumeHandle — RAII wrapper for Windows volume access via \\.\X: or volume GUID paths.
// Supports locking, dismounting, reading/writing raw volume data, and querying volume info.

#include <windows.h>
#include <winioctl.h>

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace spw
{

// Volume filesystem info from GetVolumeInformationW
struct VolumeFilesystemInfo
{
    std::wstring volumeLabel;
    std::wstring filesystemName;    // e.g. L"NTFS", L"FAT32"
    uint32_t serialNumber = 0;
    uint32_t maxComponentLength = 0;
    uint32_t filesystemFlags = 0;
};

// Volume space info
struct VolumeSpaceInfo
{
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t availableBytes = 0;   // available to current user (may differ from free if quotas)
};

class VolumeHandle
{
public:
    VolumeHandle() = default;
    ~VolumeHandle();

    // Non-copyable, movable
    VolumeHandle(const VolumeHandle&) = delete;
    VolumeHandle& operator=(const VolumeHandle&) = delete;
    VolumeHandle(VolumeHandle&& other) noexcept;
    VolumeHandle& operator=(VolumeHandle&& other) noexcept;

    // Open volume by drive letter (e.g. L'C')
    static Result<VolumeHandle> openByLetter(wchar_t driveLetter, DiskAccessMode mode);

    // Open volume by GUID path (e.g. L"\\?\Volume{GUID}\")
    static Result<VolumeHandle> openByGuid(const std::wstring& volumeGuidPath, DiskAccessMode mode);

    bool isValid() const;
    void close();

    // Lock volume for exclusive access (FSCTL_LOCK_VOLUME)
    Result<void> lock();

    // Unlock volume (FSCTL_UNLOCK_VOLUME)
    Result<void> unlock();

    // Dismount volume (FSCTL_DISMOUNT_VOLUME). Volume must be locked first.
    Result<void> dismount();

    // Read raw bytes from the volume at a byte offset
    Result<std::vector<uint8_t>> readBytes(uint64_t byteOffset, uint32_t byteCount) const;

    // Write raw bytes to the volume at a byte offset
    Result<void> writeBytes(uint64_t byteOffset, const uint8_t* data, uint32_t byteCount) const;

    // Get volume filesystem info (label, FS name, serial, flags)
    // Uses the drive root path (e.g. "C:\"), not the handle.
    static Result<VolumeFilesystemInfo> getFilesystemInfo(wchar_t driveLetter);

    // Get volume filesystem info by GUID path
    static Result<VolumeFilesystemInfo> getFilesystemInfoByGuid(const std::wstring& volumeGuidPath);

    // Get free/total space for a volume
    static Result<VolumeSpaceInfo> getSpaceInfo(wchar_t driveLetter);

    // Delete a volume mount point (e.g. to remove a drive letter assignment)
    static Result<void> deleteMountPoint(const std::wstring& mountPoint);

    // Flush buffers
    Result<void> flushBuffers() const;

    HANDLE nativeHandle() const { return m_handle; }

private:
    // Internal open helper
    static Result<VolumeHandle> openPath(const std::wstring& path, DiskAccessMode mode);

    HANDLE m_handle = INVALID_HANDLE_VALUE;
    bool m_locked = false;
    std::wstring m_path;
};

} // namespace spw
