#pragma once

// SecureErase -- Securely overwrite disk data using standard erasure methods.
//
// Supported methods:
//   - Zero fill (1 pass)
//   - DoD 5220.22-M (3-pass): 0x00, 0xFF, random
//   - DoD 5220.22-M ECE (7-pass): extended version
//   - Gutmann (35-pass): full Gutmann pattern sequence
//   - Random fill (configurable N passes)
//   - Custom pattern (user-defined byte pattern, configurable passes)
//
// Each method includes an optional verification pass.  Before erasing,
// all volumes on the target disk/partition are locked and dismounted.
//
// Uses BCryptGenRandom for cryptographically secure random data.
//
// DISCLAIMER: This code is for authorized disk utility software only.
//             All erase operations PERMANENTLY DESTROY DATA and are
//             IRREVERSIBLE.  Use with extreme caution.

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

// Erasure method
enum class EraseMethod
{
    ZeroFill,      // 1 pass: all zeros
    DoD_3Pass,     // DoD 5220.22-M: 0x00, 0xFF, random
    DoD_7Pass,     // DoD 5220.22-M ECE: 7-pass extended
    Gutmann,       // Gutmann 35-pass
    RandomFill,    // N passes of CSPRNG data
    CustomPattern, // User-defined byte pattern
};

// Configuration for a secure erase operation
struct EraseConfig
{
    EraseMethod method = EraseMethod::ZeroFill;
    int         passCount = 1;        // Only used for RandomFill
    bool        verify = true;        // Verify after last pass
    std::vector<uint8_t> customPattern; // Only used for CustomPattern
    int customPatternPasses = 1;      // Passes for custom pattern
};

// Progress callback.
// Parameters: (currentPass, totalPasses, bytesWritten, totalBytes, speedMBps)
using EraseProgress = std::function<void(int      currentPass,
                                          int      totalPasses,
                                          uint64_t bytesWritten,
                                          uint64_t totalBytes,
                                          double   speedMBps)>;

class SecureErase
{
public:
    explicit SecureErase(RawDiskHandle& disk);

    // Erase the entire disk
    Result<void> eraseDisk(
        const EraseConfig& config,
        EraseProgress progressCb = nullptr,
        std::atomic<bool>* cancelFlag = nullptr);

    // Erase a specific partition (range of sectors)
    Result<void> eraseRange(
        SectorOffset startLba,
        SectorCount  sectorCount,
        const EraseConfig& config,
        EraseProgress progressCb = nullptr,
        std::atomic<bool>* cancelFlag = nullptr);

private:
    // Build the list of passes (pattern byte sequences) for the chosen method
    static std::vector<std::vector<uint8_t>> buildPassList(const EraseConfig& config);

    // Write a single pass of a given pattern across the range
    Result<void> writePass(
        SectorOffset startLba,
        SectorCount  sectorCount,
        uint32_t     sectorSize,
        const std::vector<uint8_t>& pattern, // Empty means random data
        int currentPass,
        int totalPasses,
        EraseProgress progressCb,
        std::atomic<bool>* cancelFlag);

    // Verification pass: read back and verify against expected pattern
    Result<void> verifyPass(
        SectorOffset startLba,
        SectorCount  sectorCount,
        uint32_t     sectorSize,
        const std::vector<uint8_t>& pattern,
        int totalPasses,
        EraseProgress progressCb,
        std::atomic<bool>* cancelFlag);

    // Fill a buffer with CSPRNG random data using BCryptGenRandom
    static Result<void> fillRandom(uint8_t* buffer, uint32_t size);

    // Fill a buffer with a repeating pattern
    static void fillPattern(uint8_t* buffer, uint32_t bufferSize,
                            const std::vector<uint8_t>& pattern);

    // Lock and dismount all volumes on this disk
    Result<std::vector<HANDLE>> lockAllVolumes();
    void unlockAllVolumes(std::vector<HANDLE>& handles);

    RawDiskHandle& m_disk;
    DiskGeometryInfo m_geometry = {};
};

} // namespace spw
