#include "ImageRestorer.h"
#include "Checksums.h"

#include "../common/Constants.h"

#include <algorithm>
#include <cstring>
#include <sstream>

// RtlDecompressBuffer from ntdll.dll — LZNT1 decompression counterpart
typedef NTSTATUS(WINAPI* RtlDecompressBufferFn)(
    USHORT CompressionFormat,
    PUCHAR UncompressedBuffer,
    ULONG UncompressedBufferSize,
    PUCHAR CompressedBuffer,
    ULONG CompressedBufferSize,
    PULONG FinalUncompressedSize);

#ifndef COMPRESSION_FORMAT_LZNT1
#define COMPRESSION_FORMAT_LZNT1 0x0002
#endif

namespace spw
{

// ---------------------------------------------------------------------------
static ErrorInfo makeWin32Error(ErrorCode code, const std::string& context)
{
    const DWORD lastErr = ::GetLastError();
    std::ostringstream oss;
    oss << context << " (Win32 error " << lastErr << ")";
    return ErrorInfo::fromWin32(code, lastErr, oss.str());
}

// ---------------------------------------------------------------------------
void ImageRestorer::requestCancel()
{
    m_cancelRequested.store(true, std::memory_order_release);
}

bool ImageRestorer::isCancelRequested() const
{
    return m_cancelRequested.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// LZNT1 decompression via ntdll.dll
// ---------------------------------------------------------------------------
Result<std::vector<uint8_t>> ImageRestorer::decompressLZNT1(
    const uint8_t* compressedData, size_t compressedSize,
    size_t uncompressedSize)
{
    static HMODULE hNtdll = ::GetModuleHandleW(L"ntdll.dll");
    static auto pRtlDecompressBuffer =
        reinterpret_cast<RtlDecompressBufferFn>(
            ::GetProcAddress(hNtdll, "RtlDecompressBuffer"));

    if (!pRtlDecompressBuffer)
    {
        return ErrorInfo::fromCode(ErrorCode::NotImplemented,
            "RtlDecompressBuffer not available in ntdll.dll");
    }

    std::vector<uint8_t> output(uncompressedSize);
    ULONG finalSize = 0;

    NTSTATUS status = pRtlDecompressBuffer(
        COMPRESSION_FORMAT_LZNT1,
        output.data(),
        static_cast<ULONG>(uncompressedSize),
        const_cast<PUCHAR>(compressedData),
        static_cast<ULONG>(compressedSize),
        &finalSize);

    if (status != 0) // STATUS_SUCCESS = 0
    {
        std::ostringstream oss;
        oss << "RtlDecompressBuffer failed (NTSTATUS 0x"
            << std::hex << status << ")";
        return ErrorInfo::fromCode(ErrorCode::Unknown, oss.str());
    }

    output.resize(finalSize);
    return output;
}

// ---------------------------------------------------------------------------
// Lock/dismount destination volumes
// ---------------------------------------------------------------------------
Result<std::vector<HANDLE>> ImageRestorer::lockDestinationVolumes(
    const std::vector<wchar_t>& volumeLetters)
{
    std::vector<HANDLE> lockedHandles;

    for (wchar_t letter : volumeLetters)
    {
        RawDiskHandle::dismountVolume(letter);

        auto lockResult = RawDiskHandle::lockVolume(letter);
        if (lockResult.isError())
        {
            unlockVolumes(lockedHandles);
            return ErrorInfo::fromCode(ErrorCode::DiskLockFailed,
                std::string("Failed to lock volume ") +
                static_cast<char>(letter) + ":");
        }

        lockedHandles.push_back(lockResult.value());
    }

    return lockedHandles;
}

void ImageRestorer::unlockVolumes(std::vector<HANDLE>& handles)
{
    for (HANDLE h : handles)
    {
        if (h != INVALID_HANDLE_VALUE)
        {
            RawDiskHandle::unlockVolume(h);
            ::CloseHandle(h);
        }
    }
    handles.clear();
}

// ---------------------------------------------------------------------------
// Detect image format by reading the first 8 bytes
// ---------------------------------------------------------------------------
Result<ImageFormat> ImageRestorer::detectFormat(const std::wstring& filePath)
{
    HANDLE hFile = ::CreateFileW(
        filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return makeWin32Error(ErrorCode::FileNotFound,
            "Failed to open image file");
    }

    uint8_t magic[8] = {};
    DWORD bytesRead = 0;
    BOOL ok = ::ReadFile(hFile, magic, 8, &bytesRead, nullptr);
    ::CloseHandle(hFile);

    if (!ok || bytesRead < 8)
    {
        // Too small to have SPW header — treat as raw
        return ImageFormat::Raw;
    }

    if (std::memcmp(magic, SPW_IMAGE_MAGIC, 8) == 0)
    {
        return ImageFormat::SPW;
    }

    return ImageFormat::Raw;
}

// ---------------------------------------------------------------------------
// Inspect an SPW image and return its metadata
// ---------------------------------------------------------------------------
Result<SpwImageInfo> ImageRestorer::inspectImage(const std::wstring& filePath)
{
    auto fmtResult = detectFormat(filePath);
    if (fmtResult.isError())
        return fmtResult.error();

    if (fmtResult.value() != ImageFormat::SPW)
    {
        return ErrorInfo::fromCode(ErrorCode::ImageCorrupt,
            "File is not in SPW format");
    }

    HANDLE hFile = ::CreateFileW(
        filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return makeWin32Error(ErrorCode::FileNotFound,
            "Failed to open image file for inspection");
    }

    SpwImageHeader header = {};
    DWORD bytesRead = 0;
    BOOL ok = ::ReadFile(hFile, &header, sizeof(header), &bytesRead, nullptr);
    ::CloseHandle(hFile);

    if (!ok || bytesRead < sizeof(header))
    {
        return ErrorInfo::fromCode(ErrorCode::ImageCorrupt,
            "Failed to read SPW header");
    }

    if (header.version != 1)
    {
        return ErrorInfo::fromCode(ErrorCode::ImageCorrupt,
            "Unsupported SPW image version");
    }

    SpwImageInfo info;
    info.diskModel = std::string(header.diskModel,
        strnlen(header.diskModel, sizeof(header.diskModel)));
    info.diskSerial = std::string(header.diskSerial,
        strnlen(header.diskSerial, sizeof(header.diskSerial)));
    info.sourceDiskSize = header.sourceDiskSize;
    info.sourceSectorSize = header.sourceSectorSize;
    info.partitionTableType =
        static_cast<PartitionTableType>(header.partitionTableType);
    info.imageDataSize = header.imageDataSize;
    info.chunkCount = header.chunkCount;
    info.sparseChunkCount = header.sparseChunkCount;
    info.isCompressed = (header.compressionType != 0);
    std::memcpy(info.sha256.data(), header.sha256, 32);
    info.creationTimestamp = header.creationTimestamp;

    return info;
}

// ---------------------------------------------------------------------------
// Main restore entry point
// ---------------------------------------------------------------------------
Result<void> ImageRestorer::restoreImage(
    const ImageRestoreConfig& config,
    ImageRestoreProgressCallback progressCb)
{
    m_cancelRequested.store(false, std::memory_order_release);

    if (config.inputFilePath.empty())
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Input file path is empty");
    }
    if (config.destDiskId < 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Invalid destination disk ID");
    }

    // Detect format
    auto fmtResult = detectFormat(config.inputFilePath);
    if (fmtResult.isError())
        return fmtResult.error();

    const ImageFormat format = fmtResult.value();

    // Open image file
    HANDLE hFile = ::CreateFileW(
        config.inputFilePath.c_str(),
        GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return makeWin32Error(ErrorCode::FileNotFound,
            "Failed to open image file");
    }

    // Get file size
    LARGE_INTEGER fileSize;
    if (!::GetFileSizeEx(hFile, &fileSize))
    {
        ::CloseHandle(hFile);
        return makeWin32Error(ErrorCode::ImageReadError,
            "Failed to get image file size");
    }

    // Open destination disk
    auto dstResult = RawDiskHandle::open(config.destDiskId, DiskAccessMode::ReadWrite);
    if (dstResult.isError())
    {
        ::CloseHandle(hFile);
        return dstResult.error();
    }

    auto& dstDisk = dstResult.value();

    auto geomResult = dstDisk.getGeometry();
    if (geomResult.isError())
    {
        ::CloseHandle(hFile);
        return geomResult.error();
    }

    const uint32_t dstSectorSize = geomResult.value().bytesPerSector;

    // Lock destination volumes
    std::vector<HANDLE> lockedVolumes;
    if (!config.destVolumeLetters.empty())
    {
        auto lockResult = lockDestinationVolumes(config.destVolumeLetters);
        if (lockResult.isError())
        {
            ::CloseHandle(hFile);
            return lockResult.error();
        }
        lockedVolumes = std::move(lockResult.value());
    }

    Result<void> result = Result<void>::ok();

    if (format == ImageFormat::Raw)
    {
        result = restoreRawImage(
            hFile, static_cast<uint64_t>(fileSize.QuadPart),
            dstDisk, dstSectorSize, config.destOffsetBytes,
            config.bufferSize, progressCb);
    }
    else
    {
        // Read SPW header
        SpwImageHeader header = {};
        DWORD bytesRead = 0;

        // Seek to beginning (should already be there, but be explicit)
        LARGE_INTEGER seekPos;
        seekPos.QuadPart = 0;
        ::SetFilePointerEx(hFile, seekPos, nullptr, FILE_BEGIN);

        BOOL ok = ::ReadFile(hFile, &header, sizeof(header), &bytesRead, nullptr);
        if (!ok || bytesRead < sizeof(header))
        {
            ::CloseHandle(hFile);
            unlockVolumes(lockedVolumes);
            return ErrorInfo::fromCode(ErrorCode::ImageCorrupt,
                "Failed to read SPW header");
        }

        if (std::memcmp(header.magic, SPW_IMAGE_MAGIC, 8) != 0 ||
            header.version != 1)
        {
            ::CloseHandle(hFile);
            unlockVolumes(lockedVolumes);
            return ErrorInfo::fromCode(ErrorCode::ImageCorrupt,
                "Invalid SPW image header");
        }

        // Validate image fits on destination
        const uint64_t dstTotalBytes = geomResult.value().totalBytes;
        if (config.destOffsetBytes + header.imageDataSize > dstTotalBytes)
        {
            ::CloseHandle(hFile);
            unlockVolumes(lockedVolumes);
            return ErrorInfo::fromCode(ErrorCode::InsufficientDiskSpace,
                "Image is larger than destination disk");
        }

        // Read chunk table
        const uint32_t chunkCount = header.chunkCount;
        std::vector<SpwChunkEntry> chunkTable(chunkCount);

        ok = ::ReadFile(hFile, chunkTable.data(),
                        static_cast<DWORD>(chunkCount * sizeof(SpwChunkEntry)),
                        &bytesRead, nullptr);

        if (!ok || bytesRead <
            static_cast<DWORD>(chunkCount * sizeof(SpwChunkEntry)))
        {
            ::CloseHandle(hFile);
            unlockVolumes(lockedVolumes);
            return ErrorInfo::fromCode(ErrorCode::ImageCorrupt,
                "Failed to read SPW chunk table");
        }

        result = restoreSpwImage(
            hFile, header, chunkTable,
            dstDisk, dstSectorSize, config.destOffsetBytes,
            config.verifyAfterRestore, progressCb);
    }

    ::CloseHandle(hFile);

    // Flush writes
    if (result.isOk())
    {
        dstDisk.flushBuffers();
    }

    // Report completion
    if (result.isOk() && progressCb)
    {
        ImageRestoreProgress done;
        done.phase = ImageRestoreProgress::Phase::Complete;
        done.percentComplete = 100.0;
        progressCb(done);
    }

    unlockVolumes(lockedVolumes);
    return result;
}

// ---------------------------------------------------------------------------
// Restore raw .img — read file in chunks, write sectors to disk
// ---------------------------------------------------------------------------
Result<void> ImageRestorer::restoreRawImage(
    HANDLE hFile, uint64_t fileSize,
    RawDiskHandle& dstDisk, uint32_t dstSectorSize,
    uint64_t dstOffset, uint32_t bufferSize,
    ImageRestoreProgressCallback progressCb)
{
    // Validate image fits
    auto geomResult = dstDisk.getGeometry();
    if (geomResult.isError())
        return geomResult.error();

    if (dstOffset + fileSize > geomResult.value().totalBytes)
    {
        return ErrorInfo::fromCode(ErrorCode::InsufficientDiskSpace,
            "Image file is larger than destination disk");
    }

    const uint32_t alignedBufSize =
        (bufferSize / dstSectorSize) * dstSectorSize;
    if (alignedBufSize == 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Buffer size too small");
    }

    std::vector<uint8_t> readBuffer(alignedBufSize);

    LARGE_INTEGER startTime, perfFreq;
    ::QueryPerformanceFrequency(&perfFreq);
    ::QueryPerformanceCounter(&startTime);

    uint64_t bytesWritten = 0;
    uint64_t dstPos = dstOffset;

    while (bytesWritten < fileSize)
    {
        if (isCancelRequested())
        {
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                "Restore canceled");
        }

        const uint64_t remaining = fileSize - bytesWritten;
        const DWORD readSize = static_cast<DWORD>(
            std::min(static_cast<uint64_t>(alignedBufSize), remaining));

        DWORD bytesRead = 0;
        BOOL ok = ::ReadFile(hFile, readBuffer.data(), readSize,
                             &bytesRead, nullptr);
        if (!ok)
        {
            return makeWin32Error(ErrorCode::ImageReadError,
                "Failed to read from image file");
        }
        if (bytesRead == 0)
            break; // EOF

        // Pad to sector alignment if needed
        const uint32_t alignedWriteSize =
            ((bytesRead + dstSectorSize - 1) / dstSectorSize) * dstSectorSize;

        if (alignedWriteSize > bytesRead)
        {
            std::memset(readBuffer.data() + bytesRead, 0,
                        alignedWriteSize - bytesRead);
        }

        const SectorOffset dstLba = dstPos / dstSectorSize;
        const SectorCount dstSectors =
            static_cast<SectorCount>(alignedWriteSize / dstSectorSize);

        auto writeResult = dstDisk.writeSectors(
            dstLba, readBuffer.data(), dstSectors, dstSectorSize);
        if (writeResult.isError())
            return writeResult.error();

        dstPos += bytesRead;
        bytesWritten += bytesRead;

        if (progressCb)
        {
            ImageRestoreProgress progress;
            progress.phase = ImageRestoreProgress::Phase::Restoring;
            progress.bytesWritten = bytesWritten;
            progress.totalBytes = fileSize;
            progress.percentComplete =
                static_cast<double>(bytesWritten) /
                static_cast<double>(fileSize) * 100.0;

            LARGE_INTEGER now;
            ::QueryPerformanceCounter(&now);
            const double elapsed =
                static_cast<double>(now.QuadPart - startTime.QuadPart) /
                static_cast<double>(perfFreq.QuadPart);

            if (elapsed > 0.0)
            {
                progress.speedBytesPerSec =
                    static_cast<double>(bytesWritten) / elapsed;
                if (progress.speedBytesPerSec > 0.0)
                {
                    progress.etaSeconds =
                        static_cast<double>(fileSize - bytesWritten) /
                        progress.speedBytesPerSec;
                }
            }

            if (!progressCb(progress))
            {
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                    "Restore canceled");
            }
        }
    }

    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// Restore SPW compressed image
// ---------------------------------------------------------------------------
Result<void> ImageRestorer::restoreSpwImage(
    HANDLE hFile,
    const SpwImageHeader& header,
    const std::vector<SpwChunkEntry>& chunkTable,
    RawDiskHandle& dstDisk, uint32_t dstSectorSize,
    uint64_t dstOffset,
    bool verify,
    ImageRestoreProgressCallback progressCb)
{
    const uint32_t chunkSize = header.chunkSize;
    const uint32_t chunkCount = header.chunkCount;
    const bool isCompressed = (header.compressionType != 0);

    // Initialize SHA-256 for verification
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<uint8_t> hashObject;

    if (verify)
    {
        NTSTATUS ntStatus = ::BCryptOpenAlgorithmProvider(
            &hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (BCRYPT_SUCCESS(ntStatus))
        {
            DWORD hashObjSize = 0;
            DWORD cbData = 0;
            ::BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                                reinterpret_cast<PUCHAR>(&hashObjSize),
                                sizeof(hashObjSize), &cbData, 0);
            hashObject.resize(hashObjSize);
            ::BCryptCreateHash(hAlg, &hHash, hashObject.data(),
                               hashObjSize, nullptr, 0, 0);
        }
    }

    LARGE_INTEGER startTime, perfFreq;
    ::QueryPerformanceFrequency(&perfFreq);
    ::QueryPerformanceCounter(&startTime);

    uint64_t bytesWritten = 0;
    const uint64_t totalBytes = header.imageDataSize;
    std::vector<uint8_t> zeroChunk(chunkSize, 0);

    for (uint32_t chunkIdx = 0; chunkIdx < chunkCount; ++chunkIdx)
    {
        if (isCancelRequested())
        {
            if (hHash) ::BCryptDestroyHash(hHash);
            if (hAlg) ::BCryptCloseAlgorithmProvider(hAlg, 0);
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                "Restore canceled");
        }

        const SpwChunkEntry& entry = chunkTable[chunkIdx];
        const uint32_t uncompSize = entry.uncompressedSize;
        const uint64_t dstChunkOffset =
            dstOffset + static_cast<uint64_t>(chunkIdx) * chunkSize;

        const uint8_t* writeData = nullptr;
        std::vector<uint8_t> decompBuffer;
        std::vector<uint8_t> rawReadBuffer;

        if (entry.flags & 1)
        {
            // Sparse chunk — write zeros
            writeData = zeroChunk.data();

            // Update hash with zeros
            if (hHash)
            {
                ::BCryptHashData(hHash, const_cast<PUCHAR>(zeroChunk.data()),
                                 uncompSize, 0);
            }
        }
        else
        {
            // Seek to chunk data in the file
            LARGE_INTEGER seekPos;
            seekPos.QuadPart = static_cast<LONGLONG>(entry.fileOffset);
            if (!::SetFilePointerEx(hFile, seekPos, nullptr, FILE_BEGIN))
            {
                if (hHash) ::BCryptDestroyHash(hHash);
                if (hAlg) ::BCryptCloseAlgorithmProvider(hAlg, 0);
                return makeWin32Error(ErrorCode::ImageReadError,
                    "Failed to seek to chunk data");
            }

            // Read compressed (or uncompressed) data
            rawReadBuffer.resize(entry.compressedSize);
            DWORD bytesRead = 0;
            BOOL ok = ::ReadFile(hFile, rawReadBuffer.data(),
                                 entry.compressedSize, &bytesRead, nullptr);
            if (!ok || bytesRead < entry.compressedSize)
            {
                if (hHash) ::BCryptDestroyHash(hHash);
                if (hAlg) ::BCryptCloseAlgorithmProvider(hAlg, 0);
                return ErrorInfo::fromCode(ErrorCode::ImageReadError,
                    "Failed to read chunk data from image");
            }

            if (isCompressed && entry.compressedSize != entry.uncompressedSize)
            {
                // Decompress
                auto decompResult = decompressLZNT1(
                    rawReadBuffer.data(), rawReadBuffer.size(),
                    uncompSize);
                if (decompResult.isError())
                {
                    if (hHash) ::BCryptDestroyHash(hHash);
                    if (hAlg) ::BCryptCloseAlgorithmProvider(hAlg, 0);
                    return decompResult.error();
                }
                decompBuffer = std::move(decompResult.value());
                writeData = decompBuffer.data();
            }
            else
            {
                // Data is uncompressed (stored raw)
                writeData = rawReadBuffer.data();
            }

            // Verify CRC32 of uncompressed data
            const uint32_t actualCrc = Checksums::crc32Buffer(
                writeData, uncompSize);
            if (actualCrc != entry.crc32)
            {
                if (hHash) ::BCryptDestroyHash(hHash);
                if (hAlg) ::BCryptCloseAlgorithmProvider(hAlg, 0);

                std::ostringstream oss;
                oss << "CRC32 mismatch on chunk " << chunkIdx
                    << ": expected 0x" << std::hex << entry.crc32
                    << ", got 0x" << actualCrc;
                return ErrorInfo::fromCode(ErrorCode::ImageChecksumMismatch,
                    oss.str());
            }

            // Update SHA-256
            if (hHash)
            {
                ::BCryptHashData(hHash, const_cast<PUCHAR>(writeData),
                                 uncompSize, 0);
            }
        }

        // Write to destination disk
        const uint32_t alignedWriteSize =
            ((uncompSize + dstSectorSize - 1) / dstSectorSize) * dstSectorSize;

        // If we need padding, use a temporary buffer
        std::vector<uint8_t> paddedBuffer;
        if (alignedWriteSize > uncompSize)
        {
            paddedBuffer.resize(alignedWriteSize, 0);
            std::memcpy(paddedBuffer.data(), writeData, uncompSize);
            writeData = paddedBuffer.data();
        }

        const SectorOffset dstLba = dstChunkOffset / dstSectorSize;
        const SectorCount dstSectors =
            static_cast<SectorCount>(alignedWriteSize / dstSectorSize);

        auto writeResult = dstDisk.writeSectors(
            dstLba, writeData, dstSectors, dstSectorSize);
        if (writeResult.isError())
        {
            if (hHash) ::BCryptDestroyHash(hHash);
            if (hAlg) ::BCryptCloseAlgorithmProvider(hAlg, 0);
            return writeResult.error();
        }

        bytesWritten += uncompSize;

        // Report progress
        if (progressCb)
        {
            ImageRestoreProgress progress;
            progress.phase = ImageRestoreProgress::Phase::Restoring;
            progress.bytesWritten = bytesWritten;
            progress.totalBytes = totalBytes;
            progress.percentComplete =
                static_cast<double>(bytesWritten) /
                static_cast<double>(totalBytes) * 100.0;

            LARGE_INTEGER now;
            ::QueryPerformanceCounter(&now);
            const double elapsed =
                static_cast<double>(now.QuadPart - startTime.QuadPart) /
                static_cast<double>(perfFreq.QuadPart);

            if (elapsed > 0.0)
            {
                progress.speedBytesPerSec =
                    static_cast<double>(bytesWritten) / elapsed;
                if (progress.speedBytesPerSec > 0.0)
                {
                    progress.etaSeconds =
                        static_cast<double>(totalBytes - bytesWritten) /
                        progress.speedBytesPerSec;
                }
            }

            if (!progressCb(progress))
            {
                if (hHash) ::BCryptDestroyHash(hHash);
                if (hAlg) ::BCryptCloseAlgorithmProvider(hAlg, 0);
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                    "Restore canceled");
            }
        }
    }

    // SHA-256 verification
    if (verify && hHash)
    {
        uint8_t computedHash[32] = {};
        ::BCryptFinishHash(hHash, computedHash, 32, 0);
        ::BCryptDestroyHash(hHash);
        ::BCryptCloseAlgorithmProvider(hAlg, 0);

        // Check if the stored hash is all zeros (no hash was stored)
        bool storedHashIsZero = true;
        for (int i = 0; i < 32; ++i)
        {
            if (header.sha256[i] != 0)
            {
                storedHashIsZero = false;
                break;
            }
        }

        if (!storedHashIsZero)
        {
            if (std::memcmp(computedHash, header.sha256, 32) != 0)
            {
                return ErrorInfo::fromCode(ErrorCode::ImageChecksumMismatch,
                    "SHA-256 verification failed: restored data does not "
                    "match the hash stored in the image header");
            }
        }

        // Report verification phase
        if (progressCb)
        {
            ImageRestoreProgress progress;
            progress.phase = ImageRestoreProgress::Phase::Verifying;
            progress.bytesWritten = totalBytes;
            progress.totalBytes = totalBytes;
            progress.percentComplete = 100.0;
            progressCb(progress);
        }
    }
    else
    {
        if (hHash) ::BCryptDestroyHash(hHash);
        if (hAlg) ::BCryptCloseAlgorithmProvider(hAlg, 0);
    }

    return Result<void>::ok();
}

} // namespace spw
