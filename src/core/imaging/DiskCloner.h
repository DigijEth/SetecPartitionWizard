#pragma once

// DiskCloner — Sector-level disk and partition cloning engine.
// Supports raw (sector-by-sector) and smart (filesystem-aware) cloning,
// mismatched sector size handling, verification passes, and progress reporting.
// DISCLAIMER: This code is for authorized disk utility software only.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

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

// Progress info reported during cloning
struct CloneProgress
{
    uint64_t bytesTransferred = 0;
    uint64_t totalBytes = 0;
    double speedBytesPerSec = 0.0;
    double etaSeconds = 0.0;
    double percentComplete = 0.0;

    // Phase tracking
    enum class Phase
    {
        Preparing,
        Cloning,
        Verifying,
        Complete,
        Failed,
    };
    Phase phase = Phase::Preparing;
};

// Callback: return false to cancel the operation
using CloneProgressCallback = std::function<bool(const CloneProgress& progress)>;

// Cloning mode
enum class CloneMode
{
    // Sector-by-sector: copies every sector including free space.
    // Works for any filesystem or raw data. Slowest but most faithful.
    Raw,

    // Smart: reads filesystem allocation bitmap and skips unallocated sectors.
    // Only works for NTFS (via FSCTL_GET_VOLUME_BITMAP). Falls back to Raw
    // for unsupported filesystems.
    Smart,
};

// Configuration for a clone operation
struct CloneConfig
{
    // Source disk (must be opened read-only or read-write)
    DiskId sourceDiskId = -1;

    // Destination disk (will be opened read-write)
    DiskId destDiskId = -1;

    // If set, clone only this byte range (for partition cloning).
    // If both are 0, the entire source disk is cloned.
    uint64_t sourceOffsetBytes = 0;
    uint64_t sourceLengthBytes = 0;  // 0 = entire disk
    uint64_t destOffsetBytes = 0;

    // Cloning strategy
    CloneMode mode = CloneMode::Raw;

    // Verify after cloning by reading back and comparing hashes
    bool verifyAfterClone = true;

    // I/O buffer size (default 4 MiB)
    uint32_t bufferSize = 4 * 1024 * 1024;

    // Volume letters on the destination disk to lock/dismount before writing.
    // If empty, the cloner will attempt to auto-detect volumes.
    std::vector<wchar_t> destVolumeLetters;

    // If true, force clone even if destination is smaller than source
    // (truncates data — dangerous, but useful for known-smaller content)
    bool allowTruncation = false;
};

class DiskCloner
{
public:
    DiskCloner() = default;
    ~DiskCloner() = default;

    // Non-copyable
    DiskCloner(const DiskCloner&) = delete;
    DiskCloner& operator=(const DiskCloner&) = delete;

    // Execute a clone operation. Blocks until complete or canceled.
    Result<void> clone(const CloneConfig& config,
                       CloneProgressCallback progressCb = nullptr);

    // Request cancellation (thread-safe)
    void requestCancel();

    // Check if a cancel has been requested
    bool isCancelRequested() const;

private:
    std::atomic<bool> m_cancelRequested{false};

    // Internal: lock and dismount destination volumes
    Result<std::vector<HANDLE>> lockDestinationVolumes(
        const std::vector<wchar_t>& volumeLetters);

    // Internal: unlock previously locked volumes
    void unlockVolumes(std::vector<HANDLE>& lockedHandles);

    // Internal: perform raw sector-by-sector copy
    Result<void> cloneRaw(
        RawDiskHandle& src, uint32_t srcSectorSize,
        RawDiskHandle& dst, uint32_t dstSectorSize,
        uint64_t srcOffsetBytes, uint64_t lengthBytes, uint64_t dstOffsetBytes,
        uint32_t bufferSize, CloneProgressCallback progressCb);

    // Internal: perform smart copy (NTFS bitmap-aware)
    Result<void> cloneSmart(
        RawDiskHandle& src, uint32_t srcSectorSize,
        RawDiskHandle& dst, uint32_t dstSectorSize,
        uint64_t srcOffsetBytes, uint64_t lengthBytes, uint64_t dstOffsetBytes,
        uint32_t bufferSize, CloneProgressCallback progressCb);

    // Internal: verification pass — read back both sides and compare
    Result<void> verifyClone(
        RawDiskHandle& src, uint32_t srcSectorSize,
        RawDiskHandle& dst, uint32_t dstSectorSize,
        uint64_t srcOffsetBytes, uint64_t lengthBytes, uint64_t dstOffsetBytes,
        uint32_t bufferSize, CloneProgressCallback progressCb);

    // Internal: report progress with speed and ETA calculation
    bool reportProgress(CloneProgressCallback& cb,
                        CloneProgress::Phase phase,
                        uint64_t bytesTransferred, uint64_t totalBytes,
                        LARGE_INTEGER startTime, LARGE_INTEGER perfFreq);
};

} // namespace spw
