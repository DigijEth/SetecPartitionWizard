#pragma once

// SdCardAnalyzer — Deep analysis, counterfeit detection, speed testing,
// and health checking for SD/MMC cards on Windows.
//
// Counterfeit detection works by probing actual writable capacity — fake cards
// (capacity-spoofed NAND) report large sizes but silently wrap writes back to
// the beginning of the real NAND. We write unique signatures at geometrically
// distributed offsets, then read them back and count mismatches.
//
// DISCLAIMER: This code is for authorized disk utility software only.

#include <windows.h>
#include <winioctl.h>

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../disk/DiskEnumerator.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spw
{

// ============================================================================
// Card identification info pulled from device descriptors
// ============================================================================
struct SdCardIdentity
{
    // From STORAGE_DEVICE_DESCRIPTOR (via DeviceIoControl)
    std::wstring vendorId;          // e.g. L"SanDisk"
    std::wstring productId;         // e.g. L"SD Card" or L"Storage Device"
    std::wstring productRevision;
    std::wstring serialNumberStr;

    // SD-specific CID fields (when available via IOCTL_SFFDISK_QUERY_DEVICE_PROTOCOL)
    uint8_t     manufacturerId = 0; // CID[127:120] — MID byte
    uint16_t    oemId = 0;          // CID[119:104] — OEM/Application ID
    std::string productName;        // CID[103:64] — Product name (5 ASCII chars)
    uint8_t     productRevision8 = 0;
    uint32_t    serialNumber32 = 0; // CID[55:24]
    bool        cidValid = false;   // True if CID was readable

    // Reported speed / class (from STORAGE_DEVICE_DESCRIPTOR extended, if available)
    std::wstring busType;           // e.g. L"SD", L"MMC", L"USB"
};

// Known legitimate manufacturer IDs (CID MID byte)
// https://www.cameramemoryspeed.com/sd-memory-card-faq/reading-sd-card-cid-serial-psn-internal-information/
struct KnownManufacturer
{
    uint8_t     mid;
    const char* name;
};

// ============================================================================
// Counterfeit check result
// ============================================================================
enum class CounterfeitVerdict
{
    Genuine,            // Passed all checks
    LikelySpoofed,      // Capacity mismatch found — almost certainly fake
    Suspicious,         // Some anomalies but not conclusive
    TestFailed,         // Could not complete test (write-protected, no access)
    Untested
};

struct CounterfeitResult
{
    CounterfeitVerdict verdict = CounterfeitVerdict::Untested;

    uint64_t reportedCapacityBytes = 0;  // What the card claims
    uint64_t verifiedCapacityBytes = 0;  // What we could actually write and read back
    uint64_t firstBadOffsetBytes = 0;    // Where the first mismatch was found (0 = none)

    int      probeCount = 0;             // How many offsets were probed
    int      failCount = 0;              // How many probes failed
    double   failPercent = 0.0;

    std::string   manufacturerName;      // From known MID table (or "Unknown")
    bool          unknownManufacturer = false;
    bool          suspiciousVendorString = false; // Generic vendor like "Generic" or "Storage"

    std::string   summaryMessage;        // Human-readable verdict
};

// ============================================================================
// Speed benchmark result
// ============================================================================
struct SdSpeedResult
{
    double seqReadMBps = 0.0;
    double seqWriteMBps = 0.0;
    double randRead4kIOPS = 0.0;
    double randWrite4kIOPS = 0.0;
    bool   writeProtected = false;
    std::string notes;
};

// ============================================================================
// Health / surface scan result
// ============================================================================
struct SdHealthResult
{
    uint64_t totalSectors = 0;
    uint64_t sectorsScanned = 0;
    uint64_t badSectors = 0;
    uint64_t slowSectors = 0;      // Readable but unusually slow (>500ms per sector)
    bool     complete = false;
    std::string summary;
};

// ============================================================================
// Progress callbacks
// ============================================================================
using SdAnalysisProgress = std::function<void(const std::string& stage, int pct)>;
using SdSectorProgress   = std::function<void(uint64_t currentSector, uint64_t totalSectors,
                                               uint64_t badFound, int pct)>;

// ============================================================================
// SdCardAnalyzer
// ============================================================================
class SdCardAnalyzer
{
public:
    // Read device identity / manufacturer info from descriptor
    static Result<SdCardIdentity> queryIdentity(DiskId diskId);

    // Counterfeit detection — writes unique signatures at probe offsets and
    // verifies them. Minimally invasive: restores original data if possible.
    // WARNING: overwrites probe sectors. Only use on cards you own.
    static Result<CounterfeitResult> checkCounterfeit(
        DiskId diskId,
        SdAnalysisProgress progress = nullptr);

    // Sequential + random speed benchmark
    static Result<SdSpeedResult> benchmarkSpeed(
        DiskId diskId,
        uint64_t testSizeBytes = 64 * 1024 * 1024, // 64 MB default
        SdAnalysisProgress progress = nullptr);

    // Surface scan — read every sector and flag errors/slow sectors
    static Result<SdHealthResult> surfaceScan(
        DiskId diskId,
        std::atomic<bool>* cancelFlag = nullptr,
        SdSectorProgress progress = nullptr);

    // Check write-protection status
    static bool isWriteProtected(DiskId diskId);

    // Look up manufacturer name from MID byte
    static const char* manufacturerName(uint8_t mid);

private:
    // Write a unique 512-byte signature block to a sector and read it back
    static bool probeOffset(HANDLE hDisk, uint64_t offsetBytes,
                            uint32_t sectorSize, uint64_t diskId);

    // Generate a deterministic signature for a given offset (so we can verify)
    static void makeSignature(uint8_t* buf, uint32_t sectorSize,
                              uint64_t offsetBytes, uint64_t diskId);
};

} // namespace spw
