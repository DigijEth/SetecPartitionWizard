#include "VirtualDisk.h"

#include "../disk/DiskEnumerator.h"
#include "../common/Logging.h"

#include <windows.h>
#include <virtdisk.h>
#include <winioctl.h>

// Define the GUID manually — avoids initguid.h ordering issues in a static lib.
// Value from Windows SDK virtdisk.h DEFINE_GUID(VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT, ...)
static const GUID kVendorMicrosoft =
    { 0xec984aec, 0xa0f9, 0x47e9, { 0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b } };

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <vector>

// Link against virtdisk.lib — already in core CMakeLists
#pragma comment(lib, "virtdisk.lib")

namespace spw
{

// ============================================================================
// Helpers
// ============================================================================

VirtualDiskFormat VirtualDisk::detectFormat(const std::wstring& filePath)
{
    auto ext = filePath.substr(filePath.rfind(L'.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext == L"vhdx") return VirtualDiskFormat::VHDX;
    if (ext == L"vhd")  return VirtualDiskFormat::VHD;
    if (ext == L"vmdk") return VirtualDiskFormat::VMDK;
    if (ext == L"qcow2" || ext == L"qcow") return VirtualDiskFormat::QCOW2;
    return VirtualDiskFormat::RAW;
}

const char* VirtualDisk::formatName(VirtualDiskFormat fmt)
{
    switch (fmt)
    {
    case VirtualDiskFormat::VHD:   return "VHD";
    case VirtualDiskFormat::VHDX:  return "VHDX";
    case VirtualDiskFormat::VMDK:  return "VMDK";
    case VirtualDiskFormat::QCOW2: return "QCOW2";
    case VirtualDiskFormat::RAW:   return "RAW";
    }
    return "Unknown";
}

bool VirtualDisk::qemuImgAvailable()
{
    // Check PATH and app directory
    DWORD r = SearchPathW(nullptr, L"qemu-img.exe", nullptr, 0, nullptr, nullptr);
    return (r > 0);
}

Result<HANDLE> VirtualDisk::openVirtDiskHandle(const std::wstring& filePath,
                                                VIRTUAL_DISK_ACCESS_MASK access,
                                                OPEN_VIRTUAL_DISK_FLAG flags)
{
    VirtualDiskFormat fmt = detectFormat(filePath);

    VIRTUAL_STORAGE_TYPE storageType = {};
    storageType.DeviceId = (fmt == VirtualDiskFormat::VHD)
                           ? VIRTUAL_STORAGE_TYPE_DEVICE_VHD
                           : VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storageType.VendorId = kVendorMicrosoft;

    OPEN_VIRTUAL_DISK_PARAMETERS params = {};
    params.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    params.Version1.RWDepth = OPEN_VIRTUAL_DISK_RW_DEPTH_DEFAULT;

    HANDLE hVdisk = INVALID_HANDLE_VALUE;
    DWORD err = OpenVirtualDisk(&storageType, filePath.c_str(),
                                access, flags, &params, &hVdisk);
    if (err != ERROR_SUCCESS)
        return ErrorInfo::fromWin32(ErrorCode::DiskAccessDenied, err,
                                    "OpenVirtualDisk failed");
    return hVdisk;
}

// ============================================================================
// Mount
// ============================================================================

Result<VirtualDiskInfo> VirtualDisk::mount(const std::wstring& filePath, bool readOnly)
{
    VirtualDiskFormat fmt = detectFormat(filePath);
    if (fmt == VirtualDiskFormat::VMDK || fmt == VirtualDiskFormat::QCOW2 || fmt == VirtualDiskFormat::RAW)
    {
        // These formats require conversion to VHD/VHDX first, or use ImDisk/Arsenal Image Mounter
        return ErrorInfo::fromCode(ErrorCode::FilesystemNotSupported,
            std::string(formatName(fmt)) + " cannot be mounted natively on Windows. "
            "Convert to VHDX first (use the Convert function), or install Arsenal Image Mounter.");
    }

    auto mask = readOnly ? VIRTUAL_DISK_ACCESS_ATTACH_RO : VIRTUAL_DISK_ACCESS_ALL;
    auto handleResult = openVirtDiskHandle(filePath, mask, OPEN_VIRTUAL_DISK_FLAG_NONE);
    if (handleResult.isError())
        return handleResult.error();

    HANDLE hVdisk = handleResult.value();

    // Attach the disk (makes it visible to the OS as a physical disk)
    ATTACH_VIRTUAL_DISK_PARAMETERS attachParams = {};
    attachParams.Version = ATTACH_VIRTUAL_DISK_VERSION_1;

    ATTACH_VIRTUAL_DISK_FLAG attachFlags = ATTACH_VIRTUAL_DISK_FLAG_NONE;
    if (readOnly)
        attachFlags = static_cast<ATTACH_VIRTUAL_DISK_FLAG>(
            attachFlags | ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY);

    DWORD err = AttachVirtualDisk(hVdisk, nullptr, attachFlags,
                                   0, &attachParams, nullptr);
    if (err != ERROR_SUCCESS)
    {
        CloseHandle(hVdisk);
        return ErrorInfo::fromWin32(ErrorCode::DiskAccessDenied, err, "AttachVirtualDisk failed");
    }

    // Get the physical path (e.g. \\.\PhysicalDrive3)
    wchar_t physPath[512] = {};
    DWORD pathSize = sizeof(physPath);
    GetVirtualDiskPhysicalPath(hVdisk, &pathSize, physPath);

    CloseHandle(hVdisk);

    VirtualDiskInfo info;
    info.filePath = filePath;
    info.format = fmt;
    info.isMounted = true;
    info.physicalDrivePath = physPath;

    return info;
}

// ============================================================================
// Unmount
// ============================================================================

Result<void> VirtualDisk::unmount(const std::wstring& filePath)
{
    VirtualDiskFormat fmt = detectFormat(filePath);
    auto mask = VIRTUAL_DISK_ACCESS_ALL;
    auto handleResult = openVirtDiskHandle(filePath, mask, OPEN_VIRTUAL_DISK_FLAG_NONE);
    if (handleResult.isError())
        return handleResult.error();

    HANDLE hVdisk = handleResult.value();
    DWORD err = DetachVirtualDisk(hVdisk, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    CloseHandle(hVdisk);

    if (err != ERROR_SUCCESS && err != ERROR_NOT_FOUND)
        return ErrorInfo::fromWin32(ErrorCode::DiskAccessDenied, err, "DetachVirtualDisk failed");

    return Result<void>::ok();
}

void VirtualDisk::unmountAll()
{
    // No convenient Windows API to enumerate all attached VDs
    // This is a best-effort placeholder
}

// ============================================================================
// Query info
// ============================================================================

Result<VirtualDiskInfo> VirtualDisk::queryInfo(const std::wstring& filePath)
{
    VirtualDiskFormat fmt = detectFormat(filePath);
    if (fmt == VirtualDiskFormat::VMDK || fmt == VirtualDiskFormat::QCOW2 || fmt == VirtualDiskFormat::RAW)
    {
        // For non-VirtDisk formats, just stat the file
        VirtualDiskInfo info;
        info.filePath = filePath;
        info.format   = fmt;
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &attr))
        {
            info.physicalSizeBytes = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
            info.virtualSizeBytes  = info.physicalSizeBytes; // Best estimate without parsing
        }
        return info;
    }

    VIRTUAL_STORAGE_TYPE storageType = {};
    storageType.DeviceId = (fmt == VirtualDiskFormat::VHD)
                           ? VIRTUAL_STORAGE_TYPE_DEVICE_VHD
                           : VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storageType.VendorId = kVendorMicrosoft;

    OPEN_VIRTUAL_DISK_PARAMETERS openParams = {};
    openParams.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    openParams.Version2.GetInfoOnly = TRUE;

    HANDLE hVdisk = INVALID_HANDLE_VALUE;
    DWORD err = OpenVirtualDisk(&storageType, filePath.c_str(),
                                VIRTUAL_DISK_ACCESS_GET_INFO,
                                OPEN_VIRTUAL_DISK_FLAG_NONE, &openParams, &hVdisk);
    if (err != ERROR_SUCCESS)
        return ErrorInfo::fromWin32(ErrorCode::DiskAccessDenied, err, "OpenVirtualDisk (info) failed");

    // Query size
    GET_VIRTUAL_DISK_INFO sizeInfo = {};
    sizeInfo.Version = GET_VIRTUAL_DISK_INFO_SIZE;
    DWORD sizeInfoSize = sizeof(sizeInfo);
    DWORD err2 = GetVirtualDiskInformation(hVdisk, &sizeInfoSize, &sizeInfo, nullptr);

    VirtualDiskInfo info;
    info.filePath = filePath;
    info.format   = fmt;
    if (err2 == ERROR_SUCCESS)
    {
        info.virtualSizeBytes  = sizeInfo.Size.VirtualSize;
        info.physicalSizeBytes = sizeInfo.Size.PhysicalSize;
    }

    // Query provider subtype (fixed vs dynamic)
    GET_VIRTUAL_DISK_INFO typeInfo = {};
    typeInfo.Version = GET_VIRTUAL_DISK_INFO_PROVIDER_SUBTYPE;
    DWORD typeInfoSize = sizeof(typeInfo);
    if (GetVirtualDiskInformation(hVdisk, &typeInfoSize, &typeInfo, nullptr) == ERROR_SUCCESS)
        info.isDynamic = (typeInfo.ProviderSubtype != 2); // 2 = fixed

    CloseHandle(hVdisk);
    return info;
}

// ============================================================================
// Create
// ============================================================================

Result<void> VirtualDisk::create(const VirtualDiskCreateParams& params, VDiskProgress progress)
{
    auto report = [&](const std::string& s, int p) { if (progress) progress(s, p); };

    if (params.format == VirtualDiskFormat::VMDK || params.format == VirtualDiskFormat::QCOW2)
    {
        // Use qemu-img for VMDK/QCOW2
        if (!qemuImgAvailable())
            return ErrorInfo::fromCode(ErrorCode::NotImplemented,
                "qemu-img not found. Install QEMU and ensure qemu-img.exe is on PATH.");

        report("Creating " + std::string(formatName(params.format)) + " image via qemu-img...", 10);

        std::wstring fmtStr = (params.format == VirtualDiskFormat::VMDK) ? L"vmdk" : L"qcow2";
        std::wstring sizeStr = std::to_wstring(params.sizeBytes);

        // qemu-img create -f vmdk output.vmdk <size>
        std::wstring cmd = L"qemu-img create -f " + fmtStr + L" \"" +
                           params.filePath + L"\" " + sizeStr;

        STARTUPINFOW si = {}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return ErrorInfo::fromWin32(ErrorCode::FormatFailed, GetLastError(), "Failed to launch qemu-img");

        WaitForSingleObject(pi.hProcess, 60000);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

        if (exitCode != 0)
            return ErrorInfo::fromCode(ErrorCode::FormatFailed, "qemu-img create failed");

        report("Done.", 100);
        return Result<void>::ok();
    }

    // VHD or VHDX via VirtDisk API
    report("Creating " + std::string(formatName(params.format)) + "...", 5);

    VIRTUAL_STORAGE_TYPE storageType = {};
    storageType.VendorId = kVendorMicrosoft;
    storageType.DeviceId = (params.format == VirtualDiskFormat::VHD)
                           ? VIRTUAL_STORAGE_TYPE_DEVICE_VHD
                           : VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;

    CREATE_VIRTUAL_DISK_PARAMETERS createParams = {};
    createParams.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    createParams.Version2.MaximumSize        = params.sizeBytes;
    createParams.Version2.BlockSizeInBytes   = params.blockSizeBytes;
    createParams.Version2.SectorSizeInBytes  = params.sectorSize;

    CREATE_VIRTUAL_DISK_FLAG createFlags = params.dynamic
        ? CREATE_VIRTUAL_DISK_FLAG_NONE
        : CREATE_VIRTUAL_DISK_FLAG_FULL_PHYSICAL_ALLOCATION;

    HANDLE hVdisk = INVALID_HANDLE_VALUE;
    DWORD err = CreateVirtualDisk(&storageType,
                                   params.filePath.c_str(),
                                   VIRTUAL_DISK_ACCESS_NONE,
                                   nullptr,
                                   createFlags,
                                   0,
                                   &createParams,
                                   nullptr,
                                   &hVdisk);
    if (err != ERROR_SUCCESS)
        return ErrorInfo::fromWin32(ErrorCode::FormatFailed, err, "CreateVirtualDisk failed");

    CloseHandle(hVdisk);
    report("Done.", 100);
    return Result<void>::ok();
}

// ============================================================================
// Capture (physical disk → image)
// ============================================================================

Result<void> VirtualDisk::captureFromDisk(DiskId sourceDiskId,
                                           const std::wstring& outputPath,
                                           VirtualDiskFormat format,
                                           VDiskProgress progress)
{
    auto report = [&](const std::string& s, int p) { if (progress) progress(s, p); };

    auto diskInfoResult = DiskEnumerator::getDiskInfo(sourceDiskId);
    if (diskInfoResult.isError()) return diskInfoResult.error();
    const auto& di = diskInfoResult.value();
    uint64_t diskSize = di.sizeBytes;

    if (format == VirtualDiskFormat::VHD || format == VirtualDiskFormat::VHDX)
    {
        // Step 1: create a raw image first, then convert if needed
        // For simplicity: capture as RAW, then if VHDX is requested, create VHDX and raw-copy into it
        report("Creating virtual disk container...", 5);

        if (format == VirtualDiskFormat::VHDX || format == VirtualDiskFormat::VHD)
        {
            VirtualDiskCreateParams cp;
            cp.filePath   = outputPath;
            cp.format     = format;
            cp.sizeBytes  = diskSize;
            cp.dynamic    = false; // fixed = exact size for capture
            cp.sectorSize = di.sectorSize > 0 ? di.sectorSize : 512;

            auto createResult = create(cp, nullptr);
            if (createResult.isError()) return createResult;

            // Mount the new VHDX
            report("Mounting virtual disk for writing...", 10);
            auto mountResult = mount(outputPath, false);
            if (mountResult.isError()) return mountResult.error();

            std::wstring vdiskPhysPath = mountResult.value().physicalDrivePath;

            // Open source disk
            std::wstring srcPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(sourceDiskId);
            HANDLE hSrc = CreateFileW(srcPath.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (hSrc == INVALID_HANDLE_VALUE)
            {
                unmount(outputPath);
                return ErrorInfo::fromWin32(ErrorCode::DiskAccessDenied, GetLastError(), "Cannot open source disk");
            }

            // Allow reading past mounted volume boundaries on the source disk
            DWORD ioBytes = 0;
            DeviceIoControl(hSrc, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &ioBytes, nullptr);

            // Open target (mounted vdisk)
            HANDLE hDst = CreateFileW(vdiskPhysPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (hDst == INVALID_HANDLE_VALUE)
            {
                CloseHandle(hSrc);
                unmount(outputPath);
                return ErrorInfo::fromWin32(ErrorCode::DiskAccessDenied, GetLastError(), "Cannot open virtual disk for writing");
            }

            // Prepare the target: allow extended I/O, lock and dismount
            DeviceIoControl(hDst, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &ioBytes, nullptr);
            DeviceIoControl(hDst, FSCTL_LOCK_VOLUME,            nullptr, 0, nullptr, 0, &ioBytes, nullptr);
            DeviceIoControl(hDst, FSCTL_DISMOUNT_VOLUME,        nullptr, 0, nullptr, 0, &ioBytes, nullptr);

            constexpr uint32_t kChunk = 32 * 1024 * 1024; // 32 MB
            auto* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!buf)
            {
                CloseHandle(hSrc);
                CloseHandle(hDst);
                unmount(outputPath);
                return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Failed to allocate 32 MB I/O buffer");
            }

            uint64_t totalCopied = 0;
            DWORD n = 0;
            bool writeError = false;

            report("Capturing disk to virtual image...", 15);
            while (totalCopied < diskSize)
            {
                DWORD toRead = static_cast<DWORD>(std::min<uint64_t>(kChunk, diskSize - totalCopied));
                if (!ReadFile(hSrc, buf, toRead, &n, nullptr) || n == 0) break;

                // Write with retry — re-seek and retry up to 3 times on failure
                DWORD written = 0;
                bool ok = false;
                for (int attempt = 0; attempt < 3; ++attempt)
                {
                    if (WriteFile(hDst, buf, n, &written, nullptr) && written == n)
                    {
                        ok = true;
                        break;
                    }
                    LARGE_INTEGER seekPos;
                    seekPos.QuadPart = static_cast<LONGLONG>(totalCopied);
                    SetFilePointerEx(hDst, seekPos, nullptr, FILE_BEGIN);
                }
                if (!ok) { writeError = true; break; }

                totalCopied += n;
                int pct = 15 + static_cast<int>((totalCopied * 80) / diskSize);
                report("Copying " + std::to_string(totalCopied / (1024*1024)) + " MB / " +
                       std::to_string(diskSize / (1024*1024)) + " MB...", pct);
            }

            FlushFileBuffers(hDst);

            CloseHandle(hSrc);
            CloseHandle(hDst);
            VirtualFree(buf, 0, MEM_RELEASE);

            report("Unmounting virtual disk...", 97);
            unmount(outputPath);

            if (writeError)
                return ErrorInfo::fromCode(ErrorCode::DiskWriteError, "Capture failed — write error after 3 retry attempts");
            if (totalCopied < diskSize)
                return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Capture incomplete — read error on source disk");

            report("Done.", 100);
            return Result<void>::ok();
        }
    }

    // RAW format: straight sector copy
    report("Capturing disk as raw image...", 5);
    std::wstring srcPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(sourceDiskId);
    HANDLE hSrc = CreateFileW(srcPath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hSrc == INVALID_HANDLE_VALUE)
        return ErrorInfo::fromWin32(ErrorCode::DiskAccessDenied, GetLastError(), "Cannot open source disk");

    // Allow reading past mounted volume boundaries on the source disk
    DWORD ioBytes = 0;
    DeviceIoControl(hSrc, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &ioBytes, nullptr);

    HANDLE hOut = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hOut == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hSrc);
        return ErrorInfo::fromWin32(ErrorCode::FileCreateFailed, GetLastError(), "Cannot create output file");
    }

    constexpr uint32_t kChunk = 32 * 1024 * 1024; // 32 MB
    auto* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf)
    {
        CloseHandle(hSrc);
        CloseHandle(hOut);
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Failed to allocate 32 MB I/O buffer");
    }

    uint64_t totalCopied = 0;
    DWORD n = 0;
    bool writeError = false;

    while (totalCopied < diskSize)
    {
        DWORD toRead = static_cast<DWORD>(std::min<uint64_t>(kChunk, diskSize - totalCopied));
        if (!ReadFile(hSrc, buf, toRead, &n, nullptr) || n == 0) break;

        // Write with retry — re-seek and retry up to 3 times on failure
        DWORD written = 0;
        bool ok = false;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (WriteFile(hOut, buf, n, &written, nullptr) && written == n)
            {
                ok = true;
                break;
            }
            LARGE_INTEGER seekPos;
            seekPos.QuadPart = static_cast<LONGLONG>(totalCopied);
            SetFilePointerEx(hOut, seekPos, nullptr, FILE_BEGIN);
        }
        if (!ok) { writeError = true; break; }

        totalCopied += n;
        int pct = 5 + static_cast<int>((totalCopied * 90) / diskSize);
        report("Copying " + std::to_string(totalCopied / (1024*1024)) + " MB / " +
               std::to_string(diskSize / (1024*1024)) + " MB...", pct);
    }

    FlushFileBuffers(hOut);

    CloseHandle(hSrc);
    CloseHandle(hOut);
    VirtualFree(buf, 0, MEM_RELEASE);

    if (writeError)
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError, "Capture failed — write error after 3 retry attempts");

    report("Done.", 100);
    return Result<void>::ok();
}

// ============================================================================
// Flash to disk
// ============================================================================

Result<void> VirtualDisk::flashToDisk(const std::wstring& imagePath,
                                       DiskId targetDiskId,
                                       VDiskProgress progress)
{
    auto report = [&](const std::string& s, int p) { if (progress) progress(s, p); };

    VirtualDiskFormat fmt = detectFormat(imagePath);
    bool mounted = false;
    std::wstring sourcePath;

    if (fmt == VirtualDiskFormat::VHD || fmt == VirtualDiskFormat::VHDX)
    {
        report("Mounting virtual disk...", 2);
        auto mountResult = mount(imagePath, true);
        if (mountResult.isError()) return mountResult.error();
        sourcePath = mountResult.value().physicalDrivePath;
        mounted = true;
    }
    else
    {
        // RAW / VMDK / QCOW2: treat as raw file
        sourcePath = imagePath;
    }

    report("Opening source...", 5);
    HANDLE hSrc = CreateFileW(sourcePath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hSrc == INVALID_HANDLE_VALUE)
    {
        if (mounted) unmount(imagePath);
        return ErrorInfo::fromWin32(ErrorCode::DiskAccessDenied, GetLastError(), "Cannot open image for reading");
    }

    // Get source size
    LARGE_INTEGER fileSize{};
    GetFileSizeEx(hSrc, &fileSize);
    uint64_t totalBytes = static_cast<uint64_t>(fileSize.QuadPart);

    // For mounted VHD, use IOCTL
    if (totalBytes == 0)
    {
        GET_LENGTH_INFORMATION lenInfo{};
        DWORD ret = 0;
        DeviceIoControl(hSrc, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                        &lenInfo, sizeof(lenInfo), &ret, nullptr);
        totalBytes = static_cast<uint64_t>(lenInfo.Length.QuadPart);
    }

    std::wstring dstPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(targetDiskId);
    HANDLE hDst = CreateFileW(dstPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (hDst == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hSrc);
        if (mounted) unmount(imagePath);
        return ErrorInfo::fromWin32(ErrorCode::DiskAccessDenied, GetLastError(), "Cannot open target disk");
    }

    // Prepare the target disk: allow writes past mounted volume boundaries,
    // lock and dismount so Windows doesn't interfere during the flash.
    DWORD ioBytes = 0;
    DeviceIoControl(hDst, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &ioBytes, nullptr);
    DeviceIoControl(hDst, FSCTL_LOCK_VOLUME,            nullptr, 0, nullptr, 0, &ioBytes, nullptr);
    DeviceIoControl(hDst, FSCTL_DISMOUNT_VOLUME,        nullptr, 0, nullptr, 0, &ioBytes, nullptr);

    constexpr uint32_t kChunk = 32 * 1024 * 1024; // 32 MB
    auto* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf)
    {
        CloseHandle(hSrc);
        CloseHandle(hDst);
        if (mounted) unmount(imagePath);
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Failed to allocate 32 MB I/O buffer");
    }

    uint64_t totalWritten = 0;
    DWORD n = 0;
    bool writeError = false;

    report("Flashing...", 10);
    while (true)
    {
        DWORD toRead = static_cast<DWORD>(
            totalBytes > 0 ? std::min<uint64_t>(kChunk, totalBytes - totalWritten) : kChunk);
        if (toRead == 0) break;
        if (!ReadFile(hSrc, buf, toRead, &n, nullptr) || n == 0) break;

        // Write with retry logic — re-seek and retry up to 3 times on failure
        DWORD written = 0;
        bool ok = false;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (WriteFile(hDst, buf, n, &written, nullptr) && written == n)
            {
                ok = true;
                break;
            }
            // Re-seek to the same position for retry
            LARGE_INTEGER seekPos;
            seekPos.QuadPart = static_cast<LONGLONG>(totalWritten);
            SetFilePointerEx(hDst, seekPos, nullptr, FILE_BEGIN);
        }
        if (!ok)
        {
            writeError = true;
            break;
        }

        totalWritten += n;
        int pct = totalBytes > 0
            ? 10 + static_cast<int>((totalWritten * 85) / totalBytes)
            : 50;
        report("Writing " + std::to_string(totalWritten / (1024*1024)) + " MB...", pct);
    }

    FlushFileBuffers(hDst);

    CloseHandle(hSrc);
    CloseHandle(hDst);
    VirtualFree(buf, 0, MEM_RELEASE);
    if (mounted) unmount(imagePath);

    if (writeError)
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError, "Write failed after 3 retry attempts");

    report("Done.", 100);
    return Result<void>::ok();
}

// ============================================================================
// Convert
// ============================================================================

Result<void> VirtualDisk::convert(const std::wstring& inputPath,
                                   const std::wstring& outputPath,
                                   VirtualDiskFormat targetFormat,
                                   VDiskProgress progress)
{
    auto report = [&](const std::string& s, int p) { if (progress) progress(s, p); };

    if (!qemuImgAvailable())
        return ErrorInfo::fromCode(ErrorCode::NotImplemented,
            "qemu-img not found. Install QEMU and ensure qemu-img.exe is on PATH "
            "to convert between VMDK, QCOW2, VHD, and VHDX.");

    const wchar_t* fmtStr = L"raw";
    switch (targetFormat)
    {
    case VirtualDiskFormat::VHD:   fmtStr = L"vpc"; break;  // qemu-img uses "vpc" for VHD
    case VirtualDiskFormat::VHDX:  fmtStr = L"vhdx"; break;
    case VirtualDiskFormat::VMDK:  fmtStr = L"vmdk"; break;
    case VirtualDiskFormat::QCOW2: fmtStr = L"qcow2"; break;
    case VirtualDiskFormat::RAW:   fmtStr = L"raw"; break;
    }

    report("Converting to " + std::string(formatName(targetFormat)) + "...", 5);

    std::wstring cmd = L"qemu-img convert -p -O " + std::wstring(fmtStr) +
                       L" \"" + inputPath + L"\" \"" + outputPath + L"\"";

    STARTUPINFOW si = {}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return ErrorInfo::fromWin32(ErrorCode::FormatFailed, GetLastError(), "Failed to launch qemu-img");

    // Wait up to 60 minutes
    WaitForSingleObject(pi.hProcess, 3600000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    if (exitCode != 0)
        return ErrorInfo::fromCode(ErrorCode::FormatFailed,
            "qemu-img convert failed (exit " + std::to_string(exitCode) + ")");

    report("Conversion complete.", 100);
    return Result<void>::ok();
}

} // namespace spw
