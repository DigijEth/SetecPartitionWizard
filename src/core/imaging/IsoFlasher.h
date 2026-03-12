#pragma once

// IsoFlasher — Flash ISO/IMG files to USB drives and SD cards.
// Supports hybrid ISO dd-write, non-hybrid ISO with FAT32 extraction,
// UEFI boot detection, and basic ISO9660 filesystem parsing.
// DISCLAIMER: This code is for authorized disk utility software only.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../disk/RawDiskHandle.h"
#include "Checksums.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spw
{

// Progress info for flashing
struct FlashProgress
{
    uint64_t bytesWritten = 0;
    uint64_t totalBytes = 0;
    double speedBytesPerSec = 0.0;
    double etaSeconds = 0.0;
    double percentComplete = 0.0;

    enum class Phase
    {
        Preparing,
        Flashing,
        Verifying,
        Complete,
        Failed,
    };
    Phase phase = Phase::Preparing;
};

using FlashProgressCallback =
    std::function<bool(const FlashProgress& progress)>;

// ---------------------------------------------------------------------------
// ISO9660 on-disk structures for reading ISO contents
// All offsets per ECMA-119 / ISO 9660 specification.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

// ISO 9660 Volume Descriptor (2048-byte sectors starting at LBA 16)
struct Iso9660VolumeDescriptor
{
    uint8_t type;               // 1 = Primary, 2 = Supplementary, 255 = Terminator
    char standardId[5];         // "CD001"
    uint8_t version;            // 1
    uint8_t unused1;
    char systemId[32];
    char volumeId[32];
    uint8_t unused2[8];
    uint32_t volumeSpaceSizeLe; // Little-endian total sectors
    uint32_t volumeSpaceSizeBe; // Big-endian total sectors
    uint8_t unused3[32];
    uint16_t volumeSetSizeLe;
    uint16_t volumeSetSizeBe;
    uint16_t volumeSeqNumLe;
    uint16_t volumeSeqNumBe;
    uint16_t logicalBlockSizeLe;  // Usually 2048
    uint16_t logicalBlockSizeBe;
    uint32_t pathTableSizeLe;
    uint32_t pathTableSizeBe;
    uint32_t pathTableLocLe;      // LBA of LE path table
    uint32_t pathTableOptLocLe;
    uint32_t pathTableLocBe;      // Big-endian path table LBA
    uint32_t pathTableOptLocBe;
    uint8_t rootDirRecord[34];    // Root directory record
    // ... rest of fields to 2048 bytes (we only need the above)
};

// ISO 9660 Directory Record (variable length)
struct Iso9660DirRecord
{
    uint8_t recordLength;
    uint8_t extAttrRecordLength;
    uint32_t extentLbaLe;
    uint32_t extentLbaBe;
    uint32_t dataSizeLe;
    uint32_t dataSizeBe;
    uint8_t recordingDate[7];
    uint8_t fileFlags;      // Bit 1: directory
    uint8_t fileUnitSize;
    uint8_t interleaveGap;
    uint16_t volumeSeqNumLe;
    uint16_t volumeSeqNumBe;
    uint8_t fileIdLength;
    // fileId follows (variable length, then padding byte if fileIdLength is even)
};

#pragma pack(pop)

// Parsed file entry from ISO9660
struct IsoFileEntry
{
    std::string name;
    uint32_t lba = 0;          // Start sector in the ISO
    uint32_t size = 0;         // File size in bytes
    bool isDirectory = false;
};

// Flashing configuration
struct FlashConfig
{
    // Input file (.iso or .img)
    std::wstring inputFilePath;

    // Target disk (must be removable unless forceFixed is true)
    DiskId targetDiskId = -1;

    // Safety: refuse to flash to fixed (non-removable) disks unless forced
    bool forceFixed = false;

    // Verify after flash by reading back and comparing hash
    bool verifyAfterFlash = true;

    // Volume letters on target to lock/dismount
    std::vector<wchar_t> targetVolumeLetters;

    // I/O buffer size
    uint32_t bufferSize = 4 * 1024 * 1024;
};

class IsoFlasher
{
public:
    IsoFlasher() = default;
    ~IsoFlasher() = default;

    IsoFlasher(const IsoFlasher&) = delete;
    IsoFlasher& operator=(const IsoFlasher&) = delete;

    // Flash an ISO or IMG file to a disk. Blocks until complete or canceled.
    Result<void> flash(const FlashConfig& config,
                       FlashProgressCallback progressCb = nullptr);

    void requestCancel();
    bool isCancelRequested() const;

    // Utility: check if an ISO file is hybrid (has valid MBR at offset 0)
    static Result<bool> isHybridIso(const std::wstring& isoPath);

    // Utility: check if an ISO contains UEFI boot files
    static Result<bool> hasUefiBoot(const std::wstring& isoPath);

    // Utility: list files in an ISO9660 image (top-level directory)
    static Result<std::vector<IsoFileEntry>> listIsoContents(
        const std::wstring& isoPath);

    // Utility: read a file from an ISO9660 image by its path
    static Result<std::vector<uint8_t>> readIsoFile(
        const std::wstring& isoPath,
        const std::string& filePath);

private:
    std::atomic<bool> m_cancelRequested{false};

    // Flash raw IMG file (dd-style write)
    Result<void> flashRawImage(
        HANDLE hFile, uint64_t fileSize,
        RawDiskHandle& dstDisk, uint32_t dstSectorSize,
        uint32_t bufferSize, bool verify,
        FlashProgressCallback progressCb);

    // Flash hybrid ISO (dd-style write, same as raw)
    Result<void> flashHybridIso(
        HANDLE hFile, uint64_t fileSize,
        RawDiskHandle& dstDisk, uint32_t dstSectorSize,
        uint32_t bufferSize, bool verify,
        FlashProgressCallback progressCb);

    // Flash non-hybrid ISO: create FAT32 partition and copy files
    Result<void> flashNonHybridIso(
        HANDLE hFile, uint64_t fileSize,
        const std::wstring& isoPath,
        RawDiskHandle& dstDisk, uint32_t dstSectorSize,
        uint32_t bufferSize,
        FlashProgressCallback progressCb);

    // Verify flash by re-reading and comparing SHA-256
    Result<void> verifyFlash(
        HANDLE hFile, uint64_t fileSize,
        RawDiskHandle& dstDisk, uint32_t dstSectorSize,
        uint32_t bufferSize,
        FlashProgressCallback progressCb);

    // Lock/dismount target volumes
    Result<std::vector<HANDLE>> lockTargetVolumes(
        const std::vector<wchar_t>& volumeLetters);
    void unlockVolumes(std::vector<HANDLE>& handles);

    // Parse ISO9660 Primary Volume Descriptor
    static Result<Iso9660VolumeDescriptor> readPVD(HANDLE hFile);

    // Parse directory records from an ISO9660 directory extent
    static Result<std::vector<IsoFileEntry>> parseDirectoryExtent(
        HANDLE hFile, uint32_t extentLba, uint32_t extentSize);

    // Find a file in the ISO by walking directories (supports paths like /EFI/BOOT/BOOTX64.EFI)
    static Result<IsoFileEntry> findFileInIso(
        HANDLE hFile, const std::string& path);
};

} // namespace spw
