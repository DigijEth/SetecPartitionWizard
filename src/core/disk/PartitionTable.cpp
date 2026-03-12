// PartitionTable.cpp — Complete implementation of MBR, GPT, and APM partition table parsing/writing.
//
// DISCLAIMER: This code is for authorized disk utility software only.
// Never use partition modification code on disks without explicit authorization.

#include "PartitionTable.h"
#include "../common/Logging.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <numeric>
#include <random>

namespace spw
{

// ============================================================================
// CRC32 — Standard ISO-HDLC / ITU-T V.42 polynomial
// Used by GPT for header and partition entry array validation.
// Polynomial: 0xEDB88320 (reflected form of 0x04C11DB7)
// ============================================================================

// Build the CRC32 lookup table at startup using a helper function
static std::array<uint32_t, 256> buildCrc32Table()
{
    std::array<uint32_t, 256> table = {};
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
        }
        table[i] = crc;
    }
    return table;
}

static const std::array<uint32_t, 256> s_crc32Table = buildCrc32Table();

uint32_t crc32(const uint8_t* data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++)
    {
        crc = s_crc32Table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t crc32(const std::vector<uint8_t>& data)
{
    return crc32(data.data(), data.size());
}

// ============================================================================
// Guid helpers — the on-disk GPT GUID is stored in mixed-endian format:
//   bytes 0-3:  little-endian uint32
//   bytes 4-5:  little-endian uint16
//   bytes 6-7:  little-endian uint16
//   bytes 8-15: raw bytes (big-endian order)
// ============================================================================

static Guid guidFromBytes(const uint8_t raw[16])
{
    Guid g;
    std::memcpy(g.data, raw, 16);
    return g;
}

static void guidToBytes(const Guid& g, uint8_t out[16])
{
    std::memcpy(out, g.data, 16);
}

// Read a little-endian uint16 from a byte buffer
static uint16_t readLE16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

// Read a little-endian uint32 from a byte buffer
static uint32_t readLE32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

// Read a little-endian uint64 from a byte buffer
static uint64_t readLE64(const uint8_t* p)
{
    return static_cast<uint64_t>(readLE32(p))
         | (static_cast<uint64_t>(readLE32(p + 4)) << 32);
}

// Write a little-endian uint16
static void writeLE16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

// Write a little-endian uint32
static void writeLE32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

// Write a little-endian uint64
static void writeLE64(uint8_t* p, uint64_t v)
{
    writeLE32(p, static_cast<uint32_t>(v));
    writeLE32(p + 4, static_cast<uint32_t>(v >> 32));
}

// Read a big-endian uint16 (for APM)
static uint16_t readBE16(const uint8_t* p)
{
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

// Read a big-endian uint32 (for APM)
static uint32_t readBE32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)
         | static_cast<uint32_t>(p[3]);
}

// Extract CHS address from the 3-byte packed MBR format.
// Byte layout: [head8] [sec6:cyl_hi2] [cyl_lo8]
// cylinder = cyl_lo8 | (cyl_hi2 << 8)  -> 10-bit value (0-1023)
// head     = head8                      -> 8-bit value (0-254)
// sector   = sec6                       -> 6-bit value (1-63)
static CHSAddress decodeCHS(const uint8_t packed[3])
{
    CHSAddress chs;
    chs.head = packed[0];
    chs.sector = packed[1] & 0x3F;
    chs.cylinder = static_cast<uint32_t>(packed[2]) | ((static_cast<uint32_t>(packed[1] & 0xC0)) << 2);
    return chs;
}

// Encode CHS address into the 3-byte packed MBR format
static void encodeCHS(const CHSAddress& chs, uint8_t out[3])
{
    out[0] = chs.head;
    out[1] = static_cast<uint8_t>((chs.sector & 0x3F) | ((chs.cylinder >> 2) & 0xC0));
    out[2] = static_cast<uint8_t>(chs.cylinder & 0xFF);
}

// For partitions beyond CHS range (> ~8 GiB), use the "overflow" value FE FF FF
static void encodeCHSOverflow(uint8_t out[3])
{
    out[0] = 0xFE;
    out[1] = 0xFF;
    out[2] = 0xFF;
}

// UTF-16LE to UTF-8 conversion (simple BMP-only, sufficient for GPT names)
static std::string utf16leToUtf8(const uint16_t* data, size_t maxChars)
{
    std::string result;
    result.reserve(maxChars);
    for (size_t i = 0; i < maxChars; i++)
    {
        uint16_t ch = data[i];
        if (ch == 0)
            break;
        if (ch < 0x80)
        {
            result.push_back(static_cast<char>(ch));
        }
        else if (ch < 0x800)
        {
            result.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
        else
        {
            result.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            result.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return result;
}

// UTF-8 to UTF-16LE conversion (BMP only)
static void utf8ToUtf16le(const std::string& src, uint16_t* out, size_t maxChars)
{
    std::memset(out, 0, maxChars * sizeof(uint16_t));
    size_t outIdx = 0;
    size_t i = 0;
    while (i < src.size() && outIdx < maxChars - 1)
    {
        uint8_t c = static_cast<uint8_t>(src[i]);
        uint16_t ch = 0;
        if (c < 0x80)
        {
            ch = c;
            i += 1;
        }
        else if ((c & 0xE0) == 0xC0 && i + 1 < src.size())
        {
            ch = static_cast<uint16_t>(((c & 0x1F) << 6) | (src[i + 1] & 0x3F));
            i += 2;
        }
        else if ((c & 0xF0) == 0xE0 && i + 2 < src.size())
        {
            ch = static_cast<uint16_t>(((c & 0x0F) << 12) | ((src[i + 1] & 0x3F) << 6) | (src[i + 2] & 0x3F));
            i += 3;
        }
        else
        {
            // Skip non-BMP or malformed
            i += 1;
            continue;
        }
        out[outIdx++] = ch;
    }
}

// ============================================================================
// MbrTypes namespace — type byte to name mapping
// ============================================================================

const char* MbrTypes::typeName(uint8_t type)
{
    switch (type)
    {
    case Empty:           return "Empty";
    case FAT12:           return "FAT12";
    case FAT16_Small:     return "FAT16 (<32M)";
    case Extended:        return "Extended (CHS)";
    case FAT16_Large:     return "FAT16 (>=32M)";
    case NTFS_HPFS:       return "NTFS/HPFS/exFAT";
    case FAT32_CHS:       return "FAT32 (CHS)";
    case FAT32_LBA:       return "FAT32 (LBA)";
    case FAT16_LBA:       return "FAT16 (LBA)";
    case Extended_LBA:    return "Extended (LBA)";
    case HiddenFAT32:     return "Hidden FAT32";
    case HiddenFAT32_LBA: return "Hidden FAT32 LBA";
    case DynDisk:         return "Dynamic Disk";
    case LinuxSwap:       return "Linux Swap";
    case LinuxNative:     return "Linux";
    case LinuxExtended:   return "Linux Extended";
    case LinuxLVM:        return "Linux LVM";
    case FreeBSD:         return "FreeBSD";
    case OpenBSD:         return "OpenBSD";
    case NetBSD:          return "NetBSD";
    case HFS_APM:         return "HFS/HFS+";
    case GPT_Protective:  return "GPT Protective MBR";
    case EFI_System:      return "EFI System";
    case LinuxRaid:       return "Linux RAID";
    default:              return "Unknown";
    }
}

bool MbrTypes::isExtendedType(uint8_t type)
{
    return type == Extended || type == Extended_LBA || type == LinuxExtended;
}

// ============================================================================
// GptTypes namespace — well-known partition type GUIDs
// ============================================================================

// Helper: build a Guid from the standard string form "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"
// GPT stores GUIDs in mixed-endian: first 3 components are LE, last 2 are BE.
static Guid makeGptGuid(uint32_t d1, uint16_t d2, uint16_t d3, uint8_t d4[8])
{
    Guid g;
    // First 4 bytes: d1 in little-endian
    g.data[0] = static_cast<uint8_t>(d1);
    g.data[1] = static_cast<uint8_t>(d1 >> 8);
    g.data[2] = static_cast<uint8_t>(d1 >> 16);
    g.data[3] = static_cast<uint8_t>(d1 >> 24);
    // Bytes 4-5: d2 in little-endian
    g.data[4] = static_cast<uint8_t>(d2);
    g.data[5] = static_cast<uint8_t>(d2 >> 8);
    // Bytes 6-7: d3 in little-endian
    g.data[6] = static_cast<uint8_t>(d3);
    g.data[7] = static_cast<uint8_t>(d3 >> 8);
    // Bytes 8-15: d4 in big-endian (raw order)
    std::memcpy(&g.data[8], d4, 8);
    return g;
}

Guid GptTypes::microsoftBasicData()
{
    uint8_t d4[] = { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };
    return makeGptGuid(0xEBD0A0A2, 0xB9E5, 0x4433, d4);
}

Guid GptTypes::microsoftReserved()
{
    uint8_t d4[] = { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE };
    return makeGptGuid(0xE3C9E316, 0x0B5C, 0x4DB8, d4);
}

Guid GptTypes::efiSystem()
{
    uint8_t d4[] = { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
    return makeGptGuid(0xC12A7328, 0xF81F, 0x11D2, d4);
}

Guid GptTypes::microsoftLdmMetadata()
{
    uint8_t d4[] = { 0x85, 0xD2, 0xE1, 0xE9, 0x04, 0x34, 0xCF, 0xB3 };
    return makeGptGuid(0x5808C8AA, 0x7E8F, 0x42E0, d4);
}

Guid GptTypes::microsoftLdmData()
{
    uint8_t d4[] = { 0xBC, 0x68, 0x33, 0x11, 0x71, 0x4A, 0x69, 0xAD };
    return makeGptGuid(0xAF9B60A0, 0x1431, 0x4F62, d4);
}

Guid GptTypes::microsoftRecovery()
{
    uint8_t d4[] = { 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC };
    return makeGptGuid(0xDE94BBA4, 0x06D1, 0x4D40, d4);
}

Guid GptTypes::linuxFilesystem()
{
    uint8_t d4[] = { 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };
    return makeGptGuid(0x0FC63DAF, 0x8483, 0x4772, d4);
}

Guid GptTypes::linuxSwap()
{
    uint8_t d4[] = { 0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F };
    return makeGptGuid(0x0657FD6D, 0xA4AB, 0x43C4, d4);
}

Guid GptTypes::linuxHome()
{
    uint8_t d4[] = { 0xB8, 0x44, 0x0E, 0x14, 0xE2, 0xAE, 0xF9, 0x15 };
    return makeGptGuid(0x933AC7E1, 0x2EB4, 0x4F13, d4);
}

Guid GptTypes::linuxLvm()
{
    uint8_t d4[] = { 0xA2, 0x3C, 0x23, 0x8F, 0x2A, 0x3D, 0xF9, 0x28 };
    return makeGptGuid(0xE6D6D379, 0xF507, 0x44C2, d4);
}

Guid GptTypes::linuxRaid()
{
    uint8_t d4[] = { 0xA0, 0x06, 0x74, 0x3F, 0x0F, 0x84, 0x91, 0x1E };
    return makeGptGuid(0xA19D880F, 0x05FC, 0x4D3B, d4);
}

Guid GptTypes::appleHfsPlus()
{
    uint8_t d4[] = { 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC };
    return makeGptGuid(0x48465300, 0x0000, 0x11AA, d4);
}

Guid GptTypes::appleApfs()
{
    uint8_t d4[] = { 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC };
    return makeGptGuid(0x7C3457EF, 0x0000, 0x11AA, d4);
}

Guid GptTypes::appleBoot()
{
    uint8_t d4[] = { 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC };
    return makeGptGuid(0x426F6F74, 0x0000, 0x11AA, d4);
}

Guid GptTypes::freebsdUfs()
{
    uint8_t d4[] = { 0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B };
    return makeGptGuid(0x516E7CB6, 0x6ECF, 0x11D6, d4);
}

Guid GptTypes::freebsdSwap()
{
    uint8_t d4[] = { 0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B };
    return makeGptGuid(0x516E7CB5, 0x6ECF, 0x11D6, d4);
}

Guid GptTypes::freebsdZfs()
{
    uint8_t d4[] = { 0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B };
    return makeGptGuid(0x516E7CBA, 0x6ECF, 0x11D6, d4);
}

std::string GptTypes::typeName(const Guid& guid)
{
    if (guid == microsoftBasicData())    return "Microsoft Basic Data";
    if (guid == microsoftReserved())     return "Microsoft Reserved";
    if (guid == efiSystem())             return "EFI System";
    if (guid == microsoftLdmMetadata())  return "LDM Metadata";
    if (guid == microsoftLdmData())      return "LDM Data";
    if (guid == microsoftRecovery())     return "Windows Recovery";
    if (guid == linuxFilesystem())       return "Linux Filesystem";
    if (guid == linuxSwap())             return "Linux Swap";
    if (guid == linuxHome())             return "Linux Home";
    if (guid == linuxLvm())              return "Linux LVM";
    if (guid == linuxRaid())             return "Linux RAID";
    if (guid == appleHfsPlus())          return "Apple HFS+";
    if (guid == appleApfs())             return "Apple APFS";
    if (guid == appleBoot())             return "Apple Boot";
    if (guid == freebsdUfs())            return "FreeBSD UFS";
    if (guid == freebsdSwap())           return "FreeBSD Swap";
    if (guid == freebsdZfs())            return "FreeBSD ZFS";
    if (guid.isZero())                   return "Unused";
    return "Unknown (" + guid.toString() + ")";
}

// ============================================================================
// PartitionTable — static factory methods
// ============================================================================

Result<std::unique_ptr<PartitionTable>> PartitionTable::parse(
    const DiskReadCallback& readFunc,
    uint64_t diskSizeBytes,
    uint32_t sectorSize)
{
    // Read the first sector (MBR / DDM)
    auto sector0Result = readFunc(0, sectorSize);
    if (sector0Result.isError())
        return sector0Result.error();

    const auto& sector0 = sector0Result.value();
    if (sector0.size() < 512)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt, "Sector 0 too small");

    // Check for APM: Driver Descriptor Map signature 0x4552 ("ER") at offset 0 (big-endian)
    uint16_t ddmSig = readBE16(sector0.data());
    if (ddmSig == APM_DDM_SIGNATURE)
    {
        auto apm = std::make_unique<ApmPartitionTable>();
        apm->m_diskSizeBytes = diskSizeBytes;
        apm->m_sectorSize = sectorSize;
        auto parseResult = apm->parse(readFunc);
        if (parseResult.isError())
            return parseResult.error();
        return std::unique_ptr<PartitionTable>(std::move(apm));
    }

    // Check MBR signature at bytes 510-511
    uint16_t mbrSig = readLE16(sector0.data() + 510);
    if (mbrSig != MBR_SIGNATURE)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt,
            "No valid partition table signature (expected 0xAA55 at offset 510)");

    // Parse the MBR to check if it's a GPT protective MBR
    auto mbr = std::make_unique<MbrPartitionTable>();
    mbr->m_diskSizeBytes = diskSizeBytes;
    mbr->m_sectorSize = sectorSize;
    auto mbrParseResult = mbr->parse(readFunc);
    if (mbrParseResult.isError())
        return mbrParseResult.error();

    // If the MBR contains a GPT protective entry (type 0xEE), parse as GPT
    if (mbr->hasGptProtective())
    {
        auto gpt = std::make_unique<GptPartitionTable>();
        gpt->m_diskSizeBytes = diskSizeBytes;
        gpt->m_sectorSize = sectorSize;
        auto gptParseResult = gpt->parse(readFunc);
        if (gptParseResult.isError())
        {
            // If GPT parsing fails, fall back to MBR interpretation
            log::warn("GPT header parsing failed, falling back to MBR");
            return std::unique_ptr<PartitionTable>(std::move(mbr));
        }
        return std::unique_ptr<PartitionTable>(std::move(gpt));
    }

    return std::unique_ptr<PartitionTable>(std::move(mbr));
}

std::unique_ptr<PartitionTable> PartitionTable::createNew(
    PartitionTableType type,
    uint64_t diskSizeBytes,
    uint32_t sectorSize,
    const Guid& diskGuid)
{
    switch (type)
    {
    case PartitionTableType::MBR:
    {
        auto mbr = std::make_unique<MbrPartitionTable>();
        mbr->m_diskSizeBytes = diskSizeBytes;
        mbr->m_sectorSize = sectorSize;
        return mbr;
    }
    case PartitionTableType::GPT:
    {
        auto gpt = std::make_unique<GptPartitionTable>();
        gpt->m_diskSizeBytes = diskSizeBytes;
        gpt->m_sectorSize = sectorSize;

        // Set disk GUID — generate one if not provided
        if (diskGuid.isZero())
            gpt->setDiskGuid(Guid::generate());
        else
            gpt->setDiskGuid(diskGuid);

        return gpt;
    }
    case PartitionTableType::APM:
    {
        auto apm = std::make_unique<ApmPartitionTable>();
        apm->m_diskSizeBytes = diskSizeBytes;
        apm->m_sectorSize = sectorSize;
        return apm;
    }
    default:
        return nullptr;
    }
}

// ============================================================================
// MbrPartitionTable implementation
// ============================================================================

MbrPartitionTable::MbrPartitionTable()
{
    m_bootCode.fill(0);
}

Result<void> MbrPartitionTable::parse(const DiskReadCallback& readFunc)
{
    auto sector0Result = readFunc(0, MBR_SIZE);
    if (sector0Result.isError())
        return sector0Result.error();

    const auto& raw = sector0Result.value();
    if (raw.size() < MBR_SIZE)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt, "MBR sector too small");

    // Validate signature
    uint16_t sig = readLE16(raw.data() + 510);
    if (sig != MBR_SIGNATURE)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt, "Invalid MBR signature");

    // Copy boot code (bytes 0-445)
    std::memcpy(m_bootCode.data(), raw.data(), 446);

    // Disk signature at bytes 440-443 (used by Windows for disk identification)
    m_diskSignature = readLE32(raw.data() + 440);
    m_reserved = readLE16(raw.data() + 444);

    m_entries.clear();

    // Parse four primary partition entries at offset 446
    for (int i = 0; i < MBR_MAX_PRIMARY_PARTITIONS; i++)
    {
        const uint8_t* entryPtr = raw.data() + MBR_PARTITION_ENTRY_OFFSET + (i * MBR_PARTITION_ENTRY_SIZE);

        uint8_t status = entryPtr[0];
        uint8_t type = entryPtr[4];
        uint32_t lbaStart = readLE32(entryPtr + 8);
        uint32_t sectorCount = readLE32(entryPtr + 12);

        // Skip empty entries
        if (type == MbrTypes::Empty && lbaStart == 0 && sectorCount == 0)
            continue;

        PartitionEntry entry;
        entry.index = i;
        entry.startLba = lbaStart;
        entry.sectorCount = sectorCount;
        entry.sectorSize = m_sectorSize;
        entry.mbrType = type;
        entry.isActive = (status == 0x80);
        entry.isExtended = MbrTypes::isExtendedType(type);
        entry.isLogical = false;
        entry.chsStart = decodeCHS(entryPtr + 1);
        entry.chsEnd = decodeCHS(entryPtr + 5);

        m_entries.push_back(entry);
    }

    // Walk extended partition chain if present
    for (const auto& entry : m_entries)
    {
        if (entry.isExtended)
        {
            auto walkResult = walkExtendedChain(readFunc, entry.startLba, entry.sectorCount);
            if (walkResult.isError())
            {
                // Log but don't fail — the primary table is still valid
                log::warn("Failed to walk extended partition chain");
            }
            break; // Only one extended partition per MBR
        }
    }

    return Result<void>::ok();
}

Result<void> MbrPartitionTable::walkExtendedChain(
    const DiskReadCallback& readFunc,
    SectorOffset extStart,
    SectorOffset extSize)
{
    // EBR chain: each Extended Boot Record is a 512-byte sector containing
    // a mini partition table with up to 2 entries:
    //   Entry 0: the logical partition (offset relative to THIS EBR)
    //   Entry 1: pointer to the NEXT EBR (offset relative to extended start)
    //
    // We limit chain depth to prevent infinite loops from corrupt tables.
    constexpr int MAX_LOGICAL_PARTITIONS = 256;

    SectorOffset currentEbrLba = extStart;
    int logicalCount = 0;

    while (currentEbrLba != 0 && logicalCount < MAX_LOGICAL_PARTITIONS)
    {
        // Bounds check: EBR must be within the extended partition
        if (currentEbrLba < extStart || currentEbrLba >= extStart + extSize)
        {
            log::warn("EBR chain pointer out of extended partition bounds");
            break;
        }

        auto ebrResult = readFunc(currentEbrLba * m_sectorSize, MBR_SIZE);
        if (ebrResult.isError())
            return ebrResult.error();

        const auto& ebrData = ebrResult.value();
        if (ebrData.size() < MBR_SIZE)
            break;

        // Validate EBR signature
        uint16_t ebrSig = readLE16(ebrData.data() + 510);
        if (ebrSig != MBR_SIGNATURE)
            break;

        // Entry 0: logical partition (offset relative to this EBR's LBA)
        const uint8_t* entry0 = ebrData.data() + MBR_PARTITION_ENTRY_OFFSET;
        uint8_t type0 = entry0[4];
        uint32_t lbaStart0 = readLE32(entry0 + 8);
        uint32_t sectorCount0 = readLE32(entry0 + 12);

        if (type0 != MbrTypes::Empty && sectorCount0 > 0)
        {
            PartitionEntry logical;
            logical.index = static_cast<int>(m_entries.size());
            logical.startLba = currentEbrLba + lbaStart0; // Absolute LBA
            logical.sectorCount = sectorCount0;
            logical.sectorSize = m_sectorSize;
            logical.mbrType = type0;
            logical.isActive = (entry0[0] == 0x80);
            logical.isExtended = false;
            logical.isLogical = true;
            logical.chsStart = decodeCHS(entry0 + 1);
            logical.chsEnd = decodeCHS(entry0 + 5);

            m_entries.push_back(logical);
            logicalCount++;
        }

        // Entry 1: pointer to next EBR (offset relative to extended partition start)
        const uint8_t* entry1 = ebrData.data() + MBR_PARTITION_ENTRY_OFFSET + MBR_PARTITION_ENTRY_SIZE;
        uint8_t type1 = entry1[4];
        uint32_t lbaStart1 = readLE32(entry1 + 8);

        if (MbrTypes::isExtendedType(type1) && lbaStart1 != 0)
        {
            currentEbrLba = extStart + lbaStart1;
        }
        else
        {
            break; // End of chain
        }
    }

    return Result<void>::ok();
}

std::vector<PartitionEntry> MbrPartitionTable::partitions() const
{
    return m_entries;
}

bool MbrPartitionTable::hasGptProtective() const
{
    for (const auto& entry : m_entries)
    {
        if (entry.mbrType == MbrTypes::GPT_Protective)
            return true;
    }
    return false;
}

int MbrPartitionTable::findExtendedIndex() const
{
    for (size_t i = 0; i < m_entries.size(); i++)
    {
        if (m_entries[i].isExtended && !m_entries[i].isLogical)
            return static_cast<int>(i);
    }
    return -1;
}

bool MbrPartitionTable::overlapsExisting(SectorOffset start, SectorCount count, int excludeIndex) const
{
    SectorOffset end = start + count;
    for (const auto& entry : m_entries)
    {
        if (entry.index == excludeIndex)
            continue;
        if (entry.sectorCount == 0)
            continue;

        SectorOffset entryEnd = entry.startLba + entry.sectorCount;
        // Overlap check: ranges overlap if neither is entirely before the other
        if (start < entryEnd && entry.startLba < end)
            return true;
    }
    return false;
}

Result<void> MbrPartitionTable::addPartition(const PartitionParams& params)
{
    if (params.sectorCount == 0)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Partition size cannot be zero");

    if (params.isLogical)
    {
        // Adding a logical partition inside the extended partition
        int extIdx = findExtendedIndex();
        if (extIdx < 0)
            return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                "No extended partition exists for logical partition creation");

        const auto& ext = m_entries[extIdx];
        SectorOffset extEnd = ext.startLba + ext.sectorCount;

        // Verify logical partition fits within extended
        if (params.startLba < ext.startLba || params.startLba + params.sectorCount > extEnd)
            return ErrorInfo::fromCode(ErrorCode::PartitionOverlap,
                "Logical partition does not fit within extended partition");
    }
    else
    {
        // Primary partition: count existing primary entries (non-logical, non-empty)
        int primaryCount = 0;
        for (const auto& entry : m_entries)
        {
            if (!entry.isLogical)
                primaryCount++;
        }

        if (primaryCount >= MBR_MAX_PRIMARY_PARTITIONS)
            return ErrorInfo::fromCode(ErrorCode::PartitionTableFull,
                "MBR supports at most 4 primary partitions");
    }

    // Check for overlaps
    if (overlapsExisting(params.startLba, params.sectorCount))
        return ErrorInfo::fromCode(ErrorCode::PartitionOverlap,
            "New partition overlaps an existing partition");

    // Verify bounds
    uint64_t diskSectors = m_diskSizeBytes / m_sectorSize;
    if (params.startLba + params.sectorCount > diskSectors)
        return ErrorInfo::fromCode(ErrorCode::PartitionTooLarge,
            "Partition extends beyond disk boundary");

    // MBR uses 32-bit LBA — maximum addressable sector is 2^32 - 1
    if (params.startLba > 0xFFFFFFFFULL || params.sectorCount > 0xFFFFFFFFULL)
        return ErrorInfo::fromCode(ErrorCode::PartitionTooLarge,
            "MBR cannot address sectors beyond 2 TiB");

    PartitionEntry newEntry;
    newEntry.index = static_cast<int>(m_entries.size());
    newEntry.startLba = params.startLba;
    newEntry.sectorCount = params.sectorCount;
    newEntry.sectorSize = m_sectorSize;
    newEntry.mbrType = params.mbrType;
    newEntry.isActive = params.isActive;
    newEntry.isExtended = MbrTypes::isExtendedType(params.mbrType);
    newEntry.isLogical = params.isLogical;

    // Generate CHS values. For modern disks, use the overflow sentinel.
    if (params.startLba > 16450559ULL) // Beyond CHS range (~8 GiB with 255/63 geometry)
    {
        newEntry.chsStart = { 1023, 254, 63 };
        newEntry.chsEnd = { 1023, 254, 63 };
    }
    else
    {
        // Use standard CHS geometry (255 heads, 63 sectors per track)
        CHSGeometry geo = { 255, 63 };
        auto chsStartResult = DiskGeometry::lbaToChs(params.startLba, geo);
        auto chsEndResult = DiskGeometry::lbaToChs(params.startLba + params.sectorCount - 1, geo);
        if (chsStartResult.isOk())
            newEntry.chsStart = chsStartResult.value();
        if (chsEndResult.isOk())
            newEntry.chsEnd = chsEndResult.value();
    }

    m_entries.push_back(newEntry);
    return Result<void>::ok();
}

Result<void> MbrPartitionTable::deletePartition(int index)
{
    // Find the entry with this index
    auto it = std::find_if(m_entries.begin(), m_entries.end(),
        [index](const PartitionEntry& e) { return e.index == index; });

    if (it == m_entries.end())
        return ErrorInfo::fromCode(ErrorCode::PartitionNotFound, "Partition index not found");

    // If deleting an extended partition, also remove all logical partitions
    if (it->isExtended && !it->isLogical)
    {
        m_entries.erase(
            std::remove_if(m_entries.begin(), m_entries.end(),
                [](const PartitionEntry& e) { return e.isLogical; }),
            m_entries.end());
    }

    // Remove the partition itself (re-find after potential removal above)
    it = std::find_if(m_entries.begin(), m_entries.end(),
        [index](const PartitionEntry& e) { return e.index == index; });
    if (it != m_entries.end())
        m_entries.erase(it);

    // Re-index
    for (int i = 0; i < static_cast<int>(m_entries.size()); i++)
        m_entries[i].index = i;

    return Result<void>::ok();
}

Result<void> MbrPartitionTable::resizePartition(int index, SectorOffset newStart, SectorCount newSize)
{
    auto it = std::find_if(m_entries.begin(), m_entries.end(),
        [index](const PartitionEntry& e) { return e.index == index; });

    if (it == m_entries.end())
        return ErrorInfo::fromCode(ErrorCode::PartitionNotFound, "Partition index not found");

    if (newSize == 0)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Partition size cannot be zero");

    // Check bounds
    if (newStart + newSize > m_diskSizeBytes / m_sectorSize)
        return ErrorInfo::fromCode(ErrorCode::PartitionTooLarge, "Resized partition exceeds disk boundary");

    if (newStart > 0xFFFFFFFFULL || newSize > 0xFFFFFFFFULL)
        return ErrorInfo::fromCode(ErrorCode::PartitionTooLarge, "MBR cannot address beyond 2 TiB");

    // Check overlaps, excluding self
    if (overlapsExisting(newStart, newSize, index))
        return ErrorInfo::fromCode(ErrorCode::PartitionOverlap, "Resized partition overlaps another partition");

    it->startLba = newStart;
    it->sectorCount = newSize;

    return Result<void>::ok();
}

Result<void> MbrPartitionTable::setActivePartition(int index)
{
    // Clear all active flags first
    for (auto& entry : m_entries)
    {
        if (!entry.isLogical)
            entry.isActive = false;
    }

    if (index >= 0)
    {
        auto it = std::find_if(m_entries.begin(), m_entries.end(),
            [index](const PartitionEntry& e) { return e.index == index && !e.isLogical; });

        if (it == m_entries.end())
            return ErrorInfo::fromCode(ErrorCode::PartitionNotFound,
                "Primary partition index not found");
        it->isActive = true;
    }

    return Result<void>::ok();
}

Result<std::vector<uint8_t>> MbrPartitionTable::serialize() const
{
    // Build a 512-byte MBR sector.
    // NOTE: This only serializes the primary MBR. EBR chain serialization for logical
    // partitions would require additional sectors — the caller must write those separately.
    std::vector<uint8_t> mbr(MBR_SIZE, 0);

    // Boot code (bytes 0-439)
    std::memcpy(mbr.data(), m_bootCode.data(), 440);

    // Disk signature at bytes 440-443
    writeLE32(mbr.data() + 440, m_diskSignature);

    // Reserved at bytes 444-445
    writeLE16(mbr.data() + 444, m_reserved);

    // Write up to 4 primary entries
    int primaryIdx = 0;
    for (const auto& entry : m_entries)
    {
        if (entry.isLogical)
            continue;

        if (primaryIdx >= MBR_MAX_PRIMARY_PARTITIONS)
            break;

        uint8_t* dest = mbr.data() + MBR_PARTITION_ENTRY_OFFSET + (primaryIdx * MBR_PARTITION_ENTRY_SIZE);

        dest[0] = entry.isActive ? 0x80 : 0x00;

        // CHS encoding
        if (entry.startLba > 16450559ULL)
        {
            encodeCHSOverflow(dest + 1);
            encodeCHSOverflow(dest + 5);
        }
        else
        {
            encodeCHS(entry.chsStart, dest + 1);
            encodeCHS(entry.chsEnd, dest + 5);
        }

        dest[4] = entry.mbrType;
        writeLE32(dest + 8, static_cast<uint32_t>(entry.startLba));
        writeLE32(dest + 12, static_cast<uint32_t>(entry.sectorCount));

        primaryIdx++;
    }

    // Signature
    writeLE16(mbr.data() + 510, MBR_SIGNATURE);

    // Now serialize EBR chain for logical partitions
    std::vector<PartitionEntry> logicals;
    for (const auto& entry : m_entries)
    {
        if (entry.isLogical)
            logicals.push_back(entry);
    }

    if (!logicals.empty())
    {
        // Find the extended partition for base LBA
        int extIdx = findExtendedIndex();
        if (extIdx >= 0)
        {
            SectorOffset extStartLba = m_entries[extIdx].startLba;

            for (size_t i = 0; i < logicals.size(); i++)
            {
                // Each EBR is a 512-byte sector
                std::vector<uint8_t> ebr(MBR_SIZE, 0);

                // Entry 0: the logical partition itself
                // LBA start is relative to this EBR's location
                uint8_t* e0 = ebr.data() + MBR_PARTITION_ENTRY_OFFSET;
                e0[0] = logicals[i].isActive ? 0x80 : 0x00;
                encodeCHSOverflow(e0 + 1);
                e0[4] = logicals[i].mbrType;
                encodeCHSOverflow(e0 + 5);

                // For the EBR, the logical partition's LBA is relative to the EBR
                // The EBR sits 1 sector before the logical partition data
                SectorOffset ebrLba = logicals[i].startLba - 1;
                writeLE32(e0 + 8, 1); // Starts 1 sector after EBR
                writeLE32(e0 + 12, static_cast<uint32_t>(logicals[i].sectorCount));

                // Entry 1: pointer to next EBR (relative to extended partition start)
                if (i + 1 < logicals.size())
                {
                    uint8_t* e1 = ebr.data() + MBR_PARTITION_ENTRY_OFFSET + MBR_PARTITION_ENTRY_SIZE;
                    encodeCHSOverflow(e1 + 1);
                    e1[4] = MbrTypes::Extended;
                    encodeCHSOverflow(e1 + 5);

                    SectorOffset nextEbrLba = logicals[i + 1].startLba - 1;
                    writeLE32(e1 + 8, static_cast<uint32_t>(nextEbrLba - extStartLba));
                    writeLE32(e1 + 12, static_cast<uint32_t>(logicals[i + 1].sectorCount + 1));
                }

                writeLE16(ebr.data() + 510, MBR_SIGNATURE);

                // Append EBR sector to output
                mbr.insert(mbr.end(), ebr.begin(), ebr.end());
            }
        }
    }

    return mbr;
}

// ============================================================================
// GptPartitionTable implementation
// ============================================================================

GptPartitionTable::GptPartitionTable()
{
    m_protectiveMbr.fill(0);
}

Result<void> GptPartitionTable::parse(const DiskReadCallback& readFunc)
{
    // Read sector 0 (protective MBR) for preservation
    auto sector0Result = readFunc(0, m_sectorSize);
    if (sector0Result.isError())
        return sector0Result.error();

    const auto& sector0 = sector0Result.value();
    if (sector0.size() >= 512)
        std::memcpy(m_protectiveMbr.data(), sector0.data(), 512);

    // Read GPT header at LBA 1
    auto headerResult = readFunc(static_cast<uint64_t>(m_sectorSize), m_sectorSize);
    if (headerResult.isError())
        return headerResult.error();

    const auto& headerData = headerResult.value();
    if (headerData.size() < GPT_HEADER_SIZE)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt, "GPT header too small");

    // Validate signature: "EFI PART" = 0x5452415020494645
    uint64_t signature = readLE64(headerData.data());
    if (signature != GPT_HEADER_SIGNATURE)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt,
            "Invalid GPT header signature (expected 'EFI PART')");

    m_revision = readLE32(headerData.data() + 8);
    uint32_t headerSize = readLE32(headerData.data() + 12);
    uint32_t headerCrc = readLE32(headerData.data() + 16);
    // uint32_t reserved = readLE32(headerData.data() + 20); // Must be zero

    uint64_t myLba = readLE64(headerData.data() + 24);
    m_alternateLba = readLE64(headerData.data() + 32);
    m_firstUsableLba = readLE64(headerData.data() + 40);
    m_lastUsableLba = readLE64(headerData.data() + 48);

    // Disk GUID at offset 56 (16 bytes)
    m_diskGuid = guidFromBytes(headerData.data() + 56);

    uint64_t entryLba = readLE64(headerData.data() + 72);
    m_entryCount = readLE32(headerData.data() + 80);
    m_entrySize = readLE32(headerData.data() + 84);
    uint32_t entryCrc = readLE32(headerData.data() + 88);

    // Validate header CRC32
    // The CRC is computed with the headerCrc32 field zeroed
    {
        std::vector<uint8_t> headerCopy(headerData.begin(), headerData.begin() + headerSize);
        writeLE32(headerCopy.data() + 16, 0); // Zero out CRC field
        uint32_t computedCrc = crc32(headerCopy.data(), headerSize);
        if (computedCrc != headerCrc)
        {
            log::warn("GPT header CRC32 mismatch (stored vs computed). Attempting to continue.");
            // We don't fail here — the backup header may be valid, and we want to show
            // what we can parse even from a slightly corrupted table.
        }
    }

    // Sanity checks
    if (m_entrySize < GPT_ENTRY_SIZE)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt,
            "GPT entry size too small (minimum 128 bytes)");

    if (m_entryCount > 1024) // Reasonable upper bound
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt,
            "GPT entry count unreasonably large");

    // Read the entire partition entry array
    uint32_t entryArrayBytes = m_entryCount * m_entrySize;
    auto entryResult = readFunc(entryLba * m_sectorSize, entryArrayBytes);
    if (entryResult.isError())
        return entryResult.error();

    const auto& entryData = entryResult.value();

    // Validate entry array CRC32
    {
        uint32_t computedEntryCrc = crc32(entryData.data(), entryArrayBytes);
        if (computedEntryCrc != entryCrc)
        {
            log::warn("GPT partition entry array CRC32 mismatch");
        }
    }

    return parseEntries(entryData);
}

Result<void> GptPartitionTable::parseEntries(const std::vector<uint8_t>& entryData)
{
    m_entries.clear();

    for (uint32_t i = 0; i < m_entryCount; i++)
    {
        size_t offset = static_cast<size_t>(i) * m_entrySize;
        if (offset + GPT_ENTRY_SIZE > entryData.size())
            break;

        const uint8_t* entry = entryData.data() + offset;

        // Type GUID at offset 0
        Guid typeGuid = guidFromBytes(entry);

        // Skip unused entries (all-zero type GUID)
        if (typeGuid.isZero())
            continue;

        PartitionEntry pe;
        pe.index = static_cast<int>(i);
        pe.typeGuid = typeGuid;
        pe.uniqueGuid = guidFromBytes(entry + 16);
        pe.startLba = readLE64(entry + 32);
        pe.sectorCount = readLE64(entry + 40) - pe.startLba + 1; // endLba is inclusive
        pe.sectorSize = m_sectorSize;
        pe.gptAttributes = readLE64(entry + 48);

        // Name: UTF-16LE, 36 characters at offset 56
        const uint16_t* namePtr = reinterpret_cast<const uint16_t*>(entry + 56);
        pe.gptName = utf16leToUtf8(namePtr, 36);

        m_entries.push_back(pe);
    }

    return Result<void>::ok();
}

Result<void> GptPartitionTable::parseBackup(const DiskReadCallback& readFunc)
{
    // Backup GPT header is at the last LBA of the disk
    if (m_alternateLba == 0)
    {
        // Calculate from disk size if we don't have it from primary header
        m_alternateLba = (m_diskSizeBytes / m_sectorSize) - 1;
    }

    auto headerResult = readFunc(m_alternateLba * m_sectorSize, m_sectorSize);
    if (headerResult.isError())
        return headerResult.error();

    const auto& headerData = headerResult.value();
    if (headerData.size() < GPT_HEADER_SIZE)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt, "Backup GPT header too small");

    uint64_t signature = readLE64(headerData.data());
    if (signature != GPT_HEADER_SIGNATURE)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt, "Invalid backup GPT header signature");

    // Read backup entry array
    // In backup GPT, the entry array is located BEFORE the header
    uint64_t entryLba = readLE64(headerData.data() + 72);
    uint32_t entryCount = readLE32(headerData.data() + 80);
    uint32_t entrySize = readLE32(headerData.data() + 84);
    uint32_t entryArrayBytes = entryCount * entrySize;

    auto entryResult = readFunc(entryLba * m_sectorSize, entryArrayBytes);
    if (entryResult.isError())
        return entryResult.error();

    // Parse the backup entries (overwrite current entries)
    m_entryCount = entryCount;
    m_entrySize = entrySize;
    return parseEntries(entryResult.value());
}

std::vector<PartitionEntry> GptPartitionTable::partitions() const
{
    return m_entries;
}

bool GptPartitionTable::overlapsExisting(SectorOffset start, SectorOffset end, int excludeIndex) const
{
    for (const auto& entry : m_entries)
    {
        if (entry.index == excludeIndex)
            continue;

        SectorOffset entryEnd = entry.startLba + entry.sectorCount - 1;
        if (start <= entryEnd && entry.startLba <= end)
            return true;
    }
    return false;
}

Result<void> GptPartitionTable::addPartition(const PartitionParams& params)
{
    if (params.sectorCount == 0)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Partition size cannot be zero");

    // Check if within usable range
    if (m_lastUsableLba == 0)
    {
        // Calculate if not set
        uint64_t diskSectors = m_diskSizeBytes / m_sectorSize;
        // Reserve 34 sectors at end for backup GPT (header + 32 sectors of entries)
        m_lastUsableLba = diskSectors - 34;
    }

    if (params.startLba < m_firstUsableLba)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Partition start is before first usable LBA");

    SectorOffset endLba = params.startLba + params.sectorCount - 1;
    if (endLba > m_lastUsableLba)
        return ErrorInfo::fromCode(ErrorCode::PartitionTooLarge,
            "Partition extends beyond last usable LBA");

    // Check overlap
    if (overlapsExisting(params.startLba, endLba))
        return ErrorInfo::fromCode(ErrorCode::PartitionOverlap,
            "New partition overlaps an existing partition");

    // Find a free slot in the entry array
    int freeSlot = -1;
    std::vector<bool> usedSlots(m_entryCount, false);
    for (const auto& entry : m_entries)
    {
        if (entry.index >= 0 && entry.index < static_cast<int>(m_entryCount))
            usedSlots[entry.index] = true;
    }
    for (int i = 0; i < static_cast<int>(m_entryCount); i++)
    {
        if (!usedSlots[i])
        {
            freeSlot = i;
            break;
        }
    }

    if (freeSlot < 0)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableFull,
            "No free GPT entry slots available");

    PartitionEntry newEntry;
    newEntry.index = freeSlot;
    newEntry.startLba = params.startLba;
    newEntry.sectorCount = params.sectorCount;
    newEntry.sectorSize = m_sectorSize;
    newEntry.typeGuid = params.typeGuid.isZero() ? GptTypes::microsoftBasicData() : params.typeGuid;
    newEntry.uniqueGuid = Guid::generate();
    newEntry.gptAttributes = 0;
    newEntry.gptName = params.gptName;

    m_entries.push_back(newEntry);
    return Result<void>::ok();
}

Result<void> GptPartitionTable::deletePartition(int index)
{
    auto it = std::find_if(m_entries.begin(), m_entries.end(),
        [index](const PartitionEntry& e) { return e.index == index; });

    if (it == m_entries.end())
        return ErrorInfo::fromCode(ErrorCode::PartitionNotFound, "GPT partition index not found");

    m_entries.erase(it);
    return Result<void>::ok();
}

Result<void> GptPartitionTable::resizePartition(int index, SectorOffset newStart, SectorCount newSize)
{
    auto it = std::find_if(m_entries.begin(), m_entries.end(),
        [index](const PartitionEntry& e) { return e.index == index; });

    if (it == m_entries.end())
        return ErrorInfo::fromCode(ErrorCode::PartitionNotFound, "GPT partition index not found");

    if (newSize == 0)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Partition size cannot be zero");

    if (newStart < m_firstUsableLba)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Partition start before first usable LBA");

    SectorOffset newEnd = newStart + newSize - 1;
    if (newEnd > m_lastUsableLba)
        return ErrorInfo::fromCode(ErrorCode::PartitionTooLarge, "Resized partition exceeds last usable LBA");

    if (overlapsExisting(newStart, newEnd, index))
        return ErrorInfo::fromCode(ErrorCode::PartitionOverlap, "Resized partition overlaps another partition");

    it->startLba = newStart;
    it->sectorCount = newSize;

    return Result<void>::ok();
}

Result<void> GptPartitionTable::validateCrcs(const DiskReadCallback& readFunc) const
{
    // Re-read header and validate
    auto headerResult = readFunc(static_cast<uint64_t>(m_sectorSize), m_sectorSize);
    if (headerResult.isError())
        return headerResult.error();

    const auto& headerData = headerResult.value();
    uint32_t headerSize = readLE32(headerData.data() + 12);
    uint32_t storedHeaderCrc = readLE32(headerData.data() + 16);

    std::vector<uint8_t> headerCopy(headerData.begin(), headerData.begin() + headerSize);
    writeLE32(headerCopy.data() + 16, 0);
    uint32_t computedHeaderCrc = crc32(headerCopy.data(), headerSize);

    if (computedHeaderCrc != storedHeaderCrc)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt,
            "GPT header CRC32 mismatch: stored=0x" +
            ([&]{ std::ostringstream ss; ss << std::hex << storedHeaderCrc; return ss.str(); })() +
            " computed=0x" +
            ([&]{ std::ostringstream ss; ss << std::hex << computedHeaderCrc; return ss.str(); })());

    // Validate entry array CRC
    uint64_t entryLba = readLE64(headerData.data() + 72);
    uint32_t entryCount = readLE32(headerData.data() + 80);
    uint32_t entrySize = readLE32(headerData.data() + 84);
    uint32_t storedEntryCrc = readLE32(headerData.data() + 88);

    uint32_t entryArrayBytes = entryCount * entrySize;
    auto entryResult = readFunc(entryLba * m_sectorSize, entryArrayBytes);
    if (entryResult.isError())
        return entryResult.error();

    uint32_t computedEntryCrc = crc32(entryResult.value().data(), entryArrayBytes);
    if (computedEntryCrc != storedEntryCrc)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt,
            "GPT partition entry array CRC32 mismatch");

    return Result<void>::ok();
}

Result<std::vector<uint8_t>> GptPartitionTable::serialize() const
{
    // GPT layout on disk:
    //   LBA 0:        Protective MBR (512 bytes)
    //   LBA 1:        Primary GPT header (1 sector)
    //   LBA 2..33:    Primary partition entry array (128 entries * 128 bytes = 16384 bytes = 32 sectors)
    //   ...
    //   LBA N-33..N-2: Backup partition entry array
    //   LBA N-1:       Backup GPT header
    //
    // We serialize: protective MBR + primary header + entry array + backup entry array + backup header.
    // The caller is responsible for writing backup to the correct disk offset.

    uint64_t diskSectors = m_diskSizeBytes / m_sectorSize;
    uint32_t entryArrayBytes = m_entryCount * m_entrySize;
    uint32_t entryArraySectors = (entryArrayBytes + m_sectorSize - 1) / m_sectorSize;

    // Build protective MBR
    std::vector<uint8_t> protMbr(m_sectorSize, 0);
    std::memcpy(protMbr.data(), m_protectiveMbr.data(), std::min<size_t>(512, m_sectorSize));

    // Ensure it has a proper protective MBR entry
    {
        uint8_t* entry0 = protMbr.data() + MBR_PARTITION_ENTRY_OFFSET;
        entry0[0] = 0x00; // Not active
        entry0[1] = 0x00; // CHS start: 0/0/2
        entry0[2] = 0x02;
        entry0[3] = 0x00;
        entry0[4] = MbrTypes::GPT_Protective;
        encodeCHSOverflow(entry0 + 5);

        // LBA start = 1
        writeLE32(entry0 + 8, 1);

        // Size: entire disk (capped at 0xFFFFFFFF for MBR 32-bit field)
        uint64_t protSize = diskSectors - 1;
        if (protSize > 0xFFFFFFFFULL)
            protSize = 0xFFFFFFFFULL;
        writeLE32(entry0 + 12, static_cast<uint32_t>(protSize));

        // Clear other entries
        std::memset(protMbr.data() + MBR_PARTITION_ENTRY_OFFSET + 16, 0, 48);

        // MBR signature
        writeLE16(protMbr.data() + 510, MBR_SIGNATURE);
    }

    // Build partition entry array
    std::vector<uint8_t> entryArray(entryArrayBytes, 0);
    for (const auto& pe : m_entries)
    {
        if (pe.index < 0 || pe.index >= static_cast<int>(m_entryCount))
            continue;

        uint8_t* dest = entryArray.data() + (static_cast<size_t>(pe.index) * m_entrySize);

        // Type GUID
        guidToBytes(pe.typeGuid, dest);
        // Unique GUID
        guidToBytes(pe.uniqueGuid, dest + 16);
        // Start LBA
        writeLE64(dest + 32, pe.startLba);
        // End LBA (inclusive)
        writeLE64(dest + 40, pe.startLba + pe.sectorCount - 1);
        // Attributes
        writeLE64(dest + 48, pe.gptAttributes);
        // Name (UTF-16LE)
        utf8ToUtf16le(pe.gptName, reinterpret_cast<uint16_t*>(dest + 56), 36);
    }

    uint32_t entryCrc = crc32(entryArray.data(), entryArrayBytes);

    // Build primary GPT header
    std::vector<uint8_t> primaryHeader(m_sectorSize, 0);
    writeLE64(primaryHeader.data(), GPT_HEADER_SIGNATURE);
    writeLE32(primaryHeader.data() + 8, m_revision);
    writeLE32(primaryHeader.data() + 12, GPT_HEADER_SIZE);
    // CRC32 filled below
    writeLE32(primaryHeader.data() + 20, 0); // Reserved
    writeLE64(primaryHeader.data() + 24, 1); // myLba = 1
    writeLE64(primaryHeader.data() + 32, diskSectors - 1); // alternateLba
    writeLE64(primaryHeader.data() + 40, m_firstUsableLba);

    uint64_t lastUsable = m_lastUsableLba;
    if (lastUsable == 0)
        lastUsable = diskSectors - entryArraySectors - 2;
    writeLE64(primaryHeader.data() + 48, lastUsable);

    guidToBytes(m_diskGuid, primaryHeader.data() + 56);
    writeLE64(primaryHeader.data() + 72, 2); // entryLba = 2
    writeLE32(primaryHeader.data() + 80, m_entryCount);
    writeLE32(primaryHeader.data() + 84, m_entrySize);
    writeLE32(primaryHeader.data() + 88, entryCrc);

    // Compute header CRC32
    {
        writeLE32(primaryHeader.data() + 16, 0);
        uint32_t headerCrc = crc32(primaryHeader.data(), GPT_HEADER_SIZE);
        writeLE32(primaryHeader.data() + 16, headerCrc);
    }

    // Build backup GPT header
    std::vector<uint8_t> backupHeader(m_sectorSize, 0);
    std::memcpy(backupHeader.data(), primaryHeader.data(), m_sectorSize);

    // Swap myLba and alternateLba
    writeLE64(backupHeader.data() + 24, diskSectors - 1);
    writeLE64(backupHeader.data() + 32, 1);
    // Backup entry array starts at (lastLBA - entryArraySectors)
    writeLE64(backupHeader.data() + 72, diskSectors - 1 - entryArraySectors);

    // Recompute backup header CRC
    {
        writeLE32(backupHeader.data() + 16, 0);
        uint32_t backupCrc = crc32(backupHeader.data(), GPT_HEADER_SIZE);
        writeLE32(backupHeader.data() + 16, backupCrc);
    }

    // Assemble output: protMBR + primaryHeader + entryArray + (gap) + backupEntryArray + backupHeader
    // For writing, we output them concatenated with metadata about where each piece goes.
    // The simplest approach: output protMBR + primaryHeader + entryArray, then backupEntryArray + backupHeader.
    std::vector<uint8_t> result;
    result.reserve(protMbr.size() + primaryHeader.size() + entryArray.size() +
                   entryArray.size() + backupHeader.size());

    // Primary side (LBA 0, 1, 2..33)
    result.insert(result.end(), protMbr.begin(), protMbr.end());
    result.insert(result.end(), primaryHeader.begin(), primaryHeader.end());

    // Pad entry array to sector boundary
    std::vector<uint8_t> paddedEntries(entryArraySectors * m_sectorSize, 0);
    std::memcpy(paddedEntries.data(), entryArray.data(), entryArrayBytes);
    result.insert(result.end(), paddedEntries.begin(), paddedEntries.end());

    // Backup entry array (same content, different location on disk)
    result.insert(result.end(), paddedEntries.begin(), paddedEntries.end());

    // Backup header
    result.insert(result.end(), backupHeader.begin(), backupHeader.end());

    return result;
}

// ============================================================================
// ApmPartitionTable implementation (read-only)
// ============================================================================

ApmPartitionTable::ApmPartitionTable() {}

Result<void> ApmPartitionTable::parse(const DiskReadCallback& readFunc)
{
    // Read block 0: Driver Descriptor Map
    auto block0Result = readFunc(0, 512);
    if (block0Result.isError())
        return block0Result.error();

    const auto& block0 = block0Result.value();
    if (block0.size() < 512)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt, "APM block 0 too small");

    uint16_t ddmSig = readBE16(block0.data());
    if (ddmSig != APM_DDM_SIGNATURE)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt,
            "Invalid APM Driver Descriptor Map signature");

    m_blockSize = readBE16(block0.data() + 2);
    m_blockCount = readBE32(block0.data() + 4);

    // Sanity check block size
    if (m_blockSize == 0 || m_blockSize > 65536)
    {
        log::warn("APM reports unusual block size, defaulting to 512");
        m_blockSize = 512;
    }

    // Read first partition map entry to determine map size
    auto entry1Result = readFunc(m_blockSize, m_blockSize);
    if (entry1Result.isError())
        return entry1Result.error();

    const auto& entry1 = entry1Result.value();
    if (entry1.size() < 512)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt, "APM partition entry too small");

    uint16_t pmSig = readBE16(entry1.data());
    if (pmSig != APM_SIGNATURE)
        return ErrorInfo::fromCode(ErrorCode::PartitionTableCorrupt, "Invalid APM partition map signature");

    uint32_t mapEntryCount = readBE32(entry1.data() + 4);

    // Cap at a reasonable limit
    if (mapEntryCount > 256)
    {
        log::warn("APM reports excessive entry count, capping at 256");
        mapEntryCount = 256;
    }

    m_entries.clear();

    // Parse all partition map entries (entries start at block 1)
    for (uint32_t i = 0; i < mapEntryCount; i++)
    {
        uint64_t entryOffset = static_cast<uint64_t>(i + 1) * m_blockSize;
        auto entryResult = readFunc(entryOffset, m_blockSize);
        if (entryResult.isError())
            break;

        const auto& entryData = entryResult.value();
        if (entryData.size() < 512)
            break;

        uint16_t sig = readBE16(entryData.data());
        if (sig != APM_SIGNATURE)
            break;

        PartitionEntry pe;
        pe.index = static_cast<int>(i);
        pe.sectorSize = m_blockSize;

        // Physical block start and count
        pe.startLba = readBE32(entryData.data() + 8);
        pe.sectorCount = readBE32(entryData.data() + 12);

        // Name (null-terminated, up to 32 chars)
        char nameStr[33] = {};
        std::memcpy(nameStr, entryData.data() + 16, 32);
        pe.apmName = nameStr;

        // Type (null-terminated, up to 32 chars)
        char typeStr[33] = {};
        std::memcpy(typeStr, entryData.data() + 48, 32);
        pe.apmType = typeStr;

        // Map APM type strings to a descriptive label
        pe.label = pe.apmName;

        m_entries.push_back(pe);
    }

    return Result<void>::ok();
}

std::vector<PartitionEntry> ApmPartitionTable::partitions() const
{
    return m_entries;
}

Result<void> ApmPartitionTable::addPartition(const PartitionParams&)
{
    return ErrorInfo::fromCode(ErrorCode::NotImplemented, "APM partition modification is not supported (read-only)");
}

Result<void> ApmPartitionTable::deletePartition(int)
{
    return ErrorInfo::fromCode(ErrorCode::NotImplemented, "APM partition modification is not supported (read-only)");
}

Result<void> ApmPartitionTable::resizePartition(int, SectorOffset, SectorCount)
{
    return ErrorInfo::fromCode(ErrorCode::NotImplemented, "APM partition modification is not supported (read-only)");
}

Result<std::vector<uint8_t>> ApmPartitionTable::serialize() const
{
    return ErrorInfo::fromCode(ErrorCode::NotImplemented, "APM serialization is not supported (read-only)");
}

} // namespace spw
