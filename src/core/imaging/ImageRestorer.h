#pragma once

// ImageRestorer — Restores disk/partition images from raw (.img) or SPW format.
// Handles LZNT1 decompression, SHA-256 verification, and sparse chunk expansion.
// DISCLAIMER: This code is for authorized disk utility software only.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../disk/RawDiskHandle.h"
#include "ImageCreator.h" // For SpwImageHeader, SpwChunkEntry

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spw
{

// Progress info for image restoration
struct ImageRestoreProgress
{
    uint64_t bytesWritten = 0;
    uint64_t totalBytes = 0;
    double speedBytesPerSec = 0.0;
    double etaSeconds = 0.0;
    double percentComplete = 0.0;

    enum class Phase
    {
        Preparing,
        Restoring,
        Verifying,
        Complete,
        Failed,
    };
    Phase phase = Phase::Preparing;
};

using ImageRestoreProgressCallback =
    std::function<bool(const ImageRestoreProgress& progress)>;

// Configuration for image restoration
struct ImageRestoreConfig
{
    // Input image file
    std::wstring inputFilePath;

    // Destination disk
    DiskId destDiskId = -1;
    uint64_t destOffsetBytes = 0;  // Where to start writing (0 for whole disk)

    // Verify SHA-256 after restore
    bool verifyAfterRestore = true;

    // Volume letters to lock/dismount on destination before writing
    std::vector<wchar_t> destVolumeLetters;

    // I/O buffer size
    uint32_t bufferSize = 4 * 1024 * 1024;
};

// Information extracted from an SPW image header (for display before restore)
struct SpwImageInfo
{
    std::string diskModel;
    std::string diskSerial;
    uint64_t sourceDiskSize = 0;
    uint32_t sourceSectorSize = 0;
    PartitionTableType partitionTableType = PartitionTableType::Unknown;
    uint64_t imageDataSize = 0;
    uint32_t chunkCount = 0;
    uint32_t sparseChunkCount = 0;
    bool isCompressed = false;
    SHA256Hash sha256 = {};
    uint64_t creationTimestamp = 0;
};

class ImageRestorer
{
public:
    ImageRestorer() = default;
    ~ImageRestorer() = default;

    ImageRestorer(const ImageRestorer&) = delete;
    ImageRestorer& operator=(const ImageRestorer&) = delete;

    // Inspect an image file and return its metadata (without restoring)
    static Result<SpwImageInfo> inspectImage(const std::wstring& filePath);

    // Detect whether a file is raw or SPW format
    static Result<ImageFormat> detectFormat(const std::wstring& filePath);

    // Restore an image to disk. Blocks until complete or canceled.
    Result<void> restoreImage(const ImageRestoreConfig& config,
                              ImageRestoreProgressCallback progressCb = nullptr);

    void requestCancel();
    bool isCancelRequested() const;

private:
    std::atomic<bool> m_cancelRequested{false};

    // Restore raw .img
    Result<void> restoreRawImage(
        HANDLE hFile, uint64_t fileSize,
        RawDiskHandle& dstDisk, uint32_t dstSectorSize,
        uint64_t dstOffset, uint32_t bufferSize,
        ImageRestoreProgressCallback progressCb);

    // Restore SPW compressed image
    Result<void> restoreSpwImage(
        HANDLE hFile,
        const SpwImageHeader& header,
        const std::vector<SpwChunkEntry>& chunkTable,
        RawDiskHandle& dstDisk, uint32_t dstSectorSize,
        uint64_t dstOffset,
        bool verify,
        ImageRestoreProgressCallback progressCb);

    // Decompress LZNT1 data
    static Result<std::vector<uint8_t>> decompressLZNT1(
        const uint8_t* compressedData, size_t compressedSize,
        size_t uncompressedSize);

    // Lock/dismount destination volumes
    Result<std::vector<HANDLE>> lockDestinationVolumes(
        const std::vector<wchar_t>& volumeLetters);
    void unlockVolumes(std::vector<HANDLE>& handles);
};

} // namespace spw
