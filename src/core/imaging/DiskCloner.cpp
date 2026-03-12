#include "DiskCloner.h"
#include "Checksums.h"

#include "../common/Constants.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace spw
{

// ---------------------------------------------------------------------------
// Helper: Win32 error with GetLastError()
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
void DiskCloner::requestCancel()
{
    m_cancelRequested.store(true, std::memory_order_release);
}

bool DiskCloner::isCancelRequested() const
{
    return m_cancelRequested.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Progress reporting with speed/ETA
// ---------------------------------------------------------------------------
bool DiskCloner::reportProgress(
    CloneProgressCallback& cb,
    CloneProgress::Phase phase,
    uint64_t bytesTransferred,
    uint64_t totalBytes,
    LARGE_INTEGER startTime,
    LARGE_INTEGER perfFreq)
{
    if (!cb)
        return true;

    CloneProgress progress;
    progress.phase = phase;
    progress.bytesTransferred = bytesTransferred;
    progress.totalBytes = totalBytes;

    if (totalBytes > 0)
    {
        progress.percentComplete =
            static_cast<double>(bytesTransferred) / static_cast<double>(totalBytes) * 100.0;
    }

    // Calculate speed and ETA using high-resolution performance counter
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    const double elapsedSec =
        static_cast<double>(now.QuadPart - startTime.QuadPart) /
        static_cast<double>(perfFreq.QuadPart);

    if (elapsedSec > 0.0)
    {
        progress.speedBytesPerSec =
            static_cast<double>(bytesTransferred) / elapsedSec;

        if (progress.speedBytesPerSec > 0.0 && bytesTransferred < totalBytes)
        {
            const double remainingBytes =
                static_cast<double>(totalBytes - bytesTransferred);
            progress.etaSeconds = remainingBytes / progress.speedBytesPerSec;
        }
    }

    return cb(progress);
}

// ---------------------------------------------------------------------------
// Lock destination volumes
// ---------------------------------------------------------------------------
Result<std::vector<HANDLE>> DiskCloner::lockDestinationVolumes(
    const std::vector<wchar_t>& volumeLetters)
{
    std::vector<HANDLE> lockedHandles;

    for (wchar_t letter : volumeLetters)
    {
        // Dismount first — this invalidates all open file handles on the volume
        auto dismountResult = RawDiskHandle::dismountVolume(letter);
        if (dismountResult.isError())
        {
            // Non-fatal: volume might not be mounted. Log but continue.
        }

        // Lock the volume for exclusive access
        auto lockResult = RawDiskHandle::lockVolume(letter);
        if (lockResult.isError())
        {
            // Unlock anything we already locked
            unlockVolumes(lockedHandles);
            return ErrorInfo::fromCode(ErrorCode::DiskLockFailed,
                std::string("Failed to lock volume ") +
                static_cast<char>(letter) + ":");
        }

        lockedHandles.push_back(lockResult.value());
    }

    return lockedHandles;
}

// ---------------------------------------------------------------------------
// Unlock volumes
// ---------------------------------------------------------------------------
void DiskCloner::unlockVolumes(std::vector<HANDLE>& lockedHandles)
{
    for (HANDLE h : lockedHandles)
    {
        if (h != INVALID_HANDLE_VALUE)
        {
            RawDiskHandle::unlockVolume(h);
            ::CloseHandle(h);
        }
    }
    lockedHandles.clear();
}

// ---------------------------------------------------------------------------
// Main clone entry point
// ---------------------------------------------------------------------------
Result<void> DiskCloner::clone(const CloneConfig& config,
                               CloneProgressCallback progressCb)
{
    m_cancelRequested.store(false, std::memory_order_release);

    // Validate configuration
    if (config.sourceDiskId < 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Invalid source disk ID");
    }
    if (config.destDiskId < 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Invalid destination disk ID");
    }
    if (config.sourceDiskId == config.destDiskId)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Source and destination cannot be the same disk");
    }

    // Open source (read-only) and destination (read-write)
    auto srcResult = RawDiskHandle::open(config.sourceDiskId, DiskAccessMode::ReadOnly);
    if (srcResult.isError())
        return srcResult.error();

    auto dstResult = RawDiskHandle::open(config.destDiskId, DiskAccessMode::ReadWrite);
    if (dstResult.isError())
        return dstResult.error();

    auto& srcDisk = srcResult.value();
    auto& dstDisk = dstResult.value();

    // Get geometry for both disks
    auto srcGeom = srcDisk.getGeometry();
    if (srcGeom.isError())
        return srcGeom.error();

    auto dstGeom = dstDisk.getGeometry();
    if (dstGeom.isError())
        return dstGeom.error();

    const uint32_t srcSectorSize = srcGeom.value().bytesPerSector;
    const uint32_t dstSectorSize = dstGeom.value().bytesPerSector;
    const uint64_t srcTotalBytes = srcGeom.value().totalBytes;
    const uint64_t dstTotalBytes = dstGeom.value().totalBytes;

    // Determine the byte range to clone
    uint64_t srcOffset = config.sourceOffsetBytes;
    uint64_t dstOffset = config.destOffsetBytes;
    uint64_t cloneLength = config.sourceLengthBytes;

    if (cloneLength == 0)
    {
        // Clone entire source disk
        if (srcOffset > srcTotalBytes)
        {
            return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                "Source offset exceeds disk size");
        }
        cloneLength = srcTotalBytes - srcOffset;
    }

    // Validate source range fits in source disk
    if (srcOffset + cloneLength > srcTotalBytes)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Source range exceeds source disk size");
    }

    // Validate destination has enough space
    if (dstOffset + cloneLength > dstTotalBytes)
    {
        if (!config.allowTruncation)
        {
            return ErrorInfo::fromCode(ErrorCode::InsufficientDiskSpace,
                "Destination disk is too small for the clone operation");
        }
        // Truncate to what fits
        cloneLength = dstTotalBytes - dstOffset;
    }

    // Ensure offsets are aligned to the larger of the two sector sizes
    const uint32_t alignmentSize = std::max(srcSectorSize, dstSectorSize);
    if (srcOffset % alignmentSize != 0 || dstOffset % alignmentSize != 0)
    {
        return ErrorInfo::fromCode(ErrorCode::AlignmentError,
            "Source and destination offsets must be sector-aligned");
    }

    // Lock and dismount destination volumes
    std::vector<HANDLE> lockedVolumes;
    if (!config.destVolumeLetters.empty())
    {
        auto lockResult = lockDestinationVolumes(config.destVolumeLetters);
        if (lockResult.isError())
            return lockResult.error();
        lockedVolumes = std::move(lockResult.value());
    }

    // Perform the clone
    Result<void> cloneResult = Result<void>::ok();

    if (config.mode == CloneMode::Smart)
    {
        cloneResult = cloneSmart(
            srcDisk, srcSectorSize, dstDisk, dstSectorSize,
            srcOffset, cloneLength, dstOffset,
            config.bufferSize, progressCb);
    }
    else
    {
        cloneResult = cloneRaw(
            srcDisk, srcSectorSize, dstDisk, dstSectorSize,
            srcOffset, cloneLength, dstOffset,
            config.bufferSize, progressCb);
    }

    if (cloneResult.isError())
    {
        unlockVolumes(lockedVolumes);
        return cloneResult;
    }

    // Flush destination disk to ensure all writes are committed
    auto flushResult = dstDisk.flushBuffers();
    if (flushResult.isError())
    {
        unlockVolumes(lockedVolumes);
        return flushResult;
    }

    // Verification pass
    if (config.verifyAfterClone)
    {
        auto verifyResult = verifyClone(
            srcDisk, srcSectorSize, dstDisk, dstSectorSize,
            srcOffset, cloneLength, dstOffset,
            config.bufferSize, progressCb);

        if (verifyResult.isError())
        {
            unlockVolumes(lockedVolumes);
            return verifyResult;
        }
    }

    // Report completion
    if (progressCb)
    {
        CloneProgress done;
        done.phase = CloneProgress::Phase::Complete;
        done.bytesTransferred = cloneLength;
        done.totalBytes = cloneLength;
        done.percentComplete = 100.0;
        progressCb(done);
    }

    unlockVolumes(lockedVolumes);
    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// Raw sector-by-sector clone.
// Handles mismatched sector sizes by using an intermediate buffer aligned
// to the LCM of both sector sizes.
// ---------------------------------------------------------------------------
Result<void> DiskCloner::cloneRaw(
    RawDiskHandle& src, uint32_t srcSectorSize,
    RawDiskHandle& dst, uint32_t dstSectorSize,
    uint64_t srcOffsetBytes, uint64_t lengthBytes, uint64_t dstOffsetBytes,
    uint32_t bufferSize, CloneProgressCallback progressCb)
{
    // The I/O buffer must be a multiple of both sector sizes.
    // Find the LCM of the two sector sizes and round bufferSize up.
    // For 512 and 4096, LCM = 4096. For matching sizes, LCM = sectorSize.
    const uint32_t maxSectorSize = std::max(srcSectorSize, dstSectorSize);

    // Round buffer size down to a multiple of maxSectorSize
    const uint32_t alignedBufSize =
        (bufferSize / maxSectorSize) * maxSectorSize;

    if (alignedBufSize == 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Buffer size too small for sector alignment");
    }

    std::vector<uint8_t> ioBuffer(alignedBufSize);

    LARGE_INTEGER startTime, perfFreq;
    ::QueryPerformanceFrequency(&perfFreq);
    ::QueryPerformanceCounter(&startTime);

    uint64_t bytesRemaining = lengthBytes;
    uint64_t bytesTransferred = 0;
    uint64_t srcPos = srcOffsetBytes;
    uint64_t dstPos = dstOffsetBytes;

    while (bytesRemaining > 0)
    {
        if (isCancelRequested())
        {
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                "Clone canceled by user");
        }

        // Determine chunk size for this iteration
        const uint64_t chunkBytes = std::min(
            static_cast<uint64_t>(alignedBufSize), bytesRemaining);

        // Read from source. We use the source sector size for addressing.
        const SectorOffset srcLba = srcPos / srcSectorSize;
        const SectorCount srcSectors = static_cast<SectorCount>(
            (chunkBytes + srcSectorSize - 1) / srcSectorSize);

        auto readResult = src.readSectors(srcLba, srcSectors, srcSectorSize);
        if (readResult.isError())
            return readResult.error();

        const auto& readData = readResult.value();

        // The actual number of bytes we can write is the minimum of
        // what we read and what we need
        const size_t bytesToWrite = static_cast<size_t>(
            std::min(static_cast<uint64_t>(readData.size()), chunkBytes));

        // If sector sizes differ, we still write sector-aligned chunks.
        // Pad the last chunk with zeros if needed.
        const size_t alignedWriteSize =
            ((bytesToWrite + dstSectorSize - 1) / dstSectorSize) * dstSectorSize;

        // Prepare write buffer (may need zero-padding at the end)
        if (alignedWriteSize > readData.size())
        {
            std::memcpy(ioBuffer.data(), readData.data(), readData.size());
            std::memset(ioBuffer.data() + readData.size(), 0,
                        alignedWriteSize - readData.size());
        }

        const uint8_t* writePtr =
            (alignedWriteSize > readData.size()) ? ioBuffer.data() : readData.data();

        // Write to destination
        const SectorOffset dstLba = dstPos / dstSectorSize;
        const SectorCount dstSectors = static_cast<SectorCount>(
            alignedWriteSize / dstSectorSize);

        auto writeResult = dst.writeSectors(dstLba, writePtr, dstSectors, dstSectorSize);
        if (writeResult.isError())
            return writeResult.error();

        srcPos += bytesToWrite;
        dstPos += bytesToWrite;
        bytesTransferred += bytesToWrite;
        bytesRemaining -= bytesToWrite;

        if (!reportProgress(progressCb, CloneProgress::Phase::Cloning,
                            bytesTransferred, lengthBytes, startTime, perfFreq))
        {
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                "Clone canceled by user");
        }
    }

    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// Smart clone — reads NTFS volume bitmap to skip free clusters.
// For non-NTFS volumes, falls back to raw clone.
//
// The NTFS bitmap approach: use FSCTL_GET_VOLUME_BITMAP on the source
// volume to get a bitmap of allocated clusters. Only copy clusters that
// are marked as in-use.
// ---------------------------------------------------------------------------
Result<void> DiskCloner::cloneSmart(
    RawDiskHandle& src, uint32_t srcSectorSize,
    RawDiskHandle& dst, uint32_t dstSectorSize,
    uint64_t srcOffsetBytes, uint64_t lengthBytes, uint64_t dstOffsetBytes,
    uint32_t bufferSize, CloneProgressCallback progressCb)
{
    // To get the volume bitmap, we need a volume handle, not a raw disk handle.
    // The source offset tells us where the partition starts on disk.
    // We need to figure out if this partition's volume is accessible.

    // Try to open the volume by scanning for a volume whose extents
    // match our source offset. Use the partition layout from the source disk.
    auto layoutResult = src.getDriveLayout();
    if (layoutResult.isError())
    {
        // Can't get layout — fall back to raw
        return cloneRaw(src, srcSectorSize, dst, dstSectorSize,
                        srcOffsetBytes, lengthBytes, dstOffsetBytes,
                        bufferSize, progressCb);
    }

    // Find the partition that matches our source range
    wchar_t volumeLetter = L'\0';
    const auto& layout = layoutResult.value();

    for (const auto& part : layout.partitions)
    {
        if (part.startingOffset == srcOffsetBytes &&
            part.partitionLength == lengthBytes)
        {
            // Found a matching partition. Now we need its drive letter.
            // Use FindFirstVolumeW/GetVolumePathNamesForVolumeNameW to
            // map disk extents to volume letters. This is complex, so
            // we take a simpler approach: iterate A-Z and check if the
            // volume's disk extents match.
            for (wchar_t letter = L'A'; letter <= L'Z'; ++letter)
            {
                wchar_t volPath[] = L"\\\\.\\X:";
                volPath[4] = letter;

                HANDLE hVol = ::CreateFileW(
                    volPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, 0, nullptr);

                if (hVol == INVALID_HANDLE_VALUE)
                    continue;

                // Query IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS
                uint8_t extBuf[256] = {};
                DWORD bytesReturned = 0;
                BOOL ok = ::DeviceIoControl(
                    hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                    nullptr, 0, extBuf, sizeof(extBuf),
                    &bytesReturned, nullptr);

                if (ok)
                {
                    const auto* extents =
                        reinterpret_cast<const VOLUME_DISK_EXTENTS*>(extBuf);

                    if (extents->NumberOfDiskExtents >= 1)
                    {
                        const auto& ext = extents->Extents[0];
                        if (ext.DiskNumber == static_cast<DWORD>(src.diskId()) &&
                            static_cast<uint64_t>(ext.StartingOffset.QuadPart) == srcOffsetBytes)
                        {
                            volumeLetter = letter;
                            ::CloseHandle(hVol);
                            break;
                        }
                    }
                }

                ::CloseHandle(hVol);
            }
            break;
        }
    }

    if (volumeLetter == L'\0')
    {
        // Could not find a volume for smart copy — fall back to raw
        return cloneRaw(src, srcSectorSize, dst, dstSectorSize,
                        srcOffsetBytes, lengthBytes, dstOffsetBytes,
                        bufferSize, progressCb);
    }

    // Open the volume to get the allocation bitmap
    wchar_t volPathBuf[] = L"\\\\.\\X:";
    volPathBuf[4] = volumeLetter;

    HANDLE hVolume = ::CreateFileW(
        volPathBuf, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (hVolume == INVALID_HANDLE_VALUE)
    {
        // Fall back to raw
        return cloneRaw(src, srcSectorSize, dst, dstSectorSize,
                        srcOffsetBytes, lengthBytes, dstOffsetBytes,
                        bufferSize, progressCb);
    }

    // Query cluster size via FSCTL_GET_NTFS_VOLUME_DATA
    NTFS_VOLUME_DATA_BUFFER ntfsData = {};
    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(
        hVolume, FSCTL_GET_NTFS_VOLUME_DATA,
        nullptr, 0, &ntfsData, sizeof(ntfsData),
        &bytesReturned, nullptr);

    if (!ok)
    {
        ::CloseHandle(hVolume);
        // Not NTFS — fall back to raw
        return cloneRaw(src, srcSectorSize, dst, dstSectorSize,
                        srcOffsetBytes, lengthBytes, dstOffsetBytes,
                        bufferSize, progressCb);
    }

    const uint32_t bytesPerCluster =
        static_cast<uint32_t>(ntfsData.BytesPerCluster);
    const int64_t totalClusters = ntfsData.TotalClusters.QuadPart;

    // Allocate bitmap buffer. Each bit represents one cluster.
    // Add some padding for the VOLUME_BITMAP_BUFFER header.
    const size_t bitmapByteCount =
        static_cast<size_t>((totalClusters + 7) / 8);
    const size_t bitmapBufSize =
        sizeof(VOLUME_BITMAP_BUFFER) + bitmapByteCount;
    std::vector<uint8_t> bitmapBuf(bitmapBufSize, 0);

    STARTING_LCN_INPUT_BUFFER startLcn = {};
    startLcn.StartingLcn.QuadPart = 0;

    ok = ::DeviceIoControl(
        hVolume, FSCTL_GET_VOLUME_BITMAP,
        &startLcn, sizeof(startLcn),
        bitmapBuf.data(), static_cast<DWORD>(bitmapBuf.size()),
        &bytesReturned, nullptr);

    ::CloseHandle(hVolume);

    if (!ok)
    {
        // Bitmap query failed — fall back to raw
        return cloneRaw(src, srcSectorSize, dst, dstSectorSize,
                        srcOffsetBytes, lengthBytes, dstOffsetBytes,
                        bufferSize, progressCb);
    }

    const auto* bitmap = reinterpret_cast<const VOLUME_BITMAP_BUFFER*>(bitmapBuf.data());
    const uint8_t* bitmapData =
        bitmapBuf.data() + offsetof(VOLUME_BITMAP_BUFFER, Buffer);

    // Count allocated clusters for accurate progress reporting
    uint64_t allocatedClusters = 0;
    for (int64_t cluster = 0; cluster < totalClusters; ++cluster)
    {
        const size_t byteIdx = static_cast<size_t>(cluster / 8);
        const uint8_t bitMask = static_cast<uint8_t>(1u << (cluster % 8));
        if (bitmapData[byteIdx] & bitMask)
            ++allocatedClusters;
    }

    const uint64_t totalBytesToCopy = allocatedClusters * bytesPerCluster;
    const uint32_t maxSectorSize = std::max(srcSectorSize, dstSectorSize);
    const uint32_t alignedBufSize =
        (bufferSize / maxSectorSize) * maxSectorSize;

    if (alignedBufSize == 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Buffer size too small for sector alignment");
    }

    // Number of clusters we can batch into one I/O operation
    const uint32_t clustersPerChunk = alignedBufSize / bytesPerCluster;

    LARGE_INTEGER startTime, perfFreq;
    ::QueryPerformanceFrequency(&perfFreq);
    ::QueryPerformanceCounter(&startTime);

    uint64_t bytesTransferred = 0;
    int64_t cluster = 0;

    // Also need to zero out the destination for unallocated regions.
    // We write zeros for ranges of unallocated clusters.
    std::vector<uint8_t> zeroBuf(alignedBufSize, 0);

    while (cluster < totalClusters)
    {
        if (isCancelRequested())
        {
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                "Clone canceled by user");
        }

        // Find the next run of allocated clusters
        const size_t byteIdx = static_cast<size_t>(cluster / 8);
        const uint8_t bitMask = static_cast<uint8_t>(1u << (cluster % 8));
        const bool isAllocated = (bitmapData[byteIdx] & bitMask) != 0;

        if (!isAllocated)
        {
            // Write zeros to destination for this cluster
            const uint64_t clusterDiskOffset =
                srcOffsetBytes + static_cast<uint64_t>(cluster) * bytesPerCluster;
            const uint64_t dstClusterOffset =
                dstOffsetBytes + static_cast<uint64_t>(cluster) * bytesPerCluster;

            // Find how many consecutive unallocated clusters we have
            int64_t runLen = 0;
            while (cluster + runLen < totalClusters &&
                   runLen < static_cast<int64_t>(clustersPerChunk))
            {
                const size_t bi = static_cast<size_t>((cluster + runLen) / 8);
                const uint8_t bm =
                    static_cast<uint8_t>(1u << ((cluster + runLen) % 8));
                if (bitmapData[bi] & bm)
                    break;
                ++runLen;
            }

            // Write zeros to destination for unallocated range
            const uint64_t zeroBytes =
                static_cast<uint64_t>(runLen) * bytesPerCluster;
            uint64_t zeroRemaining = zeroBytes;
            uint64_t zeroPos = dstClusterOffset;

            while (zeroRemaining > 0)
            {
                const uint64_t writeChunk = std::min(
                    static_cast<uint64_t>(alignedBufSize), zeroRemaining);
                const SectorOffset dstLba = zeroPos / dstSectorSize;
                const SectorCount dstSectors =
                    static_cast<SectorCount>(writeChunk / dstSectorSize);

                auto writeResult = dst.writeSectors(
                    dstLba, zeroBuf.data(), dstSectors, dstSectorSize);
                if (writeResult.isError())
                    return writeResult.error();

                zeroPos += writeChunk;
                zeroRemaining -= writeChunk;
            }

            cluster += runLen;
            continue;
        }

        // Find how many consecutive allocated clusters we have
        int64_t runLen = 0;
        while (cluster + runLen < totalClusters &&
               runLen < static_cast<int64_t>(clustersPerChunk))
        {
            const size_t bi = static_cast<size_t>((cluster + runLen) / 8);
            const uint8_t bm =
                static_cast<uint8_t>(1u << ((cluster + runLen) % 8));
            if (!(bitmapData[bi] & bm))
                break;
            ++runLen;
        }

        // Copy this run of allocated clusters
        const uint64_t runBytes = static_cast<uint64_t>(runLen) * bytesPerCluster;
        const uint64_t srcClusterOffset =
            srcOffsetBytes + static_cast<uint64_t>(cluster) * bytesPerCluster;
        const uint64_t dstClusterOffset =
            dstOffsetBytes + static_cast<uint64_t>(cluster) * bytesPerCluster;

        // Read from source
        const SectorOffset srcLba = srcClusterOffset / srcSectorSize;
        const SectorCount srcSectors =
            static_cast<SectorCount>(runBytes / srcSectorSize);

        // May need to break into multiple reads if run is larger than buffer
        uint64_t runRemaining = runBytes;
        uint64_t srcRunPos = srcClusterOffset;
        uint64_t dstRunPos = dstClusterOffset;

        while (runRemaining > 0)
        {
            if (isCancelRequested())
            {
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                    "Clone canceled by user");
            }

            const uint64_t chunkBytes = std::min(
                static_cast<uint64_t>(alignedBufSize), runRemaining);

            const SectorOffset readLba = srcRunPos / srcSectorSize;
            const SectorCount readSectors =
                static_cast<SectorCount>(chunkBytes / srcSectorSize);

            auto readResult = src.readSectors(readLba, readSectors, srcSectorSize);
            if (readResult.isError())
                return readResult.error();

            const auto& data = readResult.value();

            // Write to destination
            const SectorOffset writeLba = dstRunPos / dstSectorSize;
            const SectorCount writeSectors =
                static_cast<SectorCount>(
                    ((data.size() + dstSectorSize - 1) / dstSectorSize));

            auto writeResult = dst.writeSectors(
                writeLba, data.data(), writeSectors, dstSectorSize);
            if (writeResult.isError())
                return writeResult.error();

            srcRunPos += chunkBytes;
            dstRunPos += chunkBytes;
            runRemaining -= chunkBytes;
            bytesTransferred += chunkBytes;

            if (!reportProgress(progressCb, CloneProgress::Phase::Cloning,
                                bytesTransferred, totalBytesToCopy,
                                startTime, perfFreq))
            {
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                    "Clone canceled by user");
            }
        }

        cluster += runLen;
    }

    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// Verification: read back both source and destination in chunks and
// compare SHA-256 hashes chunk by chunk.
// ---------------------------------------------------------------------------
Result<void> DiskCloner::verifyClone(
    RawDiskHandle& src, uint32_t srcSectorSize,
    RawDiskHandle& dst, uint32_t dstSectorSize,
    uint64_t srcOffsetBytes, uint64_t lengthBytes, uint64_t dstOffsetBytes,
    uint32_t bufferSize, CloneProgressCallback progressCb)
{
    const uint32_t maxSectorSize = std::max(srcSectorSize, dstSectorSize);
    const uint32_t alignedBufSize =
        (bufferSize / maxSectorSize) * maxSectorSize;

    if (alignedBufSize == 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Buffer size too small for sector alignment");
    }

    LARGE_INTEGER startTime, perfFreq;
    ::QueryPerformanceFrequency(&perfFreq);
    ::QueryPerformanceCounter(&startTime);

    uint64_t bytesRemaining = lengthBytes;
    uint64_t bytesVerified = 0;
    uint64_t srcPos = srcOffsetBytes;
    uint64_t dstPos = dstOffsetBytes;

    while (bytesRemaining > 0)
    {
        if (isCancelRequested())
        {
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                "Verification canceled by user");
        }

        const uint64_t chunkBytes = std::min(
            static_cast<uint64_t>(alignedBufSize), bytesRemaining);

        // Read source chunk
        const SectorOffset srcLba = srcPos / srcSectorSize;
        const SectorCount srcSectors =
            static_cast<SectorCount>(
                (chunkBytes + srcSectorSize - 1) / srcSectorSize);

        auto srcRead = src.readSectors(srcLba, srcSectors, srcSectorSize);
        if (srcRead.isError())
            return srcRead.error();

        // Read destination chunk
        const SectorOffset dstLba = dstPos / dstSectorSize;
        const SectorCount dstSectors =
            static_cast<SectorCount>(
                (chunkBytes + dstSectorSize - 1) / dstSectorSize);

        auto dstRead = dst.readSectors(dstLba, dstSectors, dstSectorSize);
        if (dstRead.isError())
            return dstRead.error();

        // Compare the relevant portion (up to chunkBytes)
        const size_t compareLen = static_cast<size_t>(chunkBytes);

        if (srcRead.value().size() < compareLen ||
            dstRead.value().size() < compareLen)
        {
            return ErrorInfo::fromCode(ErrorCode::ImageChecksumMismatch,
                "Verification read returned fewer bytes than expected");
        }

        if (std::memcmp(srcRead.value().data(),
                        dstRead.value().data(), compareLen) != 0)
        {
            std::ostringstream oss;
            oss << "Verification mismatch at offset "
                << srcPos << " (chunk size " << compareLen << " bytes)";
            return ErrorInfo::fromCode(ErrorCode::ImageChecksumMismatch,
                oss.str());
        }

        srcPos += chunkBytes;
        dstPos += chunkBytes;
        bytesVerified += chunkBytes;
        bytesRemaining -= chunkBytes;

        if (!reportProgress(progressCb, CloneProgress::Phase::Verifying,
                            bytesVerified, lengthBytes, startTime, perfFreq))
        {
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                "Verification canceled by user");
        }
    }

    return Result<void>::ok();
}

} // namespace spw
