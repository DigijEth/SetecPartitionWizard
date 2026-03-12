#include "SdCardAnalyzer.h"

#include "../disk/RawDiskHandle.h"
#include "../disk/DiskEnumerator.h"
#include "../common/Logging.h"

#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <devguid.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <string>

namespace spw
{

// ============================================================================
// Known SD/MMC manufacturer IDs (CID MID byte)
// Source: SD Physical Layer Simplified Spec + community databases
// ============================================================================
static const KnownManufacturer kManufacturers[] = {
    { 0x01, "Panasonic" },
    { 0x02, "Toshiba / Kingston" },
    { 0x03, "SanDisk" },
    { 0x06, "Ritek" },
    { 0x09, "ATP Electronics" },
    { 0x11, "Verbatim" },
    { 0x12, "Dana Technology" },
    { 0x13, "Apricorn" },
    { 0x1B, "Samsung" },
    { 0x1D, "AData" },
    { 0x27, "Phison" },
    { 0x28, "Lexar Media" },
    { 0x30, "Silicon Power" },
    { 0x31, "Silicon Power" },
    { 0x33, "STMicroelectronics" },
    { 0x37, "Kingston" },
    { 0x38, "ISSI" },
    { 0x39, "Intenso" },
    { 0x3E, "Netlist" },
    { 0x41, "Kingston" },
    { 0x43, "Micron/Crucial" },
    { 0x45, "Team Group" },
    { 0x46, "Sony" },
    { 0x48, "Hynix" },
    { 0x49, "Lexar" },
    { 0x4E, "Transcend" },
    { 0x51, "Qimonda" },
    { 0x52, "Hynix" },
    { 0x56, "Unknown (likely clone)" },
    { 0x64, "Transcend" },
    { 0x65, "Kingston" },
    { 0x6F, "GreenHouse" },
    { 0x70, "Pny Technologies" },
    { 0x73, "GS Nanotech" },
    { 0x74, "Transcend" },
    { 0x76, "Patriot" },
    { 0x7F, "Unknown (likely clone)" },
    { 0x82, "Sony" },
    { 0x89, "Unknown (likely clone)" },
    { 0xAD, "Hynix" },
    { 0xCE, "Samsung" },
    { 0x00, nullptr }
};

const char* SdCardAnalyzer::manufacturerName(uint8_t mid)
{
    for (int i = 0; kManufacturers[i].name != nullptr; ++i)
    {
        if (kManufacturers[i].mid == mid)
            return kManufacturers[i].name;
    }
    return "Unknown";
}

// ============================================================================
// Query device identity
// ============================================================================
Result<SdCardIdentity> SdCardAnalyzer::queryIdentity(DiskId diskId)
{
    SdCardIdentity id;

    // Open the device
    std::wstring devPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(diskId);
    HANDLE hDev = CreateFileW(devPath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDev == INVALID_HANDLE_VALUE)
        return ErrorInfo::fromWin32(ErrorCode::DiskAccessDenied, GetLastError(),
                                    "Cannot open disk for identity query");

    // STORAGE_DEVICE_DESCRIPTOR query
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType  = PropertyStandardQuery;

    constexpr DWORD kBufSize = 4096;
    std::vector<uint8_t> buf(kBufSize, 0);
    DWORD returned = 0;

    if (DeviceIoControl(hDev, IOCTL_STORAGE_QUERY_PROPERTY,
                        &query, sizeof(query),
                        buf.data(), kBufSize, &returned, nullptr))
    {
        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());

        auto readStr = [&](DWORD offset) -> std::wstring {
            if (offset == 0 || offset >= returned) return {};
            const char* p = reinterpret_cast<const char*>(buf.data()) + offset;
            std::wstring out;
            while (*p && p < reinterpret_cast<const char*>(buf.data()) + returned)
                out += static_cast<wchar_t>(*p++);
            // Trim trailing spaces
            while (!out.empty() && out.back() == L' ') out.pop_back();
            return out;
        };

        id.vendorId         = readStr(desc->VendorIdOffset);
        id.productId        = readStr(desc->ProductIdOffset);
        id.productRevision  = readStr(desc->ProductRevisionOffset);
        id.serialNumberStr  = readStr(desc->SerialNumberOffset);

        // Bus type string
        switch (desc->BusType)
        {
        case BusTypeSd:   id.busType = L"SD";    break;
        case BusTypeMmc:  id.busType = L"MMC";   break;
        case BusTypeUsb:  id.busType = L"USB";   break;
        case BusTypeSata: id.busType = L"SATA";  break;
        case BusTypeNvme: id.busType = L"NVMe";  break;
        default:          id.busType = L"Unknown"; break;
        }
    }

    // Suspicious vendor detection
    // Generic, empty, or known-fake vendor strings
    auto vendorLow = id.vendorId;
    std::transform(vendorLow.begin(), vendorLow.end(), vendorLow.begin(), ::towlower);

    CloseHandle(hDev);
    return id;
}

// ============================================================================
// Signature generation — deterministic per (offset, diskId)
// ============================================================================
void SdCardAnalyzer::makeSignature(uint8_t* buf, uint32_t sectorSize,
                                    uint64_t offsetBytes, uint64_t diskSerial)
{
    // 8-byte magic header: "SPWSDCK!" + "FAKEDEAД"
    buf[0] = 0x53; buf[1] = 0x50; buf[2] = 0x57; buf[3] = 0x53; // "SPWS"
    buf[4] = 0xFA; buf[5] = 0x4B; buf[6] = 0xDE; buf[7] = 0xAD; // fake-DEAD

    // Encode offset in bytes 8..15 (little-endian)
    for (int i = 0; i < 8; ++i)
        buf[8 + i] = static_cast<uint8_t>((offsetBytes >> (i * 8)) & 0xFF);

    // Encode disk serial in bytes 16..23
    for (int i = 0; i < 8; ++i)
        buf[16 + i] = static_cast<uint8_t>((diskSerial >> (i * 8)) & 0xFF);

    // Fill remaining bytes with a pseudo-random pattern seeded from offset+serial
    uint64_t seed = offsetBytes ^ (diskSerial << 13) ^ 0xDEADBEEFCAFEBABEULL;
    for (uint32_t i = 24; i < sectorSize; ++i)
    {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = static_cast<uint8_t>(seed >> 56);
    }
}

// ============================================================================
// Probe a single offset — write phase (save original, write signature)
// ============================================================================
bool SdCardAnalyzer::probeOffset(HANDLE hDisk, uint64_t offsetBytes,
                                  uint32_t sectorSize, uint64_t diskSerial)
{
    // This is now used only as a verify-read for a previously written signature.
    // The write-all-then-read-all approach is in checkCounterfeit().
    offsetBytes = (offsetBytes / sectorSize) * sectorSize;

    std::vector<uint8_t> expectedBuf(sectorSize);
    std::vector<uint8_t> readBuf(sectorSize);

    makeSignature(expectedBuf.data(), sectorSize, offsetBytes, diskSerial);

    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offsetBytes);
    SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN);
    DWORD nRead = 0;
    if (!ReadFile(hDisk, readBuf.data(), sectorSize, &nRead, nullptr) || nRead != sectorSize)
        return false;

    return (std::memcmp(expectedBuf.data(), readBuf.data(), sectorSize) == 0);
}

// ============================================================================
// Lock and dismount all volumes on a physical disk
// ============================================================================
static void lockAndDismountVolumes(DiskId diskId, std::vector<HANDLE>& lockedHandles)
{
    // Find volume letters for this disk by checking each drive letter
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
    {
        if (!(drives & (1 << i))) continue;
        wchar_t letter = static_cast<wchar_t>(L'A' + i);

        // Check if this volume belongs to our disk
        wchar_t volPath[] = { L'\\', L'\\', L'.', L'\\', letter, L':', L'\0' };
        HANDLE hVol = CreateFileW(volPath, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (hVol == INVALID_HANDLE_VALUE)
            continue;

        VOLUME_DISK_EXTENTS extents{};
        DWORD ret = 0;
        if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                            nullptr, 0, &extents, sizeof(extents), &ret, nullptr))
        {
            bool onOurDisk = false;
            for (DWORD e = 0; e < extents.NumberOfDiskExtents; ++e)
            {
                if (extents.Extents[e].DiskNumber == static_cast<DWORD>(diskId))
                {
                    onOurDisk = true;
                    break;
                }
            }

            if (onOurDisk)
            {
                DWORD dummy = 0;
                DeviceIoControl(hVol, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &dummy, nullptr);
                DeviceIoControl(hVol, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &dummy, nullptr);
                lockedHandles.push_back(hVol);
                continue; // keep handle open (locked)
            }
        }
        CloseHandle(hVol);
    }
}

static void unlockVolumes(std::vector<HANDLE>& lockedHandles)
{
    for (HANDLE h : lockedHandles)
    {
        DWORD dummy = 0;
        DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &dummy, nullptr);
        CloseHandle(h);
    }
    lockedHandles.clear();
}

// ============================================================================
// Counterfeit check — write-all-then-read-all algorithm
// ============================================================================
Result<CounterfeitResult> SdCardAnalyzer::checkCounterfeit(
    DiskId diskId, SdAnalysisProgress progress)
{
    auto report = [&](const std::string& s, int p) { if (progress) progress(s, p); };
    CounterfeitResult result;

    report("Opening disk for counterfeit check...", 0);

    // Get disk size
    auto diskInfoResult = DiskEnumerator::getDiskInfo(diskId);
    if (diskInfoResult.isError()) return diskInfoResult.error();
    const auto& di = diskInfoResult.value();

    result.reportedCapacityBytes = di.sizeBytes;
    if (result.reportedCapacityBytes == 0)
    {
        result.verdict = CounterfeitVerdict::TestFailed;
        result.summaryMessage = "Disk reports zero capacity — no media?";
        return result;
    }

    // Get identity for manufacturer check
    auto idResult = queryIdentity(diskId);
    if (idResult.isOk())
    {
        const auto& id = idResult.value();
        result.manufacturerName = manufacturerName(id.manufacturerId);
        result.unknownManufacturer = (id.manufacturerId == 0 || !id.cidValid
                                     || std::string(result.manufacturerName) == "Unknown");

        // Only flag vendor string if it's truly suspicious (empty or literally "Generic").
        // USB card readers legitimately report "USB" or "Mass Storage" as the bus interface,
        // not the card manufacturer — that's normal and not suspicious.
        auto vendorLow = id.vendorId;
        std::transform(vendorLow.begin(), vendorLow.end(), vendorLow.begin(), ::towlower);
        result.suspiciousVendorString =
            (vendorLow == L"generic" || vendorLow == L"storage device" || vendorLow == L"sd");
    }

    // Check write protection before locking volumes
    if (isWriteProtected(diskId))
    {
        result.verdict = CounterfeitVerdict::TestFailed;
        result.summaryMessage = "Card is write-protected — cannot probe capacity";
        return result;
    }

    report("Locking volumes on disk...", 3);

    // Lock and dismount all volumes on this disk to prevent filesystem
    // interference (journal writes, metadata updates, etc.) during probing
    std::vector<HANDLE> lockedVolumes;
    lockAndDismountVolumes(diskId, lockedVolumes);

    report("Opening disk for write probing...", 5);

    // Open for read-write with extended DASD I/O (allows access past last partition)
    std::wstring devPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(diskId);
    HANDLE hDisk = CreateFileW(devPath.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
                               nullptr);

    if (hDisk == INVALID_HANDLE_VALUE)
    {
        unlockVolumes(lockedVolumes);
        result.verdict = CounterfeitVerdict::TestFailed;
        result.summaryMessage = "Cannot open disk for writing — check admin privileges";
        return result;
    }

    // Enable extended DASD I/O — allows read/write past the last partition boundary,
    // which is critical for probing near the end of the disk
    DWORD dummy = 0;
    DeviceIoControl(hDisk, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &dummy, nullptr);

    uint32_t sectorSize = di.sectorSize > 0 ? di.sectorSize : 512;
    uint64_t totalSectors = result.reportedCapacityBytes / sectorSize;

    // Build probe offsets — geometric distribution across the disk.
    // Fake cards typically wrap at their real capacity, so probes past the
    // real capacity will silently alias to lower addresses.
    // We use write-all-then-read-all: if any write clobbered a previous one
    // (due to address aliasing), we'll detect it.
    std::vector<uint64_t> probeOffsets;

    // Near-end probes (most likely to fail on fake cards)
    static const double nearEndFracs[] = { 0.99, 0.98, 0.97, 0.95, 0.93, 0.90, 0.85, 0.80 };
    for (auto frac : nearEndFracs)
    {
        uint64_t off = static_cast<uint64_t>(totalSectors * frac) * sectorSize;
        off = (off / sectorSize) * sectorSize; // align
        if (off > 0 && off + sectorSize <= result.reportedCapacityBytes)
            probeOffsets.push_back(off);
    }

    // Mid-range probes
    static const double midFracs[] = { 0.75, 0.50, 0.625, 0.875, 0.25, 0.125 };
    for (auto frac : midFracs)
    {
        uint64_t off = static_cast<uint64_t>(totalSectors * frac) * sectorSize;
        off = (off / sectorSize) * sectorSize;
        if (off > 0 && off + sectorSize <= result.reportedCapacityBytes)
            probeOffsets.push_back(off);
    }

    // Remove duplicates (can happen on very small disks)
    std::sort(probeOffsets.begin(), probeOffsets.end());
    probeOffsets.erase(std::unique(probeOffsets.begin(), probeOffsets.end()), probeOffsets.end());

    result.probeCount = static_cast<int>(probeOffsets.size());
    result.verifiedCapacityBytes = result.reportedCapacityBytes;

    uint64_t diskSerial = static_cast<uint64_t>(diskId) ^ 0xABCD1234EF567890ULL;

    // ---- Phase 1: Save original data and write all signatures ----
    report("Writing probe signatures...", 8);

    std::vector<std::vector<uint8_t>> originalData(probeOffsets.size());
    std::vector<bool> writeOk(probeOffsets.size(), false);

    for (int i = 0; i < static_cast<int>(probeOffsets.size()); ++i)
    {
        int pct = 8 + static_cast<int>((static_cast<double>(i) / probeOffsets.size()) * 30.0);
        report("Writing signature at " + std::to_string(probeOffsets[i] / (1024 * 1024)) + " MB...", pct);

        // Save original sector
        originalData[i].resize(sectorSize, 0);
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(probeOffsets[i]);
        SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN);
        DWORD nRead = 0;
        ReadFile(hDisk, originalData[i].data(), sectorSize, &nRead, nullptr);

        // Write our unique signature
        std::vector<uint8_t> sigBuf(sectorSize);
        makeSignature(sigBuf.data(), sectorSize, probeOffsets[i], diskSerial);

        li.QuadPart = static_cast<LONGLONG>(probeOffsets[i]);
        SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN);
        DWORD nWritten = 0;
        writeOk[i] = (WriteFile(hDisk, sigBuf.data(), sectorSize, &nWritten, nullptr)
                       && nWritten == sectorSize);
    }

    // Flush all writes to ensure they hit the physical media
    FlushFileBuffers(hDisk);

    // ---- Phase 2: Close and re-open to defeat any driver-level caching ----
    // Some USB-to-SD readers cache writes internally; re-opening the handle
    // forces a fresh read from the actual NAND.
    CloseHandle(hDisk);
    Sleep(200); // Brief pause for USB controller to settle

    hDisk = CreateFileW(devPath.c_str(),
                        GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING,
                        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
                        nullptr);

    if (hDisk == INVALID_HANDLE_VALUE)
    {
        // Can't re-open — restore what we can and bail
        unlockVolumes(lockedVolumes);
        result.verdict = CounterfeitVerdict::TestFailed;
        result.summaryMessage = "Lost access to disk during probe — test inconclusive";
        return result;
    }

    DeviceIoControl(hDisk, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &dummy, nullptr);

    // ---- Phase 3: Read back ALL signatures and verify ----
    report("Verifying probe signatures...", 42);

    for (int i = 0; i < static_cast<int>(probeOffsets.size()); ++i)
    {
        int pct = 42 + static_cast<int>((static_cast<double>(i) / probeOffsets.size()) * 45.0);
        report("Verifying at " + std::to_string(probeOffsets[i] / (1024 * 1024)) + " MB...", pct);

        if (!writeOk[i])
        {
            // Write itself failed — count as failure
            result.failCount++;
            if (result.firstBadOffsetBytes == 0)
                result.firstBadOffsetBytes = probeOffsets[i];
            if (probeOffsets[i] < result.verifiedCapacityBytes)
                result.verifiedCapacityBytes = probeOffsets[i];
            continue;
        }

        bool ok = probeOffset(hDisk, probeOffsets[i], sectorSize, diskSerial);
        if (!ok)
        {
            result.failCount++;
            if (result.firstBadOffsetBytes == 0)
                result.firstBadOffsetBytes = probeOffsets[i];
            if (probeOffsets[i] < result.verifiedCapacityBytes)
                result.verifiedCapacityBytes = probeOffsets[i];
        }
    }

    // ---- Phase 4: Restore original data ----
    report("Restoring original data...", 90);

    for (int i = 0; i < static_cast<int>(probeOffsets.size()); ++i)
    {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(probeOffsets[i]);
        SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN);
        DWORD nWritten = 0;
        WriteFile(hDisk, originalData[i].data(), sectorSize, &nWritten, nullptr);
    }
    FlushFileBuffers(hDisk);
    CloseHandle(hDisk);

    // Unlock volumes
    unlockVolumes(lockedVolumes);

    result.failPercent = result.probeCount > 0
        ? (static_cast<double>(result.failCount) / result.probeCount) * 100.0 : 0.0;

    report("Analyzing results...", 95);

    // ---- Verdict ----
    if (result.failCount == 0)
    {
        // All probes passed — card is genuine regardless of vendor string.
        // USB card readers legitimately report generic vendor info; that alone
        // is not evidence of counterfeiting.
        result.verdict = CounterfeitVerdict::Genuine;
        result.summaryMessage =
            "All " + std::to_string(result.probeCount) + " capacity probes passed. "
            "No evidence of capacity spoofing detected.";

        if (result.unknownManufacturer)
            result.summaryMessage += " (Note: manufacturer ID could not be read, which is "
                                     "normal for cards accessed via USB readers.)";
    }
    else if (result.failPercent >= 25.0)
    {
        result.verdict = CounterfeitVerdict::LikelySpoofed;
        double realGB = static_cast<double>(result.verifiedCapacityBytes) / (1024.0 * 1024.0 * 1024.0);
        double claimedGB = static_cast<double>(result.reportedCapacityBytes) / (1024.0 * 1024.0 * 1024.0);
        char buf[512];
        snprintf(buf, sizeof(buf),
            "COUNTERFEIT DETECTED: Card claims %.1f GB but real capacity appears to be ~%.1f GB. "
            "%d of %d probes failed (%.0f%%). "
            "First failure at offset %.1f GB.",
            claimedGB, realGB,
            result.failCount, result.probeCount, result.failPercent,
            static_cast<double>(result.firstBadOffsetBytes) / (1024.0 * 1024.0 * 1024.0));
        result.summaryMessage = buf;
    }
    else
    {
        // Low failure rate — could be marginal NAND or I/O glitch, not necessarily fake
        result.verdict = CounterfeitVerdict::Suspicious;
        result.summaryMessage =
            std::to_string(result.failCount) + " of " + std::to_string(result.probeCount) +
            " probes failed. This may indicate marginal NAND cells or a partially "
            "counterfeit card. Recommend running the test again — if failures are "
            "consistent at the same offsets, the card is likely fake.";
    }

    report("Done.", 100);
    return result;
}

// ============================================================================
// Speed benchmark
// ============================================================================
Result<SdSpeedResult> SdCardAnalyzer::benchmarkSpeed(
    DiskId diskId, uint64_t testSizeBytes, SdAnalysisProgress progress)
{
    auto report = [&](const std::string& s, int p) { if (progress) progress(s, p); };
    SdSpeedResult result;

    auto diskInfoResult = DiskEnumerator::getDiskInfo(diskId);
    if (diskInfoResult.isError()) return diskInfoResult.error();
    uint32_t sectorSize = diskInfoResult.value().sectorSize > 0
                          ? diskInfoResult.value().sectorSize : 512;

    if (isWriteProtected(diskId))
        result.writeProtected = true;

    std::wstring devPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(diskId);

    // ---- Sequential Read ----
    report("Sequential read benchmark...", 5);
    {
        HANDLE hDisk = CreateFileW(devPath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (hDisk != INVALID_HANDLE_VALUE)
        {
            constexpr uint32_t kChunkSize = 1024 * 1024; // 1 MB chunks
            std::vector<uint8_t> buf(kChunkSize);
            uint64_t totalRead = 0;

            LARGE_INTEGER li{}; SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN);
            auto t0 = std::chrono::steady_clock::now();
            while (totalRead < testSizeBytes)
            {
                DWORD n = 0;
                if (!ReadFile(hDisk, buf.data(), kChunkSize, &n, nullptr) || n == 0) break;
                totalRead += n;

                int pct = 5 + static_cast<int>((static_cast<double>(totalRead) / testSizeBytes) * 25.0);
                report("Sequential read: " + std::to_string(totalRead / (1024*1024)) + " MB...", pct);
            }
            auto t1 = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(t1 - t0).count();
            if (secs > 0 && totalRead > 0)
                result.seqReadMBps = (static_cast<double>(totalRead) / (1024.0 * 1024.0)) / secs;
            CloseHandle(hDisk);
        }
    }

    if (result.writeProtected)
    {
        result.seqWriteMBps = 0;
        result.notes = "Write-protected — write benchmarks skipped";
        report("Done (write-protected).", 100);
        return result;
    }

    // ---- Sequential Write ----
    report("Sequential write benchmark...", 32);
    {
        HANDLE hDisk = CreateFileW(devPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (hDisk != INVALID_HANDLE_VALUE)
        {
            constexpr uint32_t kChunkSize = 1024 * 1024;
            std::vector<uint8_t> buf(kChunkSize, 0xA5); // pattern
            uint64_t totalWritten = 0;

            LARGE_INTEGER li{}; SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN);
            auto t0 = std::chrono::steady_clock::now();
            while (totalWritten < testSizeBytes)
            {
                DWORD n = 0;
                DWORD toWrite = static_cast<DWORD>(
                    std::min<uint64_t>(kChunkSize, testSizeBytes - totalWritten));
                if (!WriteFile(hDisk, buf.data(), toWrite, &n, nullptr) || n == 0) break;
                totalWritten += n;

                int pct = 32 + static_cast<int>((static_cast<double>(totalWritten) / testSizeBytes) * 28.0);
                report("Sequential write: " + std::to_string(totalWritten / (1024*1024)) + " MB...", pct);
            }
            auto t1 = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(t1 - t0).count();
            if (secs > 0 && totalWritten > 0)
                result.seqWriteMBps = (static_cast<double>(totalWritten) / (1024.0 * 1024.0)) / secs;
            CloseHandle(hDisk);
        }
    }

    // ---- Random 4K Read IOPS ----
    report("Random 4K read IOPS...", 62);
    {
        HANDLE hDisk = CreateFileW(devPath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (hDisk != INVALID_HANDLE_VALUE)
        {
            constexpr uint32_t kBlockSize = 4096;
            constexpr int kIterations = 256;
            std::vector<uint8_t> buf(kBlockSize);
            uint64_t diskSectors = diskInfoResult.value().sizeBytes / sectorSize;

            std::mt19937_64 rng(42);
            std::uniform_int_distribution<uint64_t> dist(0, diskSectors - kBlockSize / sectorSize - 1);

            auto t0 = std::chrono::steady_clock::now();
            int completed = 0;
            for (int i = 0; i < kIterations; ++i)
            {
                uint64_t off = (dist(rng) * sectorSize) & ~(uint64_t)(kBlockSize - 1);
                LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(off);
                SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN);
                DWORD n = 0;
                if (ReadFile(hDisk, buf.data(), kBlockSize, &n, nullptr) && n == kBlockSize)
                    ++completed;
            }
            auto t1 = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(t1 - t0).count();
            if (secs > 0)
                result.randRead4kIOPS = completed / secs;
            CloseHandle(hDisk);
        }
    }

    // ---- Random 4K Write IOPS ----
    report("Random 4K write IOPS...", 82);
    {
        HANDLE hDisk = CreateFileW(devPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (hDisk != INVALID_HANDLE_VALUE)
        {
            constexpr uint32_t kBlockSize = 4096;
            constexpr int kIterations = 128;
            std::vector<uint8_t> buf(kBlockSize, 0x5A);
            uint64_t diskSectors = diskInfoResult.value().sizeBytes / sectorSize;
            uint64_t safeEnd = diskSectors / 2; // Only write to first half to preserve data

            std::mt19937_64 rng(99);
            std::uniform_int_distribution<uint64_t> dist(1, safeEnd - kBlockSize / sectorSize - 1);

            auto t0 = std::chrono::steady_clock::now();
            int completed = 0;
            for (int i = 0; i < kIterations; ++i)
            {
                uint64_t off = (dist(rng) * sectorSize) & ~(uint64_t)(kBlockSize - 1);
                LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(off);
                SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN);
                DWORD n = 0;
                if (WriteFile(hDisk, buf.data(), kBlockSize, &n, nullptr) && n == kBlockSize)
                    ++completed;
            }
            auto t1 = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(t1 - t0).count();
            if (secs > 0)
                result.randWrite4kIOPS = completed / secs;
            CloseHandle(hDisk);
        }
    }

    report("Done.", 100);
    return result;
}

// ============================================================================
// Surface scan
// ============================================================================
Result<SdHealthResult> SdCardAnalyzer::surfaceScan(
    DiskId diskId, std::atomic<bool>* cancelFlag, SdSectorProgress progress)
{
    SdHealthResult result;

    auto diskInfoResult = DiskEnumerator::getDiskInfo(diskId);
    if (diskInfoResult.isError()) return diskInfoResult.error();

    uint32_t sectorSize = diskInfoResult.value().sectorSize > 0
                          ? diskInfoResult.value().sectorSize : 512;
    result.totalSectors = diskInfoResult.value().sizeBytes / sectorSize;

    std::wstring devPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(diskId);
    HANDLE hDisk = CreateFileW(devPath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_NO_BUFFERING, nullptr);
    if (hDisk == INVALID_HANDLE_VALUE)
        return ErrorInfo::fromWin32(ErrorCode::DiskAccessDenied, GetLastError(),
                                    "Cannot open disk for surface scan");

    constexpr uint32_t kSectorsPerRead = 64; // 32 KB per read
    std::vector<uint8_t> buf(kSectorsPerRead * 512);

    LARGE_INTEGER li{}; SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN);

    for (uint64_t sec = 0; sec < result.totalSectors; sec += kSectorsPerRead)
    {
        if (cancelFlag && cancelFlag->load()) break;

        uint32_t toRead = static_cast<uint32_t>(
            std::min<uint64_t>(kSectorsPerRead, result.totalSectors - sec));
        uint32_t readBytes = toRead * sectorSize;

        auto t0 = std::chrono::steady_clock::now();
        DWORD n = 0;
        bool ok = ReadFile(hDisk, buf.data(), readBytes, &n, nullptr) && n == readBytes;
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (!ok)
            result.badSectors += toRead;
        else if (ms > 500.0)
            result.slowSectors += toRead;

        result.sectorsScanned += toRead;

        if (progress)
        {
            int pct = static_cast<int>((result.sectorsScanned * 100) / result.totalSectors);
            progress(sec, result.totalSectors, result.badSectors, pct);
        }
    }

    CloseHandle(hDisk);
    result.complete = (cancelFlag == nullptr || !cancelFlag->load());

    char summary[256];
    snprintf(summary, sizeof(summary),
        "Scanned %llu sectors. Bad: %llu. Slow (>500ms): %llu.",
        (unsigned long long)result.sectorsScanned,
        (unsigned long long)result.badSectors,
        (unsigned long long)result.slowSectors);
    result.summary = summary;

    return result;
}

// ============================================================================
// Write-protection check
// ============================================================================
bool SdCardAnalyzer::isWriteProtected(DiskId diskId)
{
    std::wstring devPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(diskId);
    HANDLE h = CreateFileW(devPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return true; // Can't open for write — treat as write-protected

    // Try a test write of zero bytes via DeviceIoControl
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &returned, nullptr);
    DWORD err = GetLastError();
    CloseHandle(h);

    // IOCTL_DISK_IS_WRITABLE returns FALSE with ERROR_WRITE_PROTECT if write-protected
    return (!ok && err == ERROR_WRITE_PROTECT);
}

} // namespace spw
