#pragma once

// VirtualDisk — Create, mount, unmount, and convert virtual disk images.
// Supports VHD and VHDX natively via the Windows VirtDisk API.
// VMDK support uses qemu-img as an external converter.

#include <windows.h>
#include <virtdisk.h>

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"

#include <functional>
#include <string>
#include <vector>

namespace spw
{

enum class VirtualDiskFormat
{
    VHD,    // Fixed or dynamic .vhd (Hyper-V, VirtualBox legacy)
    VHDX,   // .vhdx — larger, more resilient, Hyper-V preferred
    VMDK,   // VMware virtual disk (qemu-img required for creation/conversion)
    QCOW2,  // QEMU native format (qemu-img required)
    RAW,    // Flat raw image (.img) — sector-for-sector copy
};

struct VirtualDiskInfo
{
    std::wstring filePath;
    VirtualDiskFormat format = VirtualDiskFormat::VHD;
    uint64_t virtualSizeBytes = 0;   // Logical size the VM sees
    uint64_t physicalSizeBytes = 0;  // Actual file size on disk
    bool     isDynamic = true;       // Dynamic (grows) vs fixed (pre-allocated)
    bool     isMounted = false;
    wchar_t  mountedDriveLetter = L'\0';
    std::wstring physicalDrivePath; // e.g. \\.\PhysicalDrive3 when mounted
};

struct VirtualDiskCreateParams
{
    std::wstring filePath;
    VirtualDiskFormat format = VirtualDiskFormat::VHDX;
    uint64_t sizeBytes = 128ULL * 1024 * 1024 * 1024; // 128 GB default
    bool     dynamic = true;    // dynamic = grows as needed; fixed = pre-allocate all
    uint32_t blockSizeBytes = 0; // 0 = use default (2MB for VHDX, 512KB for VHD)
    uint32_t sectorSize = 512;
};

using VDiskProgress = std::function<void(const std::string& stage, int pct)>;

class VirtualDisk
{
public:
    // ---- Mount / Unmount ----

    // Attach a VHD/VHDX file and return the physical drive path (e.g. \\.\PhysicalDrive3)
    // readOnly = true prevents writes to the image file
    static Result<VirtualDiskInfo> mount(const std::wstring& filePath, bool readOnly = false);

    // Detach a mounted virtual disk by its file path
    static Result<void> unmount(const std::wstring& filePath);

    // Unmount all attached virtual disks (cleanup on exit)
    static void unmountAll();

    // Get info about a VHD/VHDX without mounting it
    static Result<VirtualDiskInfo> queryInfo(const std::wstring& filePath);

    // ---- Create ----

    // Create a new VHD or VHDX file
    static Result<void> create(const VirtualDiskCreateParams& params,
                               VDiskProgress progress = nullptr);

    // ---- Capture (disk → image) ----

    // Read a physical disk and write it as a raw .img or VHD/VHDX
    static Result<void> captureFromDisk(DiskId sourceDiskId,
                                         const std::wstring& outputPath,
                                         VirtualDiskFormat format,
                                         VDiskProgress progress = nullptr);

    // ---- Flash (image → disk) ----

    // Write a virtual disk image to a physical disk (SD card, USB, etc.)
    // Mounts the VHD first if needed, then copies sector-by-sector
    static Result<void> flashToDisk(const std::wstring& imagePath,
                                     DiskId targetDiskId,
                                     VDiskProgress progress = nullptr);

    // ---- Conversion ----

    // Convert between formats using qemu-img (must be on PATH or next to exe)
    static Result<void> convert(const std::wstring& inputPath,
                                 const std::wstring& outputPath,
                                 VirtualDiskFormat targetFormat,
                                 VDiskProgress progress = nullptr);

    // Check if qemu-img is available
    static bool qemuImgAvailable();

    // Detect format from file extension
    static VirtualDiskFormat detectFormat(const std::wstring& filePath);

    // Format name string
    static const char* formatName(VirtualDiskFormat fmt);

private:
    static Result<HANDLE> openVirtDiskHandle(const std::wstring& filePath,
                                              VIRTUAL_DISK_ACCESS_MASK access,
                                              OPEN_VIRTUAL_DISK_FLAG flags);
};

} // namespace spw
