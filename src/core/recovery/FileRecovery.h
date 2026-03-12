#pragma once

// FileRecovery -- Recover deleted files from NTFS, FAT, ext2/3/4 partitions,
//                 and perform filesystem-independent file carving.
//
// Each filesystem-specific scanner reads the on-disk metadata structures
// (MFT for NTFS, directory entries + FAT for FAT, inodes for ext) looking
// for entries marked as deleted.  The file carver scans raw sectors looking
// for known file-type headers (magic bytes).
//
// DISCLAIMER: This code is for authorized disk utility / forensics software only.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "../common/Constants.h"
#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../disk/RawDiskHandle.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spw
{

// Describes a recoverable file found on the disk
struct RecoverableFile
{
    std::string filename;        // Original name, or "carved_NNNN.ext" for carved files
    uint64_t    sizeBytes = 0;   // Original file size if known, 0 otherwise
    FilesystemType sourceFs = FilesystemType::Unknown;
    std::string extension;       // "jpg", "pdf", etc.
    double      confidence = 0.0;// 0.0 - 100.0

    // Internal metadata for recovery
    SectorOffset partitionStartLba = 0;
    uint32_t     sectorSize = SECTOR_SIZE_512;
    uint64_t     mftEntryIndex = 0;   // NTFS: MFT record number
    uint32_t     firstCluster = 0;    // FAT: first cluster number
    uint64_t     inodeNumber = 0;     // ext: inode number
    SectorOffset carvedLba = 0;       // File carving: LBA of file header

    // Data run list (for NTFS / ext recovery)
    struct DataRun
    {
        uint64_t clusterOffset = 0;  // Starting cluster/block on disk
        uint64_t clusterCount = 0;   // Number of clusters/blocks
    };
    std::vector<DataRun> dataRuns;
};

// Scan mode for file recovery
enum class FileRecoveryMode
{
    FilesystemAware, // Use filesystem metadata (MFT, FAT, inodes)
    Carving,         // Raw sector scanning for magic bytes
    Both,            // Do both passes
};

// Known file types for carving
struct CarvedFileSignature
{
    std::vector<uint8_t> header;       // Magic bytes at offset 0
    uint32_t             headerOffset; // Offset from sector start where header appears
    std::string          extension;    // File extension ("jpg", "png", etc.)
    std::string          description;  // Human-readable type name
    std::vector<uint8_t> footer;       // Optional end-of-file marker
    uint64_t             maxSize;      // Max expected file size (caps carving)
};

// Progress callback.
// Parameters: (sectorsScanned, totalSectors, filesFoundSoFar)
using FileRecoveryProgress = std::function<void(uint64_t sectorsScanned,
                                                 uint64_t totalSectors,
                                                 size_t   filesFound)>;

class FileRecovery
{
public:
    FileRecovery(RawDiskHandle& disk,
                 SectorOffset partitionStartLba,
                 SectorCount  partitionSectorCount,
                 FilesystemType fsType,
                 uint32_t sectorSize = SECTOR_SIZE_512);

    // Scan for recoverable files
    Result<std::vector<RecoverableFile>> scan(
        FileRecoveryMode mode = FileRecoveryMode::Both,
        FileRecoveryProgress progressCb = nullptr,
        std::atomic<bool>* cancelFlag = nullptr);

    // Recover a specific file to the given output path
    Result<void> recoverFile(const RecoverableFile& file,
                             const std::string& outputPath);

private:
    // Filesystem-specific scanners
    Result<std::vector<RecoverableFile>> scanNtfs(
        FileRecoveryProgress progressCb, std::atomic<bool>* cancelFlag);
    Result<std::vector<RecoverableFile>> scanFat(
        FileRecoveryProgress progressCb, std::atomic<bool>* cancelFlag);
    Result<std::vector<RecoverableFile>> scanExt(
        FileRecoveryProgress progressCb, std::atomic<bool>* cancelFlag);

    // File carver
    Result<std::vector<RecoverableFile>> scanCarving(
        FileRecoveryProgress progressCb, std::atomic<bool>* cancelFlag);

    // Recovery helpers
    Result<void> recoverNtfsFile(const RecoverableFile& file, const std::string& outputPath);
    Result<void> recoverFatFile(const RecoverableFile& file, const std::string& outputPath);
    Result<void> recoverExtFile(const RecoverableFile& file, const std::string& outputPath);
    Result<void> recoverCarvedFile(const RecoverableFile& file, const std::string& outputPath);

    // Read helper: reads bytes relative to partition start
    Result<std::vector<uint8_t>> readPartitionBytes(uint64_t offset, uint32_t size) const;

    // Get built-in carving signatures
    static std::vector<CarvedFileSignature> getDefaultSignatures();

    RawDiskHandle& m_disk;
    SectorOffset   m_partStart = 0;
    SectorCount    m_partSectors = 0;
    FilesystemType m_fsType = FilesystemType::Unknown;
    uint32_t       m_sectorSize = SECTOR_SIZE_512;
};

} // namespace spw
