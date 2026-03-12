#include "SdCardRecovery.h"

#include "../disk/RawDiskHandle.h"
#include "../disk/DiskEnumerator.h"
#include "../common/Logging.h"

#include <windows.h>
#include <winioctl.h>

#include <cstring>
#include <cwctype>
#include <algorithm>

namespace spw
{

bool SdCardRecovery::looksLikeSdCard(const DiskInfo& disk)
{
    // SD/MMC bus type
    if (disk.interfaceType == DiskInterfaceType::MMC)
        return true;

    // Removable + small size (up to 2TB covers SDXC)
    if (disk.isRemovable && disk.sizeBytes > 0 && disk.sizeBytes <= 2199023255552ULL)
    {
        // Check model string for SD/MMC keywords
        std::wstring modelLower = disk.model;
        std::transform(modelLower.begin(), modelLower.end(), modelLower.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (modelLower.find(L"sd") != std::wstring::npos ||
            modelLower.find(L"mmc") != std::wstring::npos ||
            modelLower.find(L"sdhc") != std::wstring::npos ||
            modelLower.find(L"sdxc") != std::wstring::npos ||
            modelLower.find(L"micro") != std::wstring::npos ||
            modelLower.find(L"card") != std::wstring::npos ||
            modelLower.find(L"reader") != std::wstring::npos)
        {
            return true;
        }

        // Also match USB removable devices under ~256GB (likely USB card readers)
        if (disk.interfaceType == DiskInterfaceType::USB)
            return true;
    }

    return false;
}

Result<SdCardInfo> SdCardRecovery::analyzeDisk(DiskId diskId)
{
    SdCardInfo info;
    info.diskId = diskId;

    // Get disk info from enumerator
    auto diskInfoResult = DiskEnumerator::getDiskInfo(diskId);
    if (diskInfoResult.isError())
        return diskInfoResult.error();

    const auto& di = diskInfoResult.value();
    info.model = di.model;
    info.serialNumber = di.serialNumber;
    info.sizeBytes = di.sizeBytes;
    info.sectorSize = di.sectorSize;
    info.interfaceType = di.interfaceType;

    if (info.sizeBytes == 0)
    {
        info.status = SdCardStatus::NoMedia;
        info.statusDescription = L"Card reader detected but no media inserted";
        return info;
    }

    // Try to open the disk and read partition table
    auto diskResult = RawDiskHandle::open(diskId, DiskAccessMode::ReadOnly);
    if (diskResult.isError())
    {
        info.status = SdCardStatus::Unknown;
        info.statusDescription = L"Cannot open disk for analysis";
        return info;
    }

    auto& disk = diskResult.value();

    // Try to get drive layout
    auto layoutResult = disk.getDriveLayout();
    if (layoutResult.isError())
    {
        // No valid partition table at all
        info.status = SdCardStatus::NoPartitionTable;
        info.statusDescription = L"No valid partition table found (MBR or GPT)";
        info.hasPartitions = false;
        return info;
    }

    const auto& layout = layoutResult.value();

    // Check for valid partitions
    int validPartitions = 0;
    for (const auto& part : layout.partitions)
    {
        if (part.partitionLength > 0 && part.partitionNumber > 0)
            ++validPartitions;
    }

    if (validPartitions == 0)
    {
        info.status = SdCardStatus::CorruptPartition;
        info.statusDescription = L"Partition table exists but contains no valid entries";
        info.hasPartitions = false;
        return info;
    }

    info.hasPartitions = true;

    // Check if any partition has a drive letter (visible to Windows)
    auto snapshotResult = DiskEnumerator::getSystemSnapshot();
    if (snapshotResult.isOk())
    {
        for (const auto& part : snapshotResult.value().partitions)
        {
            if (part.diskId == diskId && part.driveLetter != L'\0')
            {
                info.hasDriveLetter = true;
                info.driveLetter = part.driveLetter;
                break;
            }
        }

        // Check if filesystem is recognized
        bool hasRecognizedFs = false;
        for (const auto& part : snapshotResult.value().partitions)
        {
            if (part.diskId == diskId &&
                part.filesystemType != FilesystemType::Unknown &&
                part.filesystemType != FilesystemType::Unallocated)
            {
                hasRecognizedFs = true;
                break;
            }
        }

        if (!hasRecognizedFs)
        {
            info.status = SdCardStatus::RawFilesystem;
            info.statusDescription = L"Partition exists but filesystem is RAW or unrecognized";
            return info;
        }
    }

    info.status = SdCardStatus::Healthy;
    info.statusDescription = L"Card is healthy";
    return info;
}

Result<std::vector<SdCardInfo>> SdCardRecovery::detectSdCards()
{
    std::vector<SdCardInfo> cards;

    auto disksResult = DiskEnumerator::enumerateDisks();
    if (disksResult.isError())
        return disksResult.error();

    for (const auto& disk : disksResult.value())
    {
        if (!looksLikeSdCard(disk))
            continue;

        auto analysisResult = analyzeDisk(disk.id);
        if (analysisResult.isOk())
            cards.push_back(std::move(analysisResult.value()));
    }

    // Also try to find disks that SetupAPI detects but enumerateDisks might
    // miss due to having zero partitions — scan PhysicalDrive0..31 directly
    for (int i = 0; i < 32; ++i)
    {
        // Skip if we already found this disk
        bool found = false;
        for (const auto& card : cards)
        {
            if (card.diskId == i)
            {
                found = true;
                break;
            }
        }
        if (found)
            continue;

        // Try to open the disk directly
        auto diskResult = RawDiskHandle::open(i, DiskAccessMode::ReadOnly);
        if (diskResult.isError())
            continue;

        auto& disk = diskResult.value();
        auto geomResult = disk.getGeometry();
        if (geomResult.isError())
            continue;

        // Check if it's removable media
        if (geomResult.value().mediaType == RemovableMedia ||
            geomResult.value().mediaType == FixedMedia)
        {
            // Only include small removable disks (likely SD cards)
            auto totalBytes = geomResult.value().totalBytes;
            if (totalBytes > 0 && totalBytes <= 2199023255552ULL)
            {
                auto analysisResult = analyzeDisk(i);
                if (analysisResult.isOk())
                {
                    auto& info = analysisResult.value();
                    if (info.model.empty())
                        info.model = L"Unknown Removable Disk";
                    cards.push_back(std::move(info));
                }
            }
        }
    }

    return cards;
}

Result<void> SdCardRecovery::cleanDisk(RawDiskHandle& disk, uint64_t diskSize)
{
    HANDLE hDisk = disk.nativeHandle();

    // First zero out the first and last 1MB to destroy any existing
    // partition tables (MBR at sector 0, GPT at sector 1 + backup at end)
    constexpr uint64_t kCleanSize = 1048576; // 1 MB
    std::vector<uint8_t> zeros(static_cast<size_t>(kCleanSize), 0);

    // Zero the beginning
    LARGE_INTEGER offset;
    offset.QuadPart = 0;
    if (!SetFilePointerEx(hDisk, offset, nullptr, FILE_BEGIN))
        return ErrorInfo::fromWin32(ErrorCode::DiskWriteError, GetLastError(),
                                    "Failed to seek to disk start");

    DWORD written = 0;
    if (!WriteFile(hDisk, zeros.data(), static_cast<DWORD>(kCleanSize), &written, nullptr))
        return ErrorInfo::fromWin32(ErrorCode::DiskWriteError, GetLastError(),
                                    "Failed to zero disk start");

    // Zero the end (backup GPT)
    if (diskSize > kCleanSize)
    {
        offset.QuadPart = static_cast<LONGLONG>(diskSize - kCleanSize);
        if (SetFilePointerEx(hDisk, offset, nullptr, FILE_BEGIN))
        {
            WriteFile(hDisk, zeros.data(), static_cast<DWORD>(kCleanSize), &written, nullptr);
            // Failure to zero end is non-fatal
        }
    }

    // Now use IOCTL_DISK_CREATE_DISK to create a fresh MBR
    CREATE_DISK createDisk;
    std::memset(&createDisk, 0, sizeof(createDisk));
    createDisk.PartitionStyle = PARTITION_STYLE_MBR;
    createDisk.Mbr.Signature = GetTickCount(); // Random-ish signature

    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDisk, IOCTL_DISK_CREATE_DISK,
                         &createDisk, sizeof(createDisk),
                         nullptr, 0, &bytesReturned, nullptr))
    {
        return ErrorInfo::fromWin32(ErrorCode::DiskWriteError, GetLastError(),
                                    "IOCTL_DISK_CREATE_DISK failed");
    }

    return Result<void>::ok();
}

Result<void> SdCardRecovery::createPartition(RawDiskHandle& disk, uint64_t diskSize,
                                              uint32_t sectorSize, FilesystemType fs)
{
    HANDLE hDisk = disk.nativeHandle();

    // Allocate buffer for DRIVE_LAYOUT_INFORMATION_EX with 4 MBR entries
    size_t bufSize = sizeof(DRIVE_LAYOUT_INFORMATION_EX)
                     + 3 * sizeof(PARTITION_INFORMATION_EX);
    std::vector<uint8_t> buf(bufSize, 0);
    auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());

    layout->PartitionStyle = PARTITION_STYLE_MBR;
    layout->PartitionCount = 4; // MBR always has 4 entries
    layout->Mbr.Signature = GetTickCount();

    // Single partition starting at 1MB offset (aligned)
    uint64_t partOffset = 1048576; // 1 MB alignment
    if (partOffset < static_cast<uint64_t>(sectorSize))
        partOffset = sectorSize;

    uint64_t partLength = diskSize - partOffset;
    // Align partition length down to sector boundary
    partLength = (partLength / sectorSize) * sectorSize;

    auto& part = layout->PartitionEntry[0];
    part.PartitionStyle = PARTITION_STYLE_MBR;
    part.StartingOffset.QuadPart = static_cast<LONGLONG>(partOffset);
    part.PartitionLength.QuadPart = static_cast<LONGLONG>(partLength);
    part.PartitionNumber = 1;
    part.RewritePartition = TRUE;

    // Set MBR partition type
    switch (fs)
    {
    case FilesystemType::FAT32:
        // FAT32 LBA
        part.Mbr.PartitionType = partLength > 4294967296ULL ? 0x0C : 0x0B;
        break;
    case FilesystemType::ExFAT:
        part.Mbr.PartitionType = 0x07; // exFAT uses type 0x07 same as NTFS
        break;
    case FilesystemType::NTFS:
        part.Mbr.PartitionType = 0x07;
        break;
    default:
        part.Mbr.PartitionType = 0x0B; // Default to FAT32
        break;
    }
    part.Mbr.BootIndicator = FALSE;
    part.Mbr.RecognizedPartition = TRUE;

    // Zero out the remaining 3 MBR entries
    for (int i = 1; i < 4; ++i)
    {
        layout->PartitionEntry[i].RewritePartition = TRUE;
    }

    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDisk, IOCTL_DISK_SET_DRIVE_LAYOUT_EX,
                         buf.data(), static_cast<DWORD>(bufSize),
                         nullptr, 0, &bytesReturned, nullptr))
    {
        return ErrorInfo::fromWin32(ErrorCode::DiskWriteError, GetLastError(),
                                    "IOCTL_DISK_SET_DRIVE_LAYOUT_EX failed");
    }

    return Result<void>::ok();
}

Result<void> SdCardRecovery::rescanDisk(RawDiskHandle& disk)
{
    HANDLE hDisk = disk.nativeHandle();
    DWORD bytesReturned = 0;

    // Tell Windows to re-read the partition table
    if (!DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES,
                         nullptr, 0, nullptr, 0, &bytesReturned, nullptr))
    {
        return ErrorInfo::fromWin32(ErrorCode::DiskWriteError, GetLastError(),
                                    "IOCTL_DISK_UPDATE_PROPERTIES failed");
    }

    return Result<void>::ok();
}

Result<void> SdCardRecovery::formatPartition(DiskId diskId, FilesystemType fs,
                                              const std::wstring& label)
{
    // Wait for Windows to assign a drive letter after partition creation
    Sleep(2000);

    // Find the drive letter for the new partition
    auto snapshotResult = DiskEnumerator::getSystemSnapshot();
    if (snapshotResult.isError())
        return snapshotResult.error();

    wchar_t driveLetter = L'\0';
    for (const auto& part : snapshotResult.value().partitions)
    {
        if (part.diskId == diskId && part.driveLetter != L'\0')
        {
            driveLetter = part.driveLetter;
            break;
        }
    }

    if (driveLetter == L'\0')
    {
        return ErrorInfo::fromCode(ErrorCode::DiskNotFound,
            "Windows did not assign a drive letter. "
            "Open Disk Management and assign a letter, then format manually.");
    }

    // Build format command
    std::wstring fsName;
    switch (fs)
    {
    case FilesystemType::FAT32:  fsName = L"FAT32"; break;
    case FilesystemType::ExFAT:  fsName = L"exFAT"; break;
    case FilesystemType::NTFS:   fsName = L"NTFS"; break;
    default:                     fsName = L"FAT32"; break;
    }

    // format X: /FS:FAT32 /Q /V:label /Y
    std::wstring cmd = L"format " + std::wstring(1, driveLetter) + L": /FS:" + fsName
                       + L" /Q /V:" + label + L" /Y";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        return ErrorInfo::fromWin32(ErrorCode::FormatFailed, GetLastError(),
                                    "Failed to launch format command");
    }

    // Wait up to 60 seconds for format to complete
    DWORD waitResult = WaitForSingleObject(pi.hProcess, 60000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (waitResult == WAIT_TIMEOUT)
        return ErrorInfo::fromCode(ErrorCode::FormatFailed, "Format command timed out");

    if (exitCode != 0)
        return ErrorInfo::fromCode(ErrorCode::FormatFailed,
            "Format command failed with exit code " + std::to_string(exitCode));

    return Result<void>::ok();
}

Result<void> SdCardRecovery::fixCard(DiskId diskId, const SdFixConfig& config,
                                      SdProgressCallback progress)
{
    auto report = [&progress](const std::string& stage, int pct) {
        if (progress)
            progress(stage, pct);
    };

    report("Opening disk...", 0);

    auto diskResult = RawDiskHandle::open(diskId, DiskAccessMode::ReadWrite);
    if (diskResult.isError())
        return diskResult.error();

    auto& disk = diskResult.value();

    // Get geometry for disk size
    auto geomResult = disk.getGeometry();
    if (geomResult.isError())
        return geomResult.error();

    uint64_t diskSize = geomResult.value().totalBytes;
    uint32_t sectorSize = geomResult.value().bytesPerSector;

    if (diskSize == 0)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Disk reports zero size - no media?");

    // Auto-select filesystem based on size if FAT32 requested on >32GB card
    FilesystemType targetFs = config.targetFs;
    if (targetFs == FilesystemType::FAT32 && diskSize > 34359738368ULL) // >32GB
        targetFs = FilesystemType::ExFAT;

    if (config.action == SdFixAction::CleanAndFormat ||
        config.action == SdFixAction::ReinitPartitionOnly)
    {
        report("Cleaning disk (zeroing partition tables)...", 10);
        auto cleanResult = cleanDisk(disk, diskSize);
        if (cleanResult.isError())
            return cleanResult;

        report("Creating partition table...", 30);
        auto partResult = createPartition(disk, diskSize, sectorSize, targetFs);
        if (partResult.isError())
            return partResult;

        report("Updating disk properties...", 50);
        auto rescanResult = rescanDisk(disk);
        if (rescanResult.isError())
        {
            log::warn(("Rescan failed (non-fatal): " + rescanResult.error().message).c_str());
        }

        // Close the disk handle before formatting so Windows can access it
        disk.close();
    }

    if (config.action == SdFixAction::CleanAndFormat ||
        config.action == SdFixAction::FormatOnly)
    {
        report("Formatting partition...", 60);
        auto fmtResult = formatPartition(diskId, targetFs, config.volumeLabel);
        if (fmtResult.isError())
            return fmtResult;
    }

    report("Done!", 100);
    return Result<void>::ok();
}

} // namespace spw
