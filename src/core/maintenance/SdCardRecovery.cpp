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

    // Removable + size up to 2TB (zero size allowed — card may be corrupted)
    if (disk.isRemovable && disk.sizeBytes <= 2199023255552ULL)
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
        // Zero size can mean: no card inserted, OR card is so corrupted
        // that the controller can't report geometry. Try opening it anyway.
        auto diskResult = RawDiskHandle::open(diskId, DiskAccessMode::ReadOnly);
        if (diskResult.isError())
        {
            info.status = SdCardStatus::NoMedia;
            info.statusDescription = L"Card reader found but cannot access media — insert card or try again";
        }
        else
        {
            info.status = SdCardStatus::NoPartitionTable;
            info.statusDescription = L"Card found but reports zero size — likely corrupted by interrupted format. Use Fix to repair.";
        }
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

    // Aggressive brute-force: scan PhysicalDrive0..127 to catch any device
    // that SetupAPI missed — especially cards with corrupted partition tables
    // that report zero size or broken geometry after a failed format.
    for (int i = 0; i < 128; ++i)
    {
        // Skip if already found via normal enumeration
        bool found = false;
        for (const auto& card : cards)
        {
            if (card.diskId == i) { found = true; break; }
        }
        if (found)
            continue;

        // Try to open — any device that opens is a candidate
        std::wstring devPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(i);
        HANDLE h = CreateFileW(devPath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        // Query STORAGE_DEVICE_DESCRIPTOR to determine if it's removable
        STORAGE_PROPERTY_QUERY spq = {};
        spq.PropertyId = StorageDeviceProperty;
        spq.QueryType  = PropertyStandardQuery;

        uint8_t buf[1024] = {};
        DWORD ret = 0;
        bool isRemovable = false;
        uint64_t diskSize = 0;

        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                            &spq, sizeof(spq), buf, sizeof(buf), &ret, nullptr))
        {
            auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
            isRemovable = (desc->RemovableMedia != FALSE);

            // BusTypeSd = 14, BusTypeMmc = 15
            bool isSdBus = (desc->BusType == BusTypeSd || desc->BusType == BusTypeMmc);
            bool isUsb   = (desc->BusType == BusTypeUsb);

            if (!isSdBus && !isRemovable && !isUsb)
            {
                CloseHandle(h);
                continue; // Not a removable / SD type device
            }
        }

        // Get disk size — zero is OK (mid-format crash scenario)
        GET_LENGTH_INFORMATION lenInfo = {};
        DWORD lenRet = 0;
        DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                        &lenInfo, sizeof(lenInfo), &lenRet, nullptr);
        diskSize = static_cast<uint64_t>(lenInfo.Length.QuadPart);

        CloseHandle(h);

        // Skip huge disks that are clearly not SD cards (> 2 TB)
        if (diskSize > 2199023255552ULL && diskSize != 0)
            continue;

        // Include this device — it's removable and small (or zero-size due to corruption)
        SdCardInfo info;
        info.diskId = i;
        info.sizeBytes = diskSize;
        info.sectorSize = 512;

        if (diskSize == 0)
        {
            info.status = SdCardStatus::NoPartitionTable;
            info.statusDescription = L"Device found — reports zero size (likely corrupted by interrupted format)";
            info.model = L"Removable Disk " + std::to_wstring(i) + L" (size unknown — use Fix to repair)";
        }
        else
        {
            auto analysisResult = analyzeDisk(i);
            if (analysisResult.isOk())
            {
                info = analysisResult.value();
            }
            else
            {
                info.status = SdCardStatus::NoPartitionTable;
                info.statusDescription = L"Cannot read partition table — may be corrupted";
            }
            if (info.model.empty())
                info.model = L"Removable Disk " + std::to_wstring(i);
        }

        cards.push_back(std::move(info));
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

// Helper: run a command and wait for it, return stdout+stderr and exit code
static std::pair<std::wstring, DWORD> runHidden(std::wstring cmd, DWORD timeoutMs = 120000)
{
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;

    // Pipe stdout/stderr so we can capture error messages
    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    CreatePipe(&hReadPipe, &hWritePipe, &sa, 0);
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);
    si.hStdOutput = hWritePipe;
    si.hStdError  = hWritePipe;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        return { L"CreateProcess failed", (DWORD)-1 };
    }

    CloseHandle(hWritePipe);
    WaitForSingleObject(pi.hProcess, timeoutMs);

    // Read output
    char buf[4096] = {};
    DWORD nRead = 0;
    std::string output;
    while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &nRead, nullptr) && nRead > 0)
    {
        buf[nRead] = '\0';
        output += buf;
    }
    CloseHandle(hReadPipe);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    std::wstring wout(output.begin(), output.end());
    return { wout, exitCode };
}

Result<void> SdCardRecovery::formatPartition(DiskId diskId, FilesystemType fs,
                                              const std::wstring& label)
{
    std::wstring fsName;
    switch (fs)
    {
    case FilesystemType::FAT32:  fsName = L"FAT32"; break;
    case FilesystemType::ExFAT:  fsName = L"exFAT"; break;
    case FilesystemType::NTFS:   fsName = L"NTFS";  break;
    default:                     fsName = L"FAT32"; break;
    }

    // ----------------------------------------------------------------
    // Strategy 1: PowerShell Format-Volume by disk number.
    // Works without a drive letter — uses the disk index directly.
    // This is the most reliable path for freshly-partitioned cards.
    // ----------------------------------------------------------------
    {
        std::wstring labelEscaped = label;
        // Escape single quotes in the label
        size_t pos = 0;
        while ((pos = labelEscaped.find(L"'", pos)) != std::wstring::npos)
        {
            labelEscaped.replace(pos, 1, L"''");
            pos += 2;
        }

        std::wstring psCmd =
            L"Get-Disk -Number " + std::to_wstring(diskId) +
            L" | Get-Partition | Where-Object { $_.Type -ne 'Reserved' } "
            L"| Format-Volume -FileSystem " + fsName +
            L" -NewFileSystemLabel '" + labelEscaped +
            L"' -Force -Confirm:$false";

        std::wstring fullCmd = L"powershell.exe -NonInteractive -NoProfile -Command \"" + psCmd + L"\"";
        auto [out, code] = runHidden(fullCmd, 120000);

        if (code == 0)
            return Result<void>::ok();

        log::warn(("PowerShell Format-Volume failed (exit " + std::to_string(code) + "): " +
                   std::string(out.begin(), out.end())).c_str());
    }

    // ----------------------------------------------------------------
    // Strategy 2: diskpart to assign a drive letter, then format.com
    // ----------------------------------------------------------------
    {
        // Wait a moment for Windows to recognise the partition
        Sleep(3000);

        // Find a free drive letter (start from Z: down to avoid conflicts)
        wchar_t freeLetter = L'\0';
        DWORD driveMask = GetLogicalDrives();
        for (int i = 25; i >= 4; --i) // Z..E
        {
            if (!(driveMask & (1 << i)))
            {
                freeLetter = wchar_t(L'A' + i);
                break;
            }
        }

        if (freeLetter != L'\0')
        {
            // Build a diskpart script to: select disk, select first partition, assign letter
            std::wstring script =
                L"select disk " + std::to_wstring(diskId) + L"\r\n"
                L"select partition 1\r\n"
                L"assign letter=" + std::wstring(1, freeLetter) + L"\r\n"
                L"exit\r\n";

            // Write diskpart script to a temp file
            wchar_t tempDir[MAX_PATH], scriptPath[MAX_PATH];
            GetTempPathW(MAX_PATH, tempDir);
            GetTempFileNameW(tempDir, L"spw", 0, scriptPath);
            // Add .txt extension
            std::wstring scriptFile = std::wstring(scriptPath) + L".txt";
            MoveFileW(scriptPath, scriptFile.c_str());

            HANDLE hFile = CreateFileW(scriptFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                std::string scriptA(script.begin(), script.end());
                WriteFile(hFile, scriptA.c_str(), (DWORD)scriptA.size(), &written, nullptr);
                CloseHandle(hFile);

                std::wstring diskpartCmd = L"diskpart /s \"" + scriptFile + L"\"";
                auto [dpOut, dpCode] = runHidden(diskpartCmd, 30000);
                DeleteFileW(scriptFile.c_str());

                if (dpCode == 0)
                {
                    Sleep(1500);
                    std::wstring fmtCmd = L"format " + std::wstring(1, freeLetter) +
                                         L": /FS:" + fsName + L" /Q /V:" + label + L" /Y";
                    auto [fmtOut, fmtCode] = runHidden(fmtCmd, 120000);

                    if (fmtCode == 0)
                        return Result<void>::ok();

                    log::warn(("format.com failed (exit " + std::to_string(fmtCode) + ")").c_str());
                }
            }
        }
    }

    // ----------------------------------------------------------------
    // Strategy 3: Already has a drive letter from a previous attempt
    // ----------------------------------------------------------------
    {
        Sleep(2000);
        auto snapshotResult = DiskEnumerator::getSystemSnapshot();
        if (snapshotResult.isOk())
        {
            for (const auto& part : snapshotResult.value().partitions)
            {
                if (part.diskId == diskId && part.driveLetter != L'\0')
                {
                    std::wstring fmtCmd =
                        L"format " + std::wstring(1, part.driveLetter) +
                        L": /FS:" + fsName + L" /Q /V:" + label + L" /Y";
                    auto [out, code] = runHidden(fmtCmd, 120000);
                    if (code == 0)
                        return Result<void>::ok();
                }
            }
        }
    }

    return ErrorInfo::fromCode(ErrorCode::FormatFailed,
        "All format strategies failed. The partition table was reinitialized successfully, "
        "but Windows could not format the partition automatically. "
        "Open Disk Management (diskmgmt.msc), right-click the new partition on your SD card, "
        "and choose 'Format' to complete the process manually.");
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

    // Get geometry for disk size — corrupted cards sometimes report zero
    uint64_t diskSize = 0;
    uint32_t sectorSize = 512;

    auto geomResult = disk.getGeometry();
    if (geomResult.isOk())
    {
        diskSize   = geomResult.value().totalBytes;
        sectorSize = geomResult.value().bytesPerSector > 0
                     ? geomResult.value().bytesPerSector : 512;
    }

    if (diskSize == 0)
    {
        // Try IOCTL_DISK_GET_LENGTH_INFO as fallback
        HANDLE hDisk = disk.nativeHandle();
        GET_LENGTH_INFORMATION lenInfo = {};
        DWORD ret = 0;
        if (DeviceIoControl(hDisk, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                            &lenInfo, sizeof(lenInfo), &ret, nullptr))
        {
            diskSize = static_cast<uint64_t>(lenInfo.Length.QuadPart);
        }
    }

    if (diskSize == 0)
    {
        // Card is accessible but reports zero size — this is the mid-format crash scenario.
        // The controller still accepts writes even though geometry is broken.
        // Use a conservative 32 GB estimate for the IOCTL_DISK_CREATE_DISK call;
        // the card's real controller will handle the actual boundaries.
        log::warn("Card reports zero size — proceeding with IOCTL-only repair (no raw zero write)");
        // Skip cleanDisk (which writes to sectors) since we don't know the size.
        // Just reinitialize the partition table via IOCTL, then let Windows rescan.
        report("Card reports zero size — attempting IOCTL partition table reset...", 20);

        HANDLE hDisk = disk.nativeHandle();
        CREATE_DISK createDisk = {};
        createDisk.PartitionStyle = PARTITION_STYLE_MBR;
        createDisk.Mbr.Signature  = GetTickCount();
        DWORD returned = 0;
        if (!DeviceIoControl(hDisk, IOCTL_DISK_CREATE_DISK,
                             &createDisk, sizeof(createDisk),
                             nullptr, 0, &returned, nullptr))
        {
            return ErrorInfo::fromWin32(ErrorCode::DiskWriteError, GetLastError(),
                "Cannot reinitialize card — it may need to be physically reseated or the reader replaced");
        }

        DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES,
                        nullptr, 0, nullptr, 0, &returned, nullptr);
        disk.close();

        report("Partition table reset. Waiting for Windows to rescan...", 60);
        Sleep(3000);

        auto fmtResult = formatPartition(diskId, config.targetFs, config.volumeLabel);
        if (fmtResult.isError())
            return fmtResult;

        report("Done!", 100);
        return Result<void>::ok();
    }

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
