#pragma once

// ImageCreator — Creates disk/partition images in raw (.img) or compressed SPW format.
// SPW format: [SPWIMG01 magic][header][chunk table][LZNT1-compressed 4MB chunks]
// Each chunk is independently compressed for random-access decompression.
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

// Image format to create
enum class ImageFormat
{
    // Raw byte-for-byte copy (.img)
    Raw,

    // Compressed SPW format with metadata header (.spw)
    SPW,
};

// Progress info for image creation
struct ImageCreateProgress
{
    uint64_t bytesProcessed = 0;
    uint64_t totalBytes = 0;
    uint64_t compressedBytes = 0;  // For SPW format: total compressed output so far
    double speedBytesPerSec = 0.0;
    double etaSeconds = 0.0;
    double percentComplete = 0.0;
    double compressionRatio = 0.0; // e.g. 2.5 means 2.5:1 compression
};

using ImageCreateProgressCallback =
    std::function<bool(const ImageCreateProgress& progress)>;

// ---------------------------------------------------------------------------
// SPW image file on-disk structures.
// All multi-byte fields are little-endian.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

// File header — immediately follows the 8-byte magic
struct SpwImageHeader
{
    uint8_t magic[8];           // "SPWIMG01"
    uint32_t headerSize;        // Size of this header struct in bytes
    uint32_t version;           // Format version (currently 1)

    // Source disk metadata
    char diskModel[64];         // UTF-8, null-terminated
    char diskSerial[64];        // UTF-8, null-terminated
    uint64_t sourceDiskSize;    // Total bytes on source disk
    uint32_t sourceSectorSize;  // Bytes per sector (e.g. 512, 4096)
    uint32_t partitionTableType;// PartitionTableType enum value

    // Image metadata
    uint64_t imageDataSize;     // Total uncompressed data size
    uint32_t chunkSize;         // Uncompressed chunk size (typically 4 MiB)
    uint32_t chunkCount;        // Number of chunks in the image
    uint32_t compressionType;   // 0=none, 1=LZNT1
    uint32_t compressionLevel;  // 0 for LZNT1 (no level control)

    // Timestamps
    uint64_t creationTimestamp; // Windows FILETIME (100-ns intervals since 1601)

    // Integrity
    uint8_t sha256[32];         // SHA-256 of uncompressed data

    // Sparse support: if non-zero, a bitmap follows the chunk table
    // indicating which chunks contain only zeros (skipped in file).
    uint32_t sparseChunkCount;  // Number of chunks that are all-zeros (not stored)
    uint32_t flags;             // Bit 0: sparse image

    uint8_t reserved[128];      // Future expansion
};

// Chunk table entry — one per chunk, follows the header
struct SpwChunkEntry
{
    uint64_t fileOffset;        // Byte offset in the image file where compressed data starts
    uint32_t compressedSize;    // Size of compressed data (0 if chunk is sparse/all-zeros)
    uint32_t uncompressedSize;  // Original uncompressed size
    uint32_t crc32;             // CRC32 of uncompressed data for quick validation
    uint32_t flags;             // Bit 0: sparse (all zeros, not stored)
};

#pragma pack(pop)

static_assert(sizeof(SpwImageHeader) <= 512, "SPW header must fit in one sector");

// Configuration for image creation
struct ImageCreateConfig
{
    // Source
    DiskId sourceDiskId = -1;
    uint64_t sourceOffsetBytes = 0;   // Partition offset (0 for whole disk)
    uint64_t sourceLengthBytes = 0;   // 0 = entire disk

    // Output
    std::wstring outputFilePath;
    ImageFormat format = ImageFormat::Raw;

    // SPW options
    bool enableCompression = true;    // Use LZNT1 compression
    bool enableSparse = true;         // Skip all-zero chunks

    // I/O
    uint32_t chunkSize = 4 * 1024 * 1024; // 4 MiB
};

class ImageCreator
{
public:
    ImageCreator() = default;
    ~ImageCreator() = default;

    ImageCreator(const ImageCreator&) = delete;
    ImageCreator& operator=(const ImageCreator&) = delete;

    // Create an image. Blocks until complete or canceled.
    Result<void> createImage(const ImageCreateConfig& config,
                             ImageCreateProgressCallback progressCb = nullptr);

    void requestCancel();
    bool isCancelRequested() const;

private:
    std::atomic<bool> m_cancelRequested{false};

    // Create a raw .img file (dd-style byte copy)
    Result<void> createRawImage(
        RawDiskHandle& srcDisk, uint32_t sectorSize,
        uint64_t srcOffset, uint64_t length,
        const std::wstring& outputPath, uint32_t chunkSize,
        ImageCreateProgressCallback progressCb);

    // Create a compressed SPW image
    Result<void> createSpwImage(
        RawDiskHandle& srcDisk, uint32_t sectorSize,
        uint64_t srcOffset, uint64_t length,
        const ImageCreateConfig& config,
        ImageCreateProgressCallback progressCb);

    // Compress a buffer using LZNT1 via RtlCompressBuffer
    Result<std::vector<uint8_t>> compressLZNT1(
        const uint8_t* uncompressedData, size_t uncompressedSize);

    // Check if a buffer is entirely zeros
    static bool isAllZeros(const uint8_t* data, size_t length);

    // Build disk metadata strings for the SPW header
    static void populateDiskMetadata(
        SpwImageHeader& header,
        const RawDiskHandle& disk,
        const DiskGeometryInfo& geom);
};

} // namespace spw
