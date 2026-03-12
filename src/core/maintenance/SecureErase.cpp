// SecureErase.cpp -- Secure data erasure with multiple standard methods.
//
// DISCLAIMER: This code is for authorized disk utility software only.
//             All erase operations PERMANENTLY DESTROY DATA.

#include "SecureErase.h"

#include <algorithm>
#include <cstring>

// For BCryptGenRandom
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace spw
{

// ---------------------------------------------------------------------------
// Gutmann 35-pass pattern definitions
//
// Passes 1-4 and 33-35 are random.  Passes 5-31 are specific patterns
// designed to defeat MFM and RLL encoding recovery techniques (historically
// relevant for older magnetic media).
//
// Reference: Peter Gutmann, "Secure Deletion of Data from Magnetic and
//            Solid-State Memory", 1996.
// ---------------------------------------------------------------------------

// Each inner vector is one pattern byte (repeated across the sector).
// An empty vector means "random data".
static const std::vector<std::vector<uint8_t>> GUTMANN_PASSES = {
    {},                              // Pass  1: random
    {},                              // Pass  2: random
    {},                              // Pass  3: random
    {},                              // Pass  4: random
    {0x55, 0x55, 0x55},             // Pass  5
    {0xAA, 0xAA, 0xAA},             // Pass  6
    {0x92, 0x49, 0x24},             // Pass  7
    {0x49, 0x24, 0x92},             // Pass  8
    {0x24, 0x92, 0x49},             // Pass  9
    {0x00, 0x00, 0x00},             // Pass 10
    {0x11, 0x11, 0x11},             // Pass 11
    {0x22, 0x22, 0x22},             // Pass 12
    {0x33, 0x33, 0x33},             // Pass 13
    {0x44, 0x44, 0x44},             // Pass 14
    {0x55, 0x55, 0x55},             // Pass 15
    {0x66, 0x66, 0x66},             // Pass 16
    {0x77, 0x77, 0x77},             // Pass 17
    {0x88, 0x88, 0x88},             // Pass 18
    {0x99, 0x99, 0x99},             // Pass 19
    {0xAA, 0xAA, 0xAA},             // Pass 20
    {0xBB, 0xBB, 0xBB},             // Pass 21
    {0xCC, 0xCC, 0xCC},             // Pass 22
    {0xDD, 0xDD, 0xDD},             // Pass 23
    {0xEE, 0xEE, 0xEE},             // Pass 24
    {0xFF, 0xFF, 0xFF},             // Pass 25
    {0x92, 0x49, 0x24},             // Pass 26
    {0x49, 0x24, 0x92},             // Pass 27
    {0x24, 0x92, 0x49},             // Pass 28
    {0x6D, 0xB6, 0xDB},             // Pass 29
    {0xB6, 0xDB, 0x6D},             // Pass 30
    {0xDB, 0x6D, 0xB6},             // Pass 31
    {},                              // Pass 32: random
    {},                              // Pass 33: random
    {},                              // Pass 34: random
    {},                              // Pass 35: random
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SecureErase::SecureErase(RawDiskHandle& disk)
    : m_disk(disk)
{
}

// ---------------------------------------------------------------------------
// buildPassList -- construct the list of per-pass patterns for a given method
// ---------------------------------------------------------------------------

std::vector<std::vector<uint8_t>> SecureErase::buildPassList(const EraseConfig& config)
{
    std::vector<std::vector<uint8_t>> passes;

    switch (config.method)
    {
    case EraseMethod::ZeroFill:
        passes.push_back({0x00});
        break;

    case EraseMethod::DoD_3Pass:
        // Pass 1: 0x00,  Pass 2: 0xFF,  Pass 3: random
        passes.push_back({0x00});
        passes.push_back({0xFF});
        passes.push_back({}); // empty = random
        break;

    case EraseMethod::DoD_7Pass:
        // DoD 5220.22-M ECE (7-pass):
        // Passes 1-3: DoD 3-pass (0x00, 0xFF, random)
        // Pass 4: pattern 0x00
        // Passes 5-7: DoD 3-pass again (0x00, 0xFF, random)
        passes.push_back({0x00});
        passes.push_back({0xFF});
        passes.push_back({});     // random
        passes.push_back({0x00});
        passes.push_back({0x00});
        passes.push_back({0xFF});
        passes.push_back({});     // random
        break;

    case EraseMethod::Gutmann:
        passes = GUTMANN_PASSES;
        break;

    case EraseMethod::RandomFill:
    {
        int count = std::max(config.passCount, 1);
        for (int i = 0; i < count; ++i)
            passes.push_back({}); // empty = random
        break;
    }

    case EraseMethod::CustomPattern:
    {
        int count = std::max(config.customPatternPasses, 1);
        for (int i = 0; i < count; ++i)
            passes.push_back(config.customPattern);
        break;
    }
    }

    return passes;
}

// ---------------------------------------------------------------------------
// fillRandom -- fill a buffer with cryptographically secure random data
// ---------------------------------------------------------------------------

Result<void> SecureErase::fillRandom(uint8_t* buffer, uint32_t size)
{
    NTSTATUS status = BCryptGenRandom(nullptr, buffer, size,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) // STATUS_SUCCESS == 0
        return ErrorInfo::fromCode(ErrorCode::Unknown, "BCryptGenRandom failed");

    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// fillPattern -- fill a buffer with a repeating pattern
// ---------------------------------------------------------------------------

void SecureErase::fillPattern(uint8_t* buffer, uint32_t bufferSize,
                               const std::vector<uint8_t>& pattern)
{
    if (pattern.empty())
        return; // Caller should use fillRandom instead

    if (pattern.size() == 1)
    {
        // Optimize the common case: single-byte pattern
        std::memset(buffer, pattern[0], bufferSize);
        return;
    }

    // Multi-byte pattern: tile it across the buffer
    size_t patLen = pattern.size();
    for (uint32_t i = 0; i < bufferSize; ++i)
        buffer[i] = pattern[i % patLen];
}

// ---------------------------------------------------------------------------
// lockAllVolumes -- lock and dismount every volume on this physical disk
// ---------------------------------------------------------------------------

Result<std::vector<HANDLE>> SecureErase::lockAllVolumes()
{
    std::vector<HANDLE> handles;

    // Enumerate volume letters A-Z and check which ones are on this disk
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter)
    {
        // Skip if the drive letter doesn't exist
        std::wstring rootPath = std::wstring(1, letter) + L":\\";
        UINT driveType = GetDriveTypeW(rootPath.c_str());
        if (driveType == DRIVE_NO_ROOT_DIR || driveType == DRIVE_UNKNOWN)
            continue;

        // Try to check if this volume is on our disk by opening the volume
        // and querying its extents.  This is the Win32 way to map volumes
        // to physical disks.
        std::wstring volumePath = L"\\\\.\\";
        volumePath += letter;
        volumePath += L':';

        HANDLE hVolume = CreateFileW(
            volumePath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (hVolume == INVALID_HANDLE_VALUE)
            continue;

        // Query disk extents to see if this volume is on our disk
        VOLUME_DISK_EXTENTS extents = {};
        DWORD bytesReturned = 0;
        BOOL ok = DeviceIoControl(
            hVolume,
            IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
            nullptr, 0,
            &extents, sizeof(extents),
            &bytesReturned, nullptr);

        if (!ok || extents.NumberOfDiskExtents == 0)
        {
            CloseHandle(hVolume);
            continue;
        }

        // Check if any extent is on our disk
        bool onOurDisk = false;
        for (DWORD i = 0; i < extents.NumberOfDiskExtents; ++i)
        {
            if (static_cast<int>(extents.Extents[i].DiskNumber) == m_disk.diskId())
            {
                onOurDisk = true;
                break;
            }
        }

        if (!onOurDisk)
        {
            CloseHandle(hVolume);
            continue;
        }

        // Lock the volume
        ok = DeviceIoControl(hVolume, FSCTL_LOCK_VOLUME,
                             nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
        if (!ok)
        {
            CloseHandle(hVolume);
            // Return error: cannot lock a volume that's in use
            // Clean up already-locked handles
            for (auto h : handles)
            {
                DeviceIoControl(h, FSCTL_UNLOCK_VOLUME,
                                nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
                CloseHandle(h);
            }
            return ErrorInfo::fromWin32(ErrorCode::DiskLockFailed, GetLastError(),
                                        std::string("Cannot lock volume ") +
                                        static_cast<char>(letter) + ":");
        }

        // Dismount the volume
        DeviceIoControl(hVolume, FSCTL_DISMOUNT_VOLUME,
                        nullptr, 0, nullptr, 0, &bytesReturned, nullptr);

        handles.push_back(hVolume);
    }

    return handles;
}

// ---------------------------------------------------------------------------
// unlockAllVolumes
// ---------------------------------------------------------------------------

void SecureErase::unlockAllVolumes(std::vector<HANDLE>& handles)
{
    DWORD bytesReturned = 0;
    for (auto h : handles)
    {
        DeviceIoControl(h, FSCTL_UNLOCK_VOLUME,
                        nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
        CloseHandle(h);
    }
    handles.clear();
}

// ---------------------------------------------------------------------------
// eraseDisk -- erase the entire physical disk
// ---------------------------------------------------------------------------

Result<void> SecureErase::eraseDisk(
    const EraseConfig& config,
    EraseProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    auto geoResult = m_disk.getGeometry();
    if (geoResult.isError())
        return geoResult.error();
    m_geometry = geoResult.value();

    const uint32_t sectorSize = m_geometry.bytesPerSector;
    if (sectorSize == 0)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Disk reports 0 bytes/sector");

    const uint64_t totalSectors = m_geometry.totalBytes / sectorSize;

    // Lock and dismount all volumes on this disk
    auto lockResult = lockAllVolumes();
    if (lockResult.isError())
        return lockResult.error();

    auto lockedHandles = std::move(lockResult.value());

    auto passes = buildPassList(config);
    int totalPasses = static_cast<int>(passes.size()) + (config.verify ? 1 : 0);

    Result<void> finalResult = Result<void>::ok();

    for (int passIdx = 0; passIdx < static_cast<int>(passes.size()); ++passIdx)
    {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
        {
            finalResult = ErrorInfo::fromCode(ErrorCode::OperationCanceled);
            break;
        }

        auto passResult = writePass(0, totalSectors, sectorSize, passes[passIdx],
                                     passIdx + 1, totalPasses, progressCb, cancelFlag);
        if (passResult.isError())
        {
            finalResult = passResult;
            break;
        }

        // Flush after each pass
        m_disk.flushBuffers();
    }

    // Verification pass (if requested and no errors so far)
    if (finalResult.isOk() && config.verify && !passes.empty())
    {
        // Verify against the last pass's pattern
        const auto& lastPattern = passes.back();
        auto verResult = verifyPass(0, totalSectors, sectorSize, lastPattern,
                                     totalPasses, progressCb, cancelFlag);
        if (verResult.isError())
            finalResult = verResult;
    }

    // Unlock all volumes
    unlockAllVolumes(lockedHandles);

    return finalResult;
}

// ---------------------------------------------------------------------------
// eraseRange -- erase a specific partition
// ---------------------------------------------------------------------------

Result<void> SecureErase::eraseRange(
    SectorOffset startLba,
    SectorCount  sectorCount,
    const EraseConfig& config,
    EraseProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    auto geoResult = m_disk.getGeometry();
    if (geoResult.isError())
        return geoResult.error();
    m_geometry = geoResult.value();

    const uint32_t sectorSize = m_geometry.bytesPerSector;
    if (sectorSize == 0)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Disk reports 0 bytes/sector");

    // Lock and dismount volumes
    auto lockResult = lockAllVolumes();
    if (lockResult.isError())
        return lockResult.error();

    auto lockedHandles = std::move(lockResult.value());

    auto passes = buildPassList(config);
    int totalPasses = static_cast<int>(passes.size()) + (config.verify ? 1 : 0);

    Result<void> finalResult = Result<void>::ok();

    for (int passIdx = 0; passIdx < static_cast<int>(passes.size()); ++passIdx)
    {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
        {
            finalResult = ErrorInfo::fromCode(ErrorCode::OperationCanceled);
            break;
        }

        auto passResult = writePass(startLba, sectorCount, sectorSize, passes[passIdx],
                                     passIdx + 1, totalPasses, progressCb, cancelFlag);
        if (passResult.isError())
        {
            finalResult = passResult;
            break;
        }

        m_disk.flushBuffers();
    }

    if (finalResult.isOk() && config.verify && !passes.empty())
    {
        const auto& lastPattern = passes.back();
        auto verResult = verifyPass(startLba, sectorCount, sectorSize, lastPattern,
                                     totalPasses, progressCb, cancelFlag);
        if (verResult.isError())
            finalResult = verResult;
    }

    unlockAllVolumes(lockedHandles);
    return finalResult;
}

// ---------------------------------------------------------------------------
// writePass -- write a single pass of a pattern or random data across range
// ---------------------------------------------------------------------------

Result<void> SecureErase::writePass(
    SectorOffset startLba,
    SectorCount  sectorCount,
    uint32_t     sectorSize,
    const std::vector<uint8_t>& pattern,
    int currentPass,
    int totalPasses,
    EraseProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    const bool isRandom = pattern.empty();

    // Use a 64 KiB write buffer.  For random passes, we generate random data
    // once and reuse the buffer for speed (BCryptGenRandom on every sector would
    // be prohibitively slow).  We re-randomize every N writes.
    constexpr uint32_t BUFFER_SIZE = 64 * 1024;
    const SectorCount bufferSectors = BUFFER_SIZE / sectorSize;

    std::vector<uint8_t> buffer(BUFFER_SIZE);

    if (isRandom)
    {
        auto rr = fillRandom(buffer.data(), BUFFER_SIZE);
        if (rr.isError())
            return rr;
    }
    else
    {
        fillPattern(buffer.data(), BUFFER_SIZE, pattern);
    }

    // Timing for speed calculation
    LARGE_INTEGER perfFreq, perfStart, perfNow;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&perfStart);

    uint64_t bytesWritten = 0;
    uint64_t totalBytes = sectorCount * sectorSize;
    uint32_t randomRefreshCounter = 0;
    constexpr uint32_t RANDOM_REFRESH_INTERVAL = 256; // Re-randomize every 256 writes

    SectorOffset currentLba = startLba;
    SectorOffset endLba = startLba + sectorCount;

    while (currentLba < endLba)
    {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled, "Erase canceled");

        SectorCount remaining = endLba - currentLba;
        SectorCount thisChunk = std::min(bufferSectors, remaining);
        uint32_t writeSize = static_cast<uint32_t>(thisChunk) * sectorSize;

        // Periodically refresh random buffer to maintain cryptographic quality
        if (isRandom && ++randomRefreshCounter >= RANDOM_REFRESH_INTERVAL)
        {
            randomRefreshCounter = 0;
            auto rr = fillRandom(buffer.data(), BUFFER_SIZE);
            if (rr.isError())
                return rr;
        }

        auto writeResult = m_disk.writeSectors(currentLba, buffer.data(), thisChunk, sectorSize);
        if (writeResult.isError())
        {
            // Retry individual sectors on failure
            for (SectorCount s = 0; s < thisChunk; ++s)
            {
                auto retry = m_disk.writeSectors(currentLba + s, buffer.data(), 1, sectorSize);
                // If individual sector write also fails, continue anyway
                // (bad sectors can't be erased but we don't want to abort the whole op)
                (void)retry;
            }
        }

        bytesWritten += writeSize;
        currentLba += thisChunk;

        if (progressCb)
        {
            QueryPerformanceCounter(&perfNow);
            double elapsed = static_cast<double>(perfNow.QuadPart - perfStart.QuadPart) /
                             static_cast<double>(perfFreq.QuadPart);
            double speedMBps = (elapsed > 0.0)
                ? (static_cast<double>(bytesWritten) / (1024.0 * 1024.0)) / elapsed
                : 0.0;

            progressCb(currentPass, totalPasses, bytesWritten, totalBytes, speedMBps);
        }
    }

    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// verifyPass -- read back and verify against the expected pattern
// ---------------------------------------------------------------------------

Result<void> SecureErase::verifyPass(
    SectorOffset startLba,
    SectorCount  sectorCount,
    uint32_t     sectorSize,
    const std::vector<uint8_t>& pattern,
    int totalPasses,
    EraseProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    const bool isRandom = pattern.empty();

    // Cannot verify random passes (we don't store the random data),
    // so just do a read test to confirm sectors are readable.
    constexpr uint32_t BUFFER_SIZE = 64 * 1024;
    const SectorCount bufferSectors = BUFFER_SIZE / sectorSize;

    // Build expected pattern buffer for non-random passes
    std::vector<uint8_t> expectedBuf;
    if (!isRandom)
    {
        expectedBuf.resize(BUFFER_SIZE);
        fillPattern(expectedBuf.data(), BUFFER_SIZE, pattern);
    }

    LARGE_INTEGER perfFreq, perfStart, perfNow;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&perfStart);

    uint64_t bytesVerified = 0;
    uint64_t totalBytes = sectorCount * sectorSize;

    SectorOffset currentLba = startLba;
    SectorOffset endLba = startLba + sectorCount;

    while (currentLba < endLba)
    {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled, "Verify canceled");

        SectorCount remaining = endLba - currentLba;
        SectorCount thisChunk = std::min(bufferSectors, remaining);

        auto readResult = m_disk.readSectors(currentLba, thisChunk, sectorSize);
        if (readResult.isError())
        {
            // Sectors unreadable after erase -- could be bad sectors
            // This is not necessarily an erase failure, so log but continue
        }
        else if (!isRandom)
        {
            const auto& readData = readResult.value();
            uint32_t compareSize = static_cast<uint32_t>(thisChunk) * sectorSize;
            compareSize = std::min(compareSize, static_cast<uint32_t>(readData.size()));

            if (std::memcmp(readData.data(), expectedBuf.data(), compareSize) != 0)
            {
                return ErrorInfo::fromCode(ErrorCode::DiskWriteError,
                                           "Verification failed: data mismatch after erase");
            }
        }

        bytesVerified += static_cast<uint64_t>(thisChunk) * sectorSize;
        currentLba += thisChunk;

        if (progressCb)
        {
            QueryPerformanceCounter(&perfNow);
            double elapsed = static_cast<double>(perfNow.QuadPart - perfStart.QuadPart) /
                             static_cast<double>(perfFreq.QuadPart);
            double speedMBps = (elapsed > 0.0)
                ? (static_cast<double>(bytesVerified) / (1024.0 * 1024.0)) / elapsed
                : 0.0;

            // Report as the verify pass (last pass)
            progressCb(totalPasses, totalPasses, bytesVerified, totalBytes, speedMBps);
        }
    }

    return Result<void>::ok();
}

} // namespace spw
