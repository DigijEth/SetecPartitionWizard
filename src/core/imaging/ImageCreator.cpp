#include "ImageCreator.h"

#include "../common/Constants.h"

#include <algorithm>
#include <cstring>
#include <sstream>

// RtlCompressBuffer / RtlDecompressBuffer are exported by ntdll.dll.
// We load them at runtime to avoid a hard link-time dependency.
// These functions implement LZNT1 compression, which is the same algorithm
// NTFS uses internally for file compression.
typedef NTSTATUS(WINAPI* RtlCompressBufferFn)(
    USHORT CompressionFormatAndEngine,
    PUCHAR UncompressedBuffer,
    ULONG UncompressedBufferSize,
    PUCHAR CompressedBuffer,
    ULONG CompressedBufferSize,
    ULONG UncompressedChunkSize,
    PULONG FinalCompressedSize,
    PVOID WorkSpace);

typedef NTSTATUS(WINAPI* RtlGetCompressionWorkSpaceSizeFn)(
    USHORT CompressionFormatAndEngine,
    PULONG CompressBufferWorkSpaceSize,
    PULONG CompressFragmentWorkSpaceSize);

// Compression format constants from ntifs.h
#ifndef COMPRESSION_FORMAT_LZNT1
#define COMPRESSION_FORMAT_LZNT1 0x0002
#endif
#ifndef COMPRESSION_ENGINE_STANDARD
#define COMPRESSION_ENGINE_STANDARD 0x0000
#endif
#ifndef COMPRESSION_ENGINE_MAXIMUM
#define COMPRESSION_ENGINE_MAXIMUM 0x0100
#endif

namespace spw
{

// ---------------------------------------------------------------------------
// Helper: Win32 error
// ---------------------------------------------------------------------------
static ErrorInfo makeWin32Error(ErrorCode code, const std::string& context)
{
    const DWORD lastErr = ::GetLastError();
    std::ostringstream oss;
    oss << context << " (Win32 error " << lastErr << ")";
    return ErrorInfo::fromWin32(code, lastErr, oss.str());
}

// ---------------------------------------------------------------------------
// Cancel support
// ---------------------------------------------------------------------------
void ImageCreator::requestCancel()
{
    m_cancelRequested.store(true, std::memory_order_release);
}

bool ImageCreator::isCancelRequested() const
{
    return m_cancelRequested.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Check if a buffer is entirely zeros (fast scan with 64-bit words)
// ---------------------------------------------------------------------------
bool ImageCreator::isAllZeros(const uint8_t* data, size_t length)
{
    // Check 8 bytes at a time for speed
    const size_t wordCount = length / 8;
    const uint64_t* wordPtr = reinterpret_cast<const uint64_t*>(data);

    for (size_t i = 0; i < wordCount; ++i)
    {
        if (wordPtr[i] != 0)
            return false;
    }

    // Check remaining bytes
    for (size_t i = wordCount * 8; i < length; ++i)
    {
        if (data[i] != 0)
            return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Populate SPW header with disk metadata
// ---------------------------------------------------------------------------
void ImageCreator::populateDiskMetadata(
    SpwImageHeader& header,
    const RawDiskHandle& disk,
    const DiskGeometryInfo& geom)
{
    // Zero out metadata fields
    std::memset(header.diskModel, 0, sizeof(header.diskModel));
    std::memset(header.diskSerial, 0, sizeof(header.diskSerial));

    header.sourceDiskSize = geom.totalBytes;
    header.sourceSectorSize = geom.bytesPerSector;

    // Try to get the partition table type from the drive layout
    auto layoutResult = disk.getDriveLayout();
    if (layoutResult.isOk())
    {
        header.partitionTableType =
            static_cast<uint32_t>(layoutResult.value().partitionStyle);
    }

    // Disk model and serial would ideally come from STORAGE_DEVICE_DESCRIPTOR
    // via IOCTL_STORAGE_QUERY_PROPERTY. We query it here.
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    uint8_t descBuf[1024] = {};
    DWORD bytesReturned = 0;

    BOOL ok = ::DeviceIoControl(
        disk.nativeHandle(),
        IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query),
        descBuf, sizeof(descBuf),
        &bytesReturned, nullptr);

    if (ok && bytesReturned >= sizeof(STORAGE_DEVICE_DESCRIPTOR))
    {
        const auto* desc =
            reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(descBuf);

        // VendorId and ProductId are offsets into the buffer
        if (desc->ProductIdOffset != 0 &&
            desc->ProductIdOffset < bytesReturned)
        {
            const char* productId =
                reinterpret_cast<const char*>(descBuf) + desc->ProductIdOffset;
            // strncpy is safe here because we zero-initialized diskModel
            strncpy(header.diskModel, productId, sizeof(header.diskModel) - 1);
        }

        if (desc->SerialNumberOffset != 0 &&
            desc->SerialNumberOffset < bytesReturned)
        {
            const char* serial =
                reinterpret_cast<const char*>(descBuf) + desc->SerialNumberOffset;
            strncpy(header.diskSerial, serial, sizeof(header.diskSerial) - 1);
        }
    }
}

// ---------------------------------------------------------------------------
// LZNT1 compression via ntdll.dll
// ---------------------------------------------------------------------------
Result<std::vector<uint8_t>> ImageCreator::compressLZNT1(
    const uint8_t* uncompressedData, size_t uncompressedSize)
{
    // Load ntdll.dll functions on first call
    static HMODULE hNtdll = ::GetModuleHandleW(L"ntdll.dll");
    static auto pRtlCompressBuffer =
        reinterpret_cast<RtlCompressBufferFn>(
            ::GetProcAddress(hNtdll, "RtlCompressBuffer"));
    static auto pRtlGetCompressionWorkSpaceSize =
        reinterpret_cast<RtlGetCompressionWorkSpaceSizeFn>(
            ::GetProcAddress(hNtdll, "RtlGetCompressionWorkSpaceSize"));

    if (!pRtlCompressBuffer || !pRtlGetCompressionWorkSpaceSize)
    {
        return ErrorInfo::fromCode(ErrorCode::NotImplemented,
            "LZNT1 compression functions not available in ntdll.dll");
    }

    const USHORT compressionFormat =
        COMPRESSION_FORMAT_LZNT1 | COMPRESSION_ENGINE_STANDARD;

    // Get workspace size
    ULONG workSpaceSize = 0;
    ULONG fragmentWorkSpaceSize = 0;
    NTSTATUS status = pRtlGetCompressionWorkSpaceSize(
        compressionFormat, &workSpaceSize, &fragmentWorkSpaceSize);

    if (status != 0) // STATUS_SUCCESS = 0
    {
        return ErrorInfo::fromCode(ErrorCode::Unknown,
            "RtlGetCompressionWorkSpaceSize failed");
    }

    std::vector<uint8_t> workSpace(workSpaceSize);

    // Worst case: compressed data could be slightly larger than input.
    // Allocate input size + 10% + 256 bytes for safety.
    const size_t outputBufSize = uncompressedSize + (uncompressedSize / 10) + 256;
    std::vector<uint8_t> compressedBuffer(outputBufSize);

    ULONG finalCompressedSize = 0;

    status = pRtlCompressBuffer(
        compressionFormat,
        const_cast<PUCHAR>(uncompressedData),
        static_cast<ULONG>(uncompressedSize),
        compressedBuffer.data(),
        static_cast<ULONG>(compressedBuffer.size()),
        4096, // Uncompressed chunk size parameter for LZNT1
        &finalCompressedSize,
        workSpace.data());

    if (status != 0)
    {
        return ErrorInfo::fromCode(ErrorCode::Unknown,
            "RtlCompressBuffer (LZNT1) failed");
    }

    compressedBuffer.resize(finalCompressedSize);
    return compressedBuffer;
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
Result<void> ImageCreator::createImage(
    const ImageCreateConfig& config,
    ImageCreateProgressCallback progressCb)
{
    m_cancelRequested.store(false, std::memory_order_release);

    if (config.sourceDiskId < 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Invalid source disk ID");
    }
    if (config.outputFilePath.empty())
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Output file path is empty");
    }

    // Open source disk
    auto srcResult = RawDiskHandle::open(config.sourceDiskId, DiskAccessMode::ReadOnly);
    if (srcResult.isError())
        return srcResult.error();

    auto& srcDisk = srcResult.value();

    auto geomResult = srcDisk.getGeometry();
    if (geomResult.isError())
        return geomResult.error();

    const auto& geom = geomResult.value();
    const uint32_t sectorSize = geom.bytesPerSector;

    // Determine range to image
    uint64_t srcOffset = config.sourceOffsetBytes;
    uint64_t length = config.sourceLengthBytes;

    if (length == 0)
    {
        if (srcOffset > geom.totalBytes)
        {
            return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                "Source offset exceeds disk size");
        }
        length = geom.totalBytes - srcOffset;
    }

    if (srcOffset + length > geom.totalBytes)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Source range exceeds disk size");
    }

    // Ensure offset is sector-aligned
    if (srcOffset % sectorSize != 0)
    {
        return ErrorInfo::fromCode(ErrorCode::AlignmentError,
            "Source offset must be sector-aligned");
    }

    if (config.format == ImageFormat::Raw)
    {
        return createRawImage(srcDisk, sectorSize, srcOffset, length,
                              config.outputFilePath, config.chunkSize,
                              progressCb);
    }
    else
    {
        return createSpwImage(srcDisk, sectorSize, srcOffset, length,
                              config, progressCb);
    }
}

// ---------------------------------------------------------------------------
// Raw image: dd-style byte copy to file
// ---------------------------------------------------------------------------
Result<void> ImageCreator::createRawImage(
    RawDiskHandle& srcDisk, uint32_t sectorSize,
    uint64_t srcOffset, uint64_t length,
    const std::wstring& outputPath, uint32_t chunkSize,
    ImageCreateProgressCallback progressCb)
{
    // Create output file
    HANDLE hFile = ::CreateFileW(
        outputPath.c_str(),
        GENERIC_WRITE,
        0, // No sharing during image creation
        nullptr,
        CREATE_ALWAYS,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return makeWin32Error(ErrorCode::FileCreateFailed,
            "Failed to create output image file");
    }

    // Align chunk size to sector boundary
    const uint32_t alignedChunk = (chunkSize / sectorSize) * sectorSize;
    if (alignedChunk == 0)
    {
        ::CloseHandle(hFile);
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Chunk size too small for sector alignment");
    }

    LARGE_INTEGER startTime, perfFreq;
    ::QueryPerformanceFrequency(&perfFreq);
    ::QueryPerformanceCounter(&startTime);

    uint64_t bytesRemaining = length;
    uint64_t bytesProcessed = 0;
    uint64_t srcPos = srcOffset;

    while (bytesRemaining > 0)
    {
        if (isCancelRequested())
        {
            ::CloseHandle(hFile);
            ::DeleteFileW(outputPath.c_str());
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                "Image creation canceled");
        }

        const uint64_t readBytes = std::min(
            static_cast<uint64_t>(alignedChunk), bytesRemaining);
        const SectorOffset lba = srcPos / sectorSize;
        const SectorCount sectors =
            static_cast<SectorCount>((readBytes + sectorSize - 1) / sectorSize);

        auto readResult = srcDisk.readSectors(lba, sectors, sectorSize);
        if (readResult.isError())
        {
            ::CloseHandle(hFile);
            ::DeleteFileW(outputPath.c_str());
            return readResult.error();
        }

        const auto& data = readResult.value();

        // Write only the bytes we need (may be less than sector-aligned read)
        const DWORD writeSize = static_cast<DWORD>(
            std::min(static_cast<uint64_t>(data.size()), readBytes));
        DWORD bytesWritten = 0;

        BOOL ok = ::WriteFile(hFile, data.data(), writeSize, &bytesWritten, nullptr);
        if (!ok || bytesWritten != writeSize)
        {
            ::CloseHandle(hFile);
            ::DeleteFileW(outputPath.c_str());
            return makeWin32Error(ErrorCode::ImageWriteError,
                "Failed to write to image file");
        }

        srcPos += readBytes;
        bytesProcessed += readBytes;
        bytesRemaining -= readBytes;

        if (progressCb)
        {
            ImageCreateProgress progress;
            progress.bytesProcessed = bytesProcessed;
            progress.totalBytes = length;
            progress.percentComplete =
                static_cast<double>(bytesProcessed) / static_cast<double>(length) * 100.0;

            LARGE_INTEGER now;
            ::QueryPerformanceCounter(&now);
            const double elapsed =
                static_cast<double>(now.QuadPart - startTime.QuadPart) /
                static_cast<double>(perfFreq.QuadPart);

            if (elapsed > 0.0)
            {
                progress.speedBytesPerSec =
                    static_cast<double>(bytesProcessed) / elapsed;
                if (progress.speedBytesPerSec > 0.0 && bytesProcessed < length)
                {
                    progress.etaSeconds =
                        static_cast<double>(length - bytesProcessed) /
                        progress.speedBytesPerSec;
                }
            }

            if (!progressCb(progress))
            {
                ::CloseHandle(hFile);
                ::DeleteFileW(outputPath.c_str());
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                    "Image creation canceled");
            }
        }
    }

    ::CloseHandle(hFile);
    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// SPW compressed image creation.
// File layout:
//   [SpwImageHeader]                    — fixed size
//   [SpwChunkEntry * chunkCount]        — chunk table
//   [compressed chunk data...]          — variable-size compressed blocks
//
// We write the header and chunk table as placeholders first, then write
// compressed chunks sequentially, recording offsets in the chunk table.
// Finally, we seek back and overwrite the header (with SHA-256) and
// chunk table with the actual values.
// ---------------------------------------------------------------------------
Result<void> ImageCreator::createSpwImage(
    RawDiskHandle& srcDisk, uint32_t sectorSize,
    uint64_t srcOffset, uint64_t length,
    const ImageCreateConfig& config,
    ImageCreateProgressCallback progressCb)
{
    // Calculate chunk count
    const uint32_t chunkSize = config.chunkSize;
    const uint32_t chunkCount = static_cast<uint32_t>(
        (length + chunkSize - 1) / chunkSize);

    if (chunkCount == 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Nothing to image (zero length)");
    }

    // Create output file
    HANDLE hFile = ::CreateFileW(
        config.outputFilePath.c_str(),
        GENERIC_READ | GENERIC_WRITE, // Need read for seek-back
        0, nullptr, CREATE_ALWAYS,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return makeWin32Error(ErrorCode::FileCreateFailed,
            "Failed to create SPW image file");
    }

    // Get disk geometry for metadata
    auto geomResult = srcDisk.getGeometry();
    if (geomResult.isError())
    {
        ::CloseHandle(hFile);
        return geomResult.error();
    }

    // Build header
    SpwImageHeader header = {};
    std::memcpy(header.magic, SPW_IMAGE_MAGIC, 8);
    header.headerSize = sizeof(SpwImageHeader);
    header.version = 1;
    header.imageDataSize = length;
    header.chunkSize = chunkSize;
    header.chunkCount = chunkCount;
    header.compressionType = config.enableCompression ? 1 : 0; // 1 = LZNT1
    header.compressionLevel = 0;
    header.flags = config.enableSparse ? 1u : 0u;

    // Set creation timestamp
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    header.creationTimestamp =
        (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;

    populateDiskMetadata(header, srcDisk, geomResult.value());

    // Allocate chunk table (will be filled in as we process chunks)
    std::vector<SpwChunkEntry> chunkTable(chunkCount);
    std::memset(chunkTable.data(), 0,
                chunkCount * sizeof(SpwChunkEntry));

    // Write placeholder header
    const uint64_t headerOffset = 0;
    const uint64_t chunkTableOffset = sizeof(SpwImageHeader);
    const uint64_t chunkTableSize =
        static_cast<uint64_t>(chunkCount) * sizeof(SpwChunkEntry);
    const uint64_t dataStartOffset = chunkTableOffset + chunkTableSize;

    DWORD bytesWritten = 0;

    // Write header placeholder
    BOOL ok = ::WriteFile(hFile, &header, sizeof(header), &bytesWritten, nullptr);
    if (!ok || bytesWritten != sizeof(header))
    {
        ::CloseHandle(hFile);
        ::DeleteFileW(config.outputFilePath.c_str());
        return makeWin32Error(ErrorCode::ImageWriteError,
            "Failed to write SPW header");
    }

    // Write chunk table placeholder
    ok = ::WriteFile(hFile, chunkTable.data(),
                     static_cast<DWORD>(chunkTableSize), &bytesWritten, nullptr);
    if (!ok || bytesWritten != static_cast<DWORD>(chunkTableSize))
    {
        ::CloseHandle(hFile);
        ::DeleteFileW(config.outputFilePath.c_str());
        return makeWin32Error(ErrorCode::ImageWriteError,
            "Failed to write SPW chunk table placeholder");
    }

    // Initialize SHA-256 hasher for the uncompressed data
    // We'll compute it chunk by chunk
    // Using BCrypt directly for incremental hashing
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    NTSTATUS ntStatus = ::BCryptOpenAlgorithmProvider(
        &hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(ntStatus))
    {
        ::CloseHandle(hFile);
        ::DeleteFileW(config.outputFilePath.c_str());
        return ErrorInfo::fromCode(ErrorCode::Unknown,
            "Failed to open SHA-256 algorithm provider");
    }

    DWORD hashObjectSize = 0;
    DWORD cbData = 0;
    ::BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                        reinterpret_cast<PUCHAR>(&hashObjectSize),
                        sizeof(hashObjectSize), &cbData, 0);

    std::vector<uint8_t> hashObject(hashObjectSize);
    ntStatus = ::BCryptCreateHash(
        hAlg, &hHash, hashObject.data(), hashObjectSize,
        nullptr, 0, 0);

    if (!BCRYPT_SUCCESS(ntStatus))
    {
        ::BCryptCloseAlgorithmProvider(hAlg, 0);
        ::CloseHandle(hFile);
        ::DeleteFileW(config.outputFilePath.c_str());
        return ErrorInfo::fromCode(ErrorCode::Unknown,
            "Failed to create SHA-256 hash object");
    }

    LARGE_INTEGER startTime, perfFreq;
    ::QueryPerformanceFrequency(&perfFreq);
    ::QueryPerformanceCounter(&startTime);

    uint64_t currentFileOffset = dataStartOffset;
    uint64_t bytesProcessed = 0;
    uint64_t totalCompressedBytes = 0;
    uint32_t sparseCount = 0;
    const uint32_t sectorsPerChunk = chunkSize / sectorSize;

    for (uint32_t chunkIdx = 0; chunkIdx < chunkCount; ++chunkIdx)
    {
        if (isCancelRequested())
        {
            ::BCryptDestroyHash(hHash);
            ::BCryptCloseAlgorithmProvider(hAlg, 0);
            ::CloseHandle(hFile);
            ::DeleteFileW(config.outputFilePath.c_str());
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                "Image creation canceled");
        }

        // Calculate how many bytes to read for this chunk
        const uint64_t chunkOffset =
            srcOffset + static_cast<uint64_t>(chunkIdx) * chunkSize;
        const uint64_t remaining = length - bytesProcessed;
        const uint32_t thisChunkSize = static_cast<uint32_t>(
            std::min(static_cast<uint64_t>(chunkSize), remaining));

        // Read from disk
        const SectorOffset lba = chunkOffset / sectorSize;
        const SectorCount sectors = static_cast<SectorCount>(
            (thisChunkSize + sectorSize - 1) / sectorSize);

        auto readResult = srcDisk.readSectors(lba, sectors, sectorSize);
        if (readResult.isError())
        {
            ::BCryptDestroyHash(hHash);
            ::BCryptCloseAlgorithmProvider(hAlg, 0);
            ::CloseHandle(hFile);
            ::DeleteFileW(config.outputFilePath.c_str());
            return readResult.error();
        }

        const auto& rawData = readResult.value();
        const size_t rawSize = std::min(
            static_cast<size_t>(thisChunkSize), rawData.size());

        // Update SHA-256 with uncompressed data
        ::BCryptHashData(hHash, const_cast<PUCHAR>(rawData.data()),
                         static_cast<ULONG>(rawSize), 0);

        // CRC32 for this chunk
        const uint32_t chunkCrc = Checksums::crc32Buffer(rawData.data(), rawSize);

        // Check for sparse (all-zero) chunks
        SpwChunkEntry& entry = chunkTable[chunkIdx];
        entry.uncompressedSize = static_cast<uint32_t>(rawSize);
        entry.crc32 = chunkCrc;

        if (config.enableSparse && isAllZeros(rawData.data(), rawSize))
        {
            // Sparse chunk — don't store data
            entry.fileOffset = 0;
            entry.compressedSize = 0;
            entry.flags = 1; // Sparse flag
            ++sparseCount;
        }
        else if (config.enableCompression)
        {
            // Compress with LZNT1
            auto compResult = compressLZNT1(rawData.data(), rawSize);
            if (compResult.isError())
            {
                // Compression failed — store uncompressed
                entry.fileOffset = currentFileOffset;
                entry.compressedSize = static_cast<uint32_t>(rawSize);
                entry.flags = 0;

                ok = ::WriteFile(hFile, rawData.data(),
                                 static_cast<DWORD>(rawSize),
                                 &bytesWritten, nullptr);
                if (!ok)
                {
                    ::BCryptDestroyHash(hHash);
                    ::BCryptCloseAlgorithmProvider(hAlg, 0);
                    ::CloseHandle(hFile);
                    ::DeleteFileW(config.outputFilePath.c_str());
                    return makeWin32Error(ErrorCode::ImageWriteError,
                        "Failed to write uncompressed chunk");
                }

                currentFileOffset += rawSize;
                totalCompressedBytes += rawSize;
            }
            else
            {
                const auto& compressed = compResult.value();

                // Only use compressed version if it's actually smaller
                if (compressed.size() < rawSize)
                {
                    entry.fileOffset = currentFileOffset;
                    entry.compressedSize =
                        static_cast<uint32_t>(compressed.size());
                    entry.flags = 0;

                    ok = ::WriteFile(hFile, compressed.data(),
                                     static_cast<DWORD>(compressed.size()),
                                     &bytesWritten, nullptr);
                    if (!ok)
                    {
                        ::BCryptDestroyHash(hHash);
                        ::BCryptCloseAlgorithmProvider(hAlg, 0);
                        ::CloseHandle(hFile);
                        ::DeleteFileW(config.outputFilePath.c_str());
                        return makeWin32Error(ErrorCode::ImageWriteError,
                            "Failed to write compressed chunk");
                    }

                    currentFileOffset += compressed.size();
                    totalCompressedBytes += compressed.size();
                }
                else
                {
                    // Compressed is larger — store uncompressed
                    entry.fileOffset = currentFileOffset;
                    entry.compressedSize = static_cast<uint32_t>(rawSize);
                    entry.flags = 0;

                    ok = ::WriteFile(hFile, rawData.data(),
                                     static_cast<DWORD>(rawSize),
                                     &bytesWritten, nullptr);
                    if (!ok)
                    {
                        ::BCryptDestroyHash(hHash);
                        ::BCryptCloseAlgorithmProvider(hAlg, 0);
                        ::CloseHandle(hFile);
                        ::DeleteFileW(config.outputFilePath.c_str());
                        return makeWin32Error(ErrorCode::ImageWriteError,
                            "Failed to write uncompressed chunk");
                    }

                    currentFileOffset += rawSize;
                    totalCompressedBytes += rawSize;
                }
            }
        }
        else
        {
            // No compression — store raw
            entry.fileOffset = currentFileOffset;
            entry.compressedSize = static_cast<uint32_t>(rawSize);
            entry.flags = 0;

            ok = ::WriteFile(hFile, rawData.data(),
                             static_cast<DWORD>(rawSize),
                             &bytesWritten, nullptr);
            if (!ok)
            {
                ::BCryptDestroyHash(hHash);
                ::BCryptCloseAlgorithmProvider(hAlg, 0);
                ::CloseHandle(hFile);
                ::DeleteFileW(config.outputFilePath.c_str());
                return makeWin32Error(ErrorCode::ImageWriteError,
                    "Failed to write raw chunk");
            }

            currentFileOffset += rawSize;
            totalCompressedBytes += rawSize;
        }

        bytesProcessed += rawSize;

        // Report progress
        if (progressCb)
        {
            ImageCreateProgress progress;
            progress.bytesProcessed = bytesProcessed;
            progress.totalBytes = length;
            progress.compressedBytes = totalCompressedBytes;
            progress.percentComplete =
                static_cast<double>(bytesProcessed) /
                static_cast<double>(length) * 100.0;

            if (totalCompressedBytes > 0)
            {
                progress.compressionRatio =
                    static_cast<double>(bytesProcessed) /
                    static_cast<double>(totalCompressedBytes);
            }

            LARGE_INTEGER now;
            ::QueryPerformanceCounter(&now);
            const double elapsed =
                static_cast<double>(now.QuadPart - startTime.QuadPart) /
                static_cast<double>(perfFreq.QuadPart);

            if (elapsed > 0.0)
            {
                progress.speedBytesPerSec =
                    static_cast<double>(bytesProcessed) / elapsed;
                if (progress.speedBytesPerSec > 0.0 && bytesProcessed < length)
                {
                    progress.etaSeconds =
                        static_cast<double>(length - bytesProcessed) /
                        progress.speedBytesPerSec;
                }
            }

            if (!progressCb(progress))
            {
                ::BCryptDestroyHash(hHash);
                ::BCryptCloseAlgorithmProvider(hAlg, 0);
                ::CloseHandle(hFile);
                ::DeleteFileW(config.outputFilePath.c_str());
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                    "Image creation canceled");
            }
        }
    }

    // Finalize SHA-256
    ::BCryptFinishHash(hHash, header.sha256, 32, 0);
    ::BCryptDestroyHash(hHash);
    ::BCryptCloseAlgorithmProvider(hAlg, 0);

    // Update header with final values
    header.sparseChunkCount = sparseCount;

    // Seek back to beginning and rewrite header with SHA-256 and final metadata
    LARGE_INTEGER seekPos;
    seekPos.QuadPart = 0;
    if (!::SetFilePointerEx(hFile, seekPos, nullptr, FILE_BEGIN))
    {
        ::CloseHandle(hFile);
        return makeWin32Error(ErrorCode::ImageWriteError,
            "Failed to seek to header position");
    }

    ok = ::WriteFile(hFile, &header, sizeof(header), &bytesWritten, nullptr);
    if (!ok || bytesWritten != sizeof(header))
    {
        ::CloseHandle(hFile);
        return makeWin32Error(ErrorCode::ImageWriteError,
            "Failed to rewrite SPW header");
    }

    // Rewrite chunk table with actual offsets and sizes
    ok = ::WriteFile(hFile, chunkTable.data(),
                     static_cast<DWORD>(chunkTableSize),
                     &bytesWritten, nullptr);
    if (!ok || bytesWritten != static_cast<DWORD>(chunkTableSize))
    {
        ::CloseHandle(hFile);
        return makeWin32Error(ErrorCode::ImageWriteError,
            "Failed to rewrite SPW chunk table");
    }

    ::CloseHandle(hFile);
    return Result<void>::ok();
}

} // namespace spw
