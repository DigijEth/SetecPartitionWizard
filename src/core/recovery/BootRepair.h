#pragma once

// BootRepair -- Repair MBR boot code, GPT headers, NTFS/FAT boot sectors,
//               and Windows Boot Configuration Data (BCD).
//
// Every repair method validates structures before writing.  Destructive
// operations are clearly documented.
//
// DISCLAIMER: This code is for authorized disk utility software only.
//             Boot repair operations write to sector 0 and other critical
//             disk areas.  Incorrect use can render a system unbootable.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "../common/Constants.h"
#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../disk/RawDiskHandle.h"
#include "../disk/PartitionTable.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spw
{

// Which boot structures were repaired
struct BootRepairReport
{
    bool mbrRepaired      = false;
    bool gptRepaired      = false;
    bool bootSectorRepaired = false;
    bool bcdRepaired      = false;
    bool bootloaderRepaired = false;
    std::string details;          // Human-readable log
};

// Progress callback for multi-step boot repair.
// Parameters: (stepDescription, stepIndex, totalSteps)
using BootRepairProgress = std::function<void(const std::string& step,
                                               int stepIndex,
                                               int totalSteps)>;

class BootRepair
{
public:
    explicit BootRepair(RawDiskHandle& disk);

    // ---------------------------------------------------------------
    // MBR repair: write standard Windows 7+ compatible bootstrap code
    // to sector 0, preserving the partition table entries and disk
    // signature.
    // ---------------------------------------------------------------
    Result<void> repairMbr(BootRepairProgress progressCb = nullptr);

    // ---------------------------------------------------------------
    // GPT repair: rebuild primary from backup, or backup from primary.
    // direction: true = rebuild primary from backup,
    //            false = rebuild backup from primary.
    // ---------------------------------------------------------------
    Result<void> repairGpt(bool rebuildPrimaryFromBackup,
                           BootRepairProgress progressCb = nullptr);

    // ---------------------------------------------------------------
    // Boot sector repair: restore the NTFS or FAT backup boot sector.
    // partitionStartLba: LBA of the partition whose boot sector is
    //                    damaged.
    // ---------------------------------------------------------------
    Result<void> repairBootSector(SectorOffset partitionStartLba,
                                  SectorCount partitionSectorCount,
                                  BootRepairProgress progressCb = nullptr);

    // ---------------------------------------------------------------
    // BCD repair: invoke bcdedit.exe /rebuildbcd, or create a minimal
    // BCD store on the given EFI System Partition volume letter.
    // ---------------------------------------------------------------
    Result<void> repairBcd(wchar_t espVolumeLetter,
                           BootRepairProgress progressCb = nullptr);

    // ---------------------------------------------------------------
    // Bootloader repair: copy bootmgr and create/repair
    // EFI\Microsoft\Boot on the EFI System Partition.
    // windowsVolumeLetter: the drive letter of the Windows install.
    // ---------------------------------------------------------------
    Result<void> repairBootloader(wchar_t espVolumeLetter,
                                  wchar_t windowsVolumeLetter,
                                  BootRepairProgress progressCb = nullptr);

    // ---------------------------------------------------------------
    // Full automatic repair: detects disk type and runs all applicable
    // repair steps.
    // ---------------------------------------------------------------
    Result<BootRepairReport> autoRepair(wchar_t espVolumeLetter = 0,
                                         wchar_t windowsVolumeLetter = 0,
                                         BootRepairProgress progressCb = nullptr);

private:
    // Validate an MBR sector before accepting it
    bool validateMbr(const std::vector<uint8_t>& sector) const;

    // Validate a GPT header before accepting it
    bool validateGptHeader(const std::vector<uint8_t>& headerSector) const;

    // Get standard Windows MBR bootstrap code (446 bytes)
    static std::vector<uint8_t> getStandardMbrBootCode();

    RawDiskHandle& m_disk;
    DiskGeometryInfo m_geometry = {};
};

} // namespace spw
