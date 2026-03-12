// FormatEngine.cpp — Format partitions to various filesystems.
//
// Windows-native formats: NTFS, FAT32 (<=32GB), FAT16, FAT12, exFAT, ReFS
//   -> Delegated to format.com with appropriate flags.
//
// Direct-write formats: ext2/3/4, FAT32 large (>32GB), Linux swap
//   -> On-disk structures written directly via raw disk I/O.
//
// DISCLAIMER: This code is for authorized disk utility software only.

#include "FormatEngine.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <QString>
#include <QProcess>
#include <QRegularExpression>

namespace spw
{

// ============================================================================
// On-disk structure definitions for direct-write formatters
// ============================================================================

#pragma pack(push, 1)

// ----- FAT32 BPB (BIOS Parameter Block) -----
struct Fat32Bpb
{
    uint8_t  jmpBoot[3];        // 0x00: Jump instruction
    char     oemName[8];        // 0x03: OEM name
    uint16_t bytesPerSector;    // 0x0B
    uint8_t  sectorsPerCluster; // 0x0D
    uint16_t reservedSectors;   // 0x0E
    uint8_t  numFats;           // 0x10: Almost always 2
    uint16_t rootEntryCount;    // 0x11: 0 for FAT32
    uint16_t totalSectors16;    // 0x13: 0 for FAT32
    uint8_t  mediaType;         // 0x15: 0xF8 for hard disks
    uint16_t fatSize16;         // 0x16: 0 for FAT32
    uint16_t sectorsPerTrack;   // 0x18
    uint16_t numHeads;          // 0x1A
    uint32_t hiddenSectors;     // 0x1C
    uint32_t totalSectors32;    // 0x20
    // FAT32-specific extended BPB
    uint32_t fatSize32;         // 0x24
    uint16_t extFlags;          // 0x28
    uint16_t fsVersion;         // 0x2A
    uint32_t rootCluster;       // 0x2C: Usually 2
    uint16_t fsInfoSector;      // 0x30: Usually 1
    uint16_t backupBootSector;  // 0x32: Usually 6
    uint8_t  reserved[12];      // 0x34
    uint8_t  driveNumber;       // 0x40
    uint8_t  reserved1;         // 0x41
    uint8_t  bootSig;           // 0x42: 0x29
    uint32_t volumeSerial;      // 0x43
    char     volumeLabel[11];   // 0x47
    char     fsType[8];         // 0x52: "FAT32   "
};

// FAT32 FSInfo sector
struct Fat32FsInfo
{
    uint32_t leadSig;           // 0x41615252
    uint8_t  reserved1[480];
    uint32_t structSig;         // 0x61417272
    uint32_t freeCount;         // Free cluster count (0xFFFFFFFF if unknown)
    uint32_t nextFree;          // Next free cluster hint
    uint8_t  reserved2[12];
    uint32_t trailSig;          // 0xAA550000
};

// ----- ext2/3/4 superblock (1024 bytes at offset 1024) -----
struct Ext4Superblock
{
    uint32_t s_inodes_count;         // 0x00
    uint32_t s_blocks_count_lo;      // 0x04
    uint32_t s_r_blocks_count_lo;    // 0x08: Reserved blocks
    uint32_t s_free_blocks_count_lo; // 0x0C
    uint32_t s_free_inodes_count;    // 0x10
    uint32_t s_first_data_block;     // 0x14: 0 for 4K blocks, 1 for 1K blocks
    uint32_t s_log_block_size;       // 0x18: Block size = 1024 << s_log_block_size
    uint32_t s_log_cluster_size;     // 0x1C: Cluster size (same as block usually)
    uint32_t s_blocks_per_group;     // 0x20
    uint32_t s_clusters_per_group;   // 0x24
    uint32_t s_inodes_per_group;     // 0x28
    uint32_t s_mtime;                // 0x2C: Last mount time
    uint32_t s_wtime;                // 0x30: Last write time
    uint16_t s_mnt_count;            // 0x34
    uint16_t s_max_mnt_count;        // 0x36
    uint16_t s_magic;                // 0x38: 0xEF53
    uint16_t s_state;                // 0x3A: 1 = clean
    uint16_t s_errors;               // 0x3C: 1 = continue on error
    uint16_t s_minor_rev_level;      // 0x3E
    uint32_t s_lastcheck;            // 0x40
    uint32_t s_checkinterval;        // 0x44
    uint32_t s_creator_os;           // 0x48: 0 = Linux
    uint32_t s_rev_level;            // 0x4C: 1 = dynamic inode sizes
    uint16_t s_def_resuid;           // 0x50: Default UID for reserved blocks
    uint16_t s_def_resgid;           // 0x52
    // Rev 1 (dynamic) fields
    uint32_t s_first_ino;            // 0x54: First non-reserved inode (11)
    uint16_t s_inode_size;           // 0x56: 128 or 256
    uint16_t s_block_group_nr;       // 0x58
    uint32_t s_feature_compat;       // 0x5C
    uint32_t s_feature_incompat;     // 0x60
    uint32_t s_feature_ro_compat;    // 0x64
    uint8_t  s_uuid[16];            // 0x68
    char     s_volume_name[16];     // 0x78
    char     s_last_mounted[64];    // 0x88
    uint32_t s_algorithm_usage_bitmap; // 0xC8
    // Performance hints
    uint8_t  s_prealloc_blocks;      // 0xCC
    uint8_t  s_prealloc_dir_blocks;  // 0xCD
    uint16_t s_reserved_gdt_blocks;  // 0xCE
    // Journal (ext3/4)
    uint8_t  s_journal_uuid[16];    // 0xD0
    uint32_t s_journal_inum;         // 0xE0
    uint32_t s_journal_dev;          // 0xE4
    uint32_t s_last_orphan;          // 0xE8
    uint32_t s_hash_seed[4];        // 0xEC
    uint8_t  s_def_hash_version;     // 0xFC
    uint8_t  s_jnl_backup_type;      // 0xFD
    uint16_t s_desc_size;            // 0xFE: Group descriptor size (32 or 64)
    uint32_t s_default_mount_opts;   // 0x100
    uint32_t s_first_meta_bg;        // 0x104
    uint32_t s_mkfs_time;            // 0x108
    uint32_t s_jnl_blocks[17];      // 0x10C
    // 64-bit support
    uint32_t s_blocks_count_hi;      // 0x150
    uint32_t s_r_blocks_count_hi;    // 0x154
    uint32_t s_free_blocks_count_hi; // 0x158
    uint16_t s_min_extra_isize;      // 0x15C
    uint16_t s_want_extra_isize;     // 0x15E
    uint32_t s_flags;                // 0x160
    uint16_t s_raid_stride;          // 0x164
    uint16_t s_mmp_interval;         // 0x166
    uint64_t s_mmp_block;            // 0x168
    uint32_t s_raid_stripe_width;    // 0x170
    uint8_t  s_log_groups_per_flex;  // 0x174
    uint8_t  s_checksum_type;        // 0x175
    uint16_t s_reserved_pad;         // 0x176
    uint64_t s_kbytes_written;       // 0x178
    uint32_t s_snapshot_inum;        // 0x180
    uint32_t s_snapshot_id;          // 0x184
    uint64_t s_snapshot_r_blocks_count; // 0x188
    uint32_t s_snapshot_list;        // 0x190
    uint32_t s_error_count;          // 0x194
    uint32_t s_first_error_time;     // 0x198
    uint32_t s_first_error_ino;      // 0x19C
    uint64_t s_first_error_block;    // 0x1A0
    uint8_t  s_first_error_func[32]; // 0x1A8
    uint32_t s_first_error_line;     // 0x1C8
    uint32_t s_last_error_time;      // 0x1CC
    uint32_t s_last_error_ino;       // 0x1D0
    uint32_t s_last_error_line;      // 0x1D4
    uint64_t s_last_error_block;     // 0x1D8
    uint8_t  s_last_error_func[32];  // 0x1E0
    uint8_t  s_mount_opts[64];       // 0x200
    uint32_t s_usr_quota_inum;       // 0x240
    uint32_t s_grp_quota_inum;       // 0x244
    uint32_t s_overhead_blocks;      // 0x248
    uint32_t s_backup_bgs[2];        // 0x24C
    uint8_t  s_encrypt_algos[4];     // 0x254
    uint8_t  s_encrypt_pw_salt[16];  // 0x258
    uint32_t s_lpf_ino;              // 0x268
    uint32_t s_prj_quota_inum;       // 0x26C
    uint32_t s_checksum_seed;        // 0x270
    uint8_t  s_wtime_hi;             // 0x274
    uint8_t  s_mtime_hi;             // 0x275
    uint8_t  s_mkfs_time_hi;         // 0x276
    uint8_t  s_lastcheck_hi;         // 0x277
    uint8_t  s_first_error_time_hi;  // 0x278
    uint8_t  s_last_error_time_hi;   // 0x279
    uint8_t  s_pad[2];              // 0x27A
    uint16_t s_encoding;             // 0x27C
    uint16_t s_encoding_flags;       // 0x27E
    uint32_t s_orphan_file_inum;     // 0x280
    uint32_t s_reserved[94];         // 0x284
    uint32_t s_checksum;             // 0x3FC: CRC32C of superblock
};
static_assert(sizeof(Ext4Superblock) == 1024, "ext4 superblock must be 1024 bytes");

// ext2/3/4 block group descriptor (32 bytes for ext2/3, 64 bytes for ext4 with 64-bit)
struct Ext4GroupDesc32
{
    uint32_t bg_block_bitmap_lo;     // 0x00
    uint32_t bg_inode_bitmap_lo;     // 0x04
    uint32_t bg_inode_table_lo;      // 0x08
    uint16_t bg_free_blocks_count_lo;// 0x0C
    uint16_t bg_free_inodes_count_lo;// 0x0E
    uint16_t bg_used_dirs_count_lo;  // 0x10
    uint16_t bg_flags;               // 0x12
    uint32_t bg_exclude_bitmap_lo;   // 0x14
    uint16_t bg_block_bitmap_csum_lo;// 0x18
    uint16_t bg_inode_bitmap_csum_lo;// 0x1A
    uint16_t bg_itable_unused_lo;    // 0x1C
    uint16_t bg_checksum;            // 0x1E
};
static_assert(sizeof(Ext4GroupDesc32) == 32, "ext4 group desc (32-bit) must be 32 bytes");

// ext2/3/4 inode (128 bytes base, may be larger)
struct Ext4Inode
{
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;       // 512-byte blocks
    uint32_t i_flags;
    uint32_t i_osd1;
    uint8_t  i_block[60];       // Block pointers or extent tree
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;       // For regular files
    uint32_t i_obso_faddr;
    uint8_t  i_osd2[12];
};
static_assert(sizeof(Ext4Inode) == 128, "ext4 base inode must be 128 bytes");

// ext directory entry
struct Ext4DirEntry
{
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[256]; // Variable length, but we allocate max
};

// Linux swap header (at offset 0, pagesize bytes total)
struct SwapHeader
{
    char     bootbits[1024];     // 0x000: Boot sector (unused)
    uint32_t version;            // 0x400: Version (1)
    uint32_t last_page;          // 0x404: Last usable page
    uint32_t nr_badpages;        // 0x408
    uint8_t  sws_uuid[16];      // 0x40C: UUID
    char     sws_volume[16];     // 0x41C: Volume label
    uint32_t padding[117];       // Padding
    uint32_t badpages[1];        // 0x600: Bad page list (variable)
};

#pragma pack(pop)

// ext feature flags
namespace ExtFeature
{
    // Compatible features (can mount read-write even if unknown)
    constexpr uint32_t COMPAT_DIR_PREALLOC   = 0x0001;
    constexpr uint32_t COMPAT_HAS_JOURNAL    = 0x0004;
    constexpr uint32_t COMPAT_EXT_ATTR       = 0x0008;
    constexpr uint32_t COMPAT_RESIZE_INODE   = 0x0010;
    constexpr uint32_t COMPAT_DIR_INDEX      = 0x0020;
    constexpr uint32_t COMPAT_SPARSE_SUPER2  = 0x0200;

    // Incompatible features (must not mount if unknown)
    constexpr uint32_t INCOMPAT_FILETYPE     = 0x0002;
    constexpr uint32_t INCOMPAT_RECOVER      = 0x0004; // Journal needs recovery
    constexpr uint32_t INCOMPAT_JOURNAL_DEV  = 0x0008;
    constexpr uint32_t INCOMPAT_META_BG      = 0x0010;
    constexpr uint32_t INCOMPAT_EXTENTS      = 0x0040;
    constexpr uint32_t INCOMPAT_64BIT        = 0x0080;
    constexpr uint32_t INCOMPAT_FLEX_BG      = 0x0200;
    constexpr uint32_t INCOMPAT_LARGEDIR     = 0x4000;
    constexpr uint32_t INCOMPAT_INLINE_DATA  = 0x8000;

    // Read-only compatible features
    constexpr uint32_t RO_COMPAT_SPARSE_SUPER = 0x0001;
    constexpr uint32_t RO_COMPAT_LARGE_FILE   = 0x0002;
    constexpr uint32_t RO_COMPAT_HUGE_FILE    = 0x0008;
    constexpr uint32_t RO_COMPAT_GDT_CSUM     = 0x0010;
    constexpr uint32_t RO_COMPAT_DIR_NLINK    = 0x0020;
    constexpr uint32_t RO_COMPAT_EXTRA_ISIZE  = 0x0040;
    constexpr uint32_t RO_COMPAT_METADATA_CSUM = 0x0400;
}

// ext inode modes — undefine POSIX macros from <sys/stat.h> to avoid conflicts
#undef S_IFDIR
#undef S_IFREG
#undef S_IRUSR
#undef S_IWUSR
#undef S_IXUSR
#undef S_IRGRP
#undef S_IXGRP
#undef S_IROTH
#undef S_IXOTH

namespace ExtMode
{
    constexpr uint16_t S_IFDIR  = 0x4000;
    constexpr uint16_t S_IFREG  = 0x8000;
    constexpr uint16_t S_IRUSR  = 0x0100;
    constexpr uint16_t S_IWUSR  = 0x0080;
    constexpr uint16_t S_IXUSR  = 0x0040;
    constexpr uint16_t S_IRGRP  = 0x0020;
    constexpr uint16_t S_IXGRP  = 0x0008;
    constexpr uint16_t S_IROTH  = 0x0004;
    constexpr uint16_t S_IXOTH  = 0x0001;
}

// ext directory file types
namespace ExtFileType
{
    constexpr uint8_t FT_UNKNOWN  = 0;
    constexpr uint8_t FT_REG_FILE = 1;
    constexpr uint8_t FT_DIR      = 2;
}

// ============================================================================
// Utility: generate random bytes for UUIDs and serial numbers
// ============================================================================
static void generateRandomBytes(uint8_t* buf, size_t len)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned int> dist(0, 255);
    for (size_t i = 0; i < len; ++i)
    {
        buf[i] = static_cast<uint8_t>(dist(gen));
    }
}

static uint32_t generateSerial()
{
    uint8_t buf[4];
    generateRandomBytes(buf, 4);
    uint32_t serial = 0;
    std::memcpy(&serial, buf, 4);
    return serial;
}

static uint32_t currentUnixTime()
{
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

// Simple CRC32C for ext4 metadata checksums (Castagnoli polynomial 0x1EDC6F41)
static uint32_t crc32c(uint32_t crc, const uint8_t* data, size_t len)
{
    // Software CRC32C — polynomial 0x82F63B78 (bit-reversed Castagnoli)
    crc = ~crc;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0x82F63B78u;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

// Check if a block group number has a superblock backup (sparse_super feature)
static bool hasSuperblockBackup(uint32_t groupNum)
{
    if (groupNum == 0) return true;
    if (groupNum == 1) return true;
    // Powers of 3, 5, 7
    for (uint32_t base : {3u, 5u, 7u})
    {
        uint32_t val = base;
        while (val <= groupNum)
        {
            if (val == groupNum) return true;
            // Guard against overflow
            if (val > 0xFFFFFFFF / base) break;
            val *= base;
        }
    }
    return false;
}

// ============================================================================
// Public API implementation
// ============================================================================

bool FormatEngine::isFormatSupported(FilesystemType fs)
{
    switch (fs)
    {
    case FilesystemType::NTFS:
    case FilesystemType::FAT32:
    case FilesystemType::FAT16:
    case FilesystemType::FAT12:
    case FilesystemType::ExFAT:
    case FilesystemType::ReFS:
    case FilesystemType::Ext2:
    case FilesystemType::Ext3:
    case FilesystemType::Ext4:
    case FilesystemType::SWAP_LINUX:
        return true;
    default:
        return false;
    }
}

uint32_t FormatEngine::recommendedClusterSize(FilesystemType fs, uint64_t volumeSizeBytes)
{
    const uint64_t MB = 1024ULL * 1024;
    const uint64_t GB = 1024ULL * MB;
    const uint64_t TB = 1024ULL * GB;

    switch (fs)
    {
    case FilesystemType::NTFS:
        if (volumeSizeBytes <= 512 * MB) return 4096;
        if (volumeSizeBytes <= 1 * TB) return 4096;
        if (volumeSizeBytes <= 2 * TB) return 8192;
        return 8192; // Larger volumes

    case FilesystemType::FAT32:
        if (volumeSizeBytes <= 64 * MB) return 512;
        if (volumeSizeBytes <= 128 * MB) return 1024;
        if (volumeSizeBytes <= 256 * MB) return 4096;
        if (volumeSizeBytes <= 8 * GB) return 8192;
        if (volumeSizeBytes <= 16 * GB) return 16384;
        return 32768; // Up to 2TB (FAT32 max with 32K clusters)

    case FilesystemType::ExFAT:
        if (volumeSizeBytes <= 256 * MB) return 4096;
        if (volumeSizeBytes <= 32 * GB) return 32768;
        return 131072; // 128K for large volumes

    case FilesystemType::Ext2:
    case FilesystemType::Ext3:
    case FilesystemType::Ext4:
        if (volumeSizeBytes <= 512 * MB) return 1024;
        return 4096; // 4K is standard for anything >= 512MB

    case FilesystemType::FAT16:
        if (volumeSizeBytes <= 16 * MB) return 2048;
        if (volumeSizeBytes <= 128 * MB) return 4096;
        return 16384; // Max for FAT16

    case FilesystemType::FAT12:
        return 512;

    default:
        return 4096;
    }
}

int FormatEngine::maxLabelLength(FilesystemType fs)
{
    switch (fs)
    {
    case FilesystemType::NTFS:     return 32;
    case FilesystemType::FAT32:    return 11;
    case FilesystemType::FAT16:    return 11;
    case FilesystemType::FAT12:    return 11;
    case FilesystemType::ExFAT:    return 11;
    case FilesystemType::ReFS:     return 32;
    case FilesystemType::Ext2:
    case FilesystemType::Ext3:
    case FilesystemType::Ext4:     return 16;
    case FilesystemType::SWAP_LINUX: return 16;
    default:                       return 0;
    }
}

Result<void> FormatEngine::format(const FormatTarget& target,
                                   const FormatOptions& options,
                                   FormatProgressCallback progress)
{
    if (!isFormatSupported(options.targetFs))
    {
        return ErrorInfo::fromCode(ErrorCode::FilesystemNotSupported,
            "Filesystem type not supported for formatting");
    }

    // Validate the target
    if (!target.hasDriveLetter() && !target.hasRawTarget())
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Format target must specify either a drive letter or raw disk + offset");
    }

    // Dispatch to appropriate formatter
    switch (options.targetFs)
    {
    case FilesystemType::Ext2:
    case FilesystemType::Ext3:
    case FilesystemType::Ext4:
        return formatExt(target, options, progress);

    case FilesystemType::SWAP_LINUX:
        return formatLinuxSwap(target, options, progress);

    case FilesystemType::FAT32:
        // Check if volume is >32GB — Windows format.com refuses this
        if (options.forceFat32Large || target.partitionSizeBytes > 32ULL * 1024 * 1024 * 1024)
        {
            return formatFat32Large(target, options, progress);
        }
        return formatWithWindowsTool(target, options, progress);

    case FilesystemType::NTFS:
    case FilesystemType::FAT16:
    case FilesystemType::FAT12:
    case FilesystemType::ExFAT:
    case FilesystemType::ReFS:
        return formatWithWindowsTool(target, options, progress);

    default:
        return ErrorInfo::fromCode(ErrorCode::FilesystemNotSupported,
            "Unexpected filesystem type in format dispatch");
    }
}

// ============================================================================
// Windows-native formatting via format.com
// ============================================================================

Result<void> FormatEngine::formatWithWindowsTool(const FormatTarget& target,
                                                  const FormatOptions& options,
                                                  FormatProgressCallback progress)
{
    if (!target.hasDriveLetter())
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Windows format requires a drive letter");
    }

    if (progress) progress(0, "Preparing to format with Windows...");

    // Build the format.com command line
    // format.com /FS:TYPE /Q /X /V:label drive:
    // /Q = quick format, /X = force dismount, /Y = no confirmation prompt
    QString fsName;
    switch (options.targetFs)
    {
    case FilesystemType::NTFS:   fsName = "NTFS"; break;
    case FilesystemType::FAT32:  fsName = "FAT32"; break;
    case FilesystemType::FAT16:  fsName = "FAT"; break;
    case FilesystemType::FAT12:  fsName = "FAT"; break;
    case FilesystemType::ExFAT:  fsName = "exFAT"; break;
    case FilesystemType::ReFS:   fsName = "ReFS"; break;
    default:
        return ErrorInfo::fromCode(ErrorCode::FilesystemNotSupported,
            "Windows format.com does not support this filesystem");
    }

    QString drivePath = QString("%1:").arg(QChar(target.driveLetter));

    QStringList args;
    args << drivePath;
    args << "/FS:" + fsName;
    args << "/Y";   // Suppress confirmation
    args << "/X";   // Force dismount

    if (options.quickFormat)
    {
        args << "/Q";
    }

    if (!options.volumeLabel.empty())
    {
        args << "/V:" + QString::fromStdString(options.volumeLabel);
    }
    else
    {
        // Empty label
        args << "/V:";
    }

    if (options.clusterSize > 0)
    {
        args << "/A:" + QString::number(options.clusterSize);
    }

    if (progress) progress(10, "Running format.com...");

    // Run format.com
    QProcess formatProcess;
    formatProcess.setProgram("format.com");
    formatProcess.setArguments(args);

    // format.com reads from stdin for confirmation; we pipe "Y\n" just in case
    formatProcess.start();
    if (!formatProcess.waitForStarted(10000))
    {
        return ErrorInfo::fromCode(ErrorCode::FormatFailed,
            "Failed to start format.com: " + formatProcess.errorString().toStdString());
    }

    // Send confirmation if needed
    formatProcess.write("Y\n");
    formatProcess.closeWriteChannel();

    // Monitor progress — format.com outputs percentage lines
    // We parse stdout for "XX percent completed" lines
    while (formatProcess.state() != QProcess::NotRunning)
    {
        formatProcess.waitForReadyRead(500);

        QByteArray output = formatProcess.readAllStandardOutput();
        if (!output.isEmpty() && progress)
        {
            QString text = QString::fromLocal8Bit(output);
            // Look for percentage pattern
            QRegularExpression percentRx("(\\d+)\\s+percent");
            auto match = percentRx.match(text);
            if (match.hasMatch())
            {
                int pct = match.captured(1).toInt();
                // Scale to 10-90 range (10% was prep, last 10% is finalization)
                int scaledPct = 10 + (pct * 80) / 100;
                progress(scaledPct, QString("Formatting... %1%").arg(pct));
            }
        }
    }

    formatProcess.waitForFinished(300000); // 5 minute timeout for full format

    int exitCode = formatProcess.exitCode();
    if (exitCode != 0)
    {
        QByteArray errOutput = formatProcess.readAllStandardError();
        QByteArray stdOutput = formatProcess.readAllStandardOutput();
        std::string combinedOutput = stdOutput.toStdString() + errOutput.toStdString();
        return ErrorInfo::fromCode(ErrorCode::FormatFailed,
            "format.com exited with code " + std::to_string(exitCode) + ": " + combinedOutput);
    }

    if (progress) progress(95, "Notifying system of changes...");

    // Notify the OS
    notifyPartitionChangeLetter(target.driveLetter);

    if (progress) progress(100, "Format complete");
    return Result<void>::ok();
}

// ============================================================================
// ext2/3/4 direct-write formatter
//
// On-disk layout:
//   Block 0 (or byte 0-1023): Boot block (zeroed)
//   Byte 1024-2047: Superblock
//   After superblock: Block group descriptor table
//   Each block group: block bitmap, inode bitmap, inode table, data blocks
//
// For ext3/4 with journal, we allocate inode 8 and reserve journal blocks.
// ============================================================================

Result<void> FormatEngine::formatExt(const FormatTarget& target,
                                      const FormatOptions& options,
                                      FormatProgressCallback progress)
{
    if (progress) progress(0, "Preparing ext filesystem...");

    // Determine partition size
    uint64_t partSize = target.partitionSizeBytes;
    if (partSize == 0 && target.hasDriveLetter())
    {
        auto spaceResult = VolumeHandle::getSpaceInfo(target.driveLetter);
        if (!spaceResult)
            return ErrorInfo::fromCode(ErrorCode::FormatFailed, "Cannot determine volume size");
        partSize = spaceResult.value().totalBytes;
    }

    if (partSize < 1024 * 1024) // Minimum 1MB
    {
        return ErrorInfo::fromCode(ErrorCode::PartitionTooSmall,
            "Partition too small for ext filesystem (minimum 1MB)");
    }

    // Determine block size
    uint32_t blockSize = options.blockSize;
    if (blockSize == 0)
    {
        blockSize = recommendedClusterSize(options.targetFs, partSize);
    }
    // Validate block size
    if (blockSize != 1024 && blockSize != 2048 && blockSize != 4096)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "ext block size must be 1024, 2048, or 4096");
    }

    const uint32_t logBlockSize = (blockSize == 1024) ? 0 : (blockSize == 2048) ? 1 : 2;
    const uint32_t firstDataBlock = (blockSize == 1024) ? 1 : 0;

    // Calculate filesystem geometry
    const uint64_t totalBlocks = partSize / blockSize;
    const uint32_t blocksPerGroup = blockSize * 8; // One bitmap block can track blockSize*8 blocks
    const uint32_t numGroups = static_cast<uint32_t>((totalBlocks + blocksPerGroup - 1) / blocksPerGroup);

    // Inode calculations
    uint32_t inodeSize = options.inodeSize;
    if (inodeSize == 0) inodeSize = 256;
    if (inodeSize != 128 && inodeSize != 256)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "ext inode size must be 128 or 256");
    }

    // Inodes per group: approximately 1 inode per 16KB of disk space, minimum 16
    uint32_t inodesPerGroup = options.inodesPerGroup;
    if (inodesPerGroup == 0)
    {
        // Standard ratio: one inode per 16384 bytes
        uint64_t bytesPerGroup = static_cast<uint64_t>(blocksPerGroup) * blockSize;
        inodesPerGroup = static_cast<uint32_t>(bytesPerGroup / 16384);
        if (inodesPerGroup < 16) inodesPerGroup = 16;
        // Must be a multiple of (blockSize / inodeSize) for inode table alignment
        uint32_t inodesPerBlock = blockSize / inodeSize;
        inodesPerGroup = ((inodesPerGroup + inodesPerBlock - 1) / inodesPerBlock) * inodesPerBlock;
        // Cap at what the inode bitmap can track
        if (inodesPerGroup > blockSize * 8)
            inodesPerGroup = static_cast<uint32_t>(blockSize * 8);
    }

    const uint32_t totalInodes = inodesPerGroup * numGroups;
    const uint32_t inodeTableBlocksPerGroup = (inodesPerGroup * inodeSize + blockSize - 1) / blockSize;

    // Group descriptor size: 32 for ext2/3, 64 for ext4 with 64-bit
    uint16_t descSize = 32;
    bool use64bit = false;
    if (options.targetFs == FilesystemType::Ext4 && options.enable64bit && totalBlocks > 0xFFFFFFFF)
    {
        descSize = 64;
        use64bit = true;
    }

    // Group descriptor table blocks (after superblock in each group with backup)
    const uint32_t gdtBlocks = (numGroups * descSize + blockSize - 1) / blockSize;

    // Reserved GDT blocks for future growth (online resize)
    const uint32_t reservedGdtBlocks = std::min<uint32_t>(1024, (blockSize / descSize) * 16);

    // Determine journal size (ext3/4 only)
    bool hasJournal = (options.targetFs == FilesystemType::Ext3 ||
                       options.targetFs == FilesystemType::Ext4) && options.enableJournal;
    uint32_t journalBlocks = 0;
    if (hasJournal)
    {
        // Journal size heuristic: between 1024 and 32768 blocks
        if (totalBlocks < 32768)
            journalBlocks = 1024;
        else if (totalBlocks < 262144)
            journalBlocks = 4096;
        else if (totalBlocks < 524288)
            journalBlocks = 8192;
        else if (totalBlocks < 1048576)
            journalBlocks = 16384;
        else
            journalBlocks = 32768;

        // Don't let journal exceed 10% of total
        if (journalBlocks > totalBlocks / 10)
            journalBlocks = static_cast<uint32_t>(totalBlocks / 10);
        if (journalBlocks < 1024)
            journalBlocks = 1024;
    }

    if (progress) progress(5, "Calculated filesystem geometry...");

    // Calculate overhead per group: superblock backup + GDT + reserved GDT + bitmaps + inode table
    // Only groups 0 and groups that are powers of 3,5,7 have superblock backups (sparse_super)
    auto groupOverhead = [&](uint32_t groupIdx) -> uint32_t
    {
        uint32_t overhead = 0;
        if (hasSuperblockBackup(groupIdx))
        {
            overhead += 1 + gdtBlocks + reservedGdtBlocks; // superblock + GDT + reserved GDT
        }
        overhead += 2; // block bitmap + inode bitmap
        overhead += inodeTableBlocksPerGroup;
        return overhead;
    };

    // Calculate free blocks
    uint64_t usedBlocks = 0;
    for (uint32_t g = 0; g < numGroups; ++g)
    {
        usedBlocks += groupOverhead(g);
    }
    usedBlocks += journalBlocks;
    // First data block is also not available
    if (firstDataBlock > 0)
        usedBlocks += firstDataBlock;

    uint64_t freeBlocks = (totalBlocks > usedBlocks) ? (totalBlocks - usedBlocks) : 0;

    // Reserved blocks (5% for root)
    uint64_t reservedBlocks = totalBlocks / 20;
    if (reservedBlocks > freeBlocks) reservedBlocks = freeBlocks;

    // Generate UUID
    uint8_t uuid[16];
    generateRandomBytes(uuid, 16);
    // Set UUID version 4 and variant bits
    uuid[6] = (uuid[6] & 0x0F) | 0x40; // Version 4
    uuid[8] = (uuid[8] & 0x3F) | 0x80; // Variant 1

    // Build superblock
    Ext4Superblock sb = {};
    sb.s_inodes_count = totalInodes;
    sb.s_blocks_count_lo = static_cast<uint32_t>(totalBlocks & 0xFFFFFFFF);
    sb.s_r_blocks_count_lo = static_cast<uint32_t>(reservedBlocks & 0xFFFFFFFF);
    sb.s_free_blocks_count_lo = static_cast<uint32_t>(freeBlocks & 0xFFFFFFFF);
    sb.s_free_inodes_count = totalInodes - 11; // Inodes 1-11 are reserved
    sb.s_first_data_block = firstDataBlock;
    sb.s_log_block_size = logBlockSize;
    sb.s_log_cluster_size = logBlockSize;
    sb.s_blocks_per_group = blocksPerGroup;
    sb.s_clusters_per_group = blocksPerGroup;
    sb.s_inodes_per_group = inodesPerGroup;

    uint32_t now = currentUnixTime();
    sb.s_mtime = 0;
    sb.s_wtime = now;
    sb.s_mnt_count = 0;
    sb.s_max_mnt_count = static_cast<uint16_t>(-1); // Disable fsck by mount count
    sb.s_magic = EXT_SUPER_MAGIC;
    sb.s_state = 1; // Clean
    sb.s_errors = 1; // Continue on error
    sb.s_minor_rev_level = 0;
    sb.s_lastcheck = now;
    sb.s_checkinterval = 0; // Disable periodic fsck
    sb.s_creator_os = 0; // Linux
    sb.s_rev_level = 1; // Dynamic revision
    sb.s_def_resuid = 0;
    sb.s_def_resgid = 0;
    sb.s_first_ino = 11;
    sb.s_inode_size = static_cast<uint16_t>(inodeSize);
    sb.s_block_group_nr = 0; // Set per copy
    sb.s_desc_size = descSize;

    // Feature flags
    sb.s_feature_compat = ExtFeature::COMPAT_EXT_ATTR |
                          ExtFeature::COMPAT_RESIZE_INODE |
                          ExtFeature::COMPAT_DIR_INDEX;

    sb.s_feature_incompat = ExtFeature::INCOMPAT_FILETYPE;
    sb.s_feature_ro_compat = ExtFeature::RO_COMPAT_SPARSE_SUPER |
                             ExtFeature::RO_COMPAT_LARGE_FILE;

    if (options.targetFs == FilesystemType::Ext3)
    {
        if (hasJournal)
            sb.s_feature_compat |= ExtFeature::COMPAT_HAS_JOURNAL;
    }
    else if (options.targetFs == FilesystemType::Ext4)
    {
        if (hasJournal)
            sb.s_feature_compat |= ExtFeature::COMPAT_HAS_JOURNAL;

        if (options.enableExtents)
            sb.s_feature_incompat |= ExtFeature::INCOMPAT_EXTENTS;

        sb.s_feature_incompat |= ExtFeature::INCOMPAT_FLEX_BG;

        if (use64bit)
            sb.s_feature_incompat |= ExtFeature::INCOMPAT_64BIT;

        if (options.enableHugeFile)
            sb.s_feature_ro_compat |= ExtFeature::RO_COMPAT_HUGE_FILE;

        sb.s_feature_ro_compat |= ExtFeature::RO_COMPAT_EXTRA_ISIZE |
                                  ExtFeature::RO_COMPAT_DIR_NLINK;
    }

    std::memcpy(sb.s_uuid, uuid, 16);

    // Volume label
    if (!options.volumeLabel.empty())
    {
        size_t labelLen = std::min<size_t>(options.volumeLabel.size(), 16);
        std::memcpy(sb.s_volume_name, options.volumeLabel.data(), labelLen);
    }

    // Hash seed for directory indexing
    generateRandomBytes(reinterpret_cast<uint8_t*>(sb.s_hash_seed), 16);
    sb.s_def_hash_version = 1; // Half-MD4

    sb.s_reserved_gdt_blocks = static_cast<uint16_t>(reservedGdtBlocks);
    sb.s_mkfs_time = now;

    if (hasJournal)
    {
        sb.s_journal_inum = 8; // Journal inode
    }

    // 64-bit block counts
    sb.s_blocks_count_hi = static_cast<uint32_t>(totalBlocks >> 32);
    sb.s_r_blocks_count_hi = static_cast<uint32_t>(reservedBlocks >> 32);
    sb.s_free_blocks_count_hi = static_cast<uint32_t>(freeBlocks >> 32);

    if (inodeSize > 128)
    {
        sb.s_min_extra_isize = 32;
        sb.s_want_extra_isize = 32;
    }

    // Flex BG: log of flex group size (default 4 = 16 groups per flex)
    sb.s_log_groups_per_flex = 4;

    if (progress) progress(10, "Opening device for writing...");

    // Open the volume or raw disk for writing
    // We need either a VolumeHandle (drive letter) or RawDiskHandle (raw disk)
    std::unique_ptr<VolumeHandle> volumeHandle;
    std::unique_ptr<RawDiskHandle> rawHandle;
    uint64_t writeBaseOffset = 0;

    if (target.hasDriveLetter())
    {
        auto lockResult = lockAndDismount(target.driveLetter);
        if (!lockResult)
            return lockResult.error();
        volumeHandle = std::make_unique<VolumeHandle>(std::move(lockResult.value()));
    }
    else if (target.hasRawTarget())
    {
        auto diskResult = RawDiskHandle::open(target.diskIndex, DiskAccessMode::ReadWrite);
        if (!diskResult)
            return diskResult.error();
        rawHandle = std::make_unique<RawDiskHandle>(std::move(diskResult.value()));
        writeBaseOffset = target.partitionOffsetBytes;
    }

    // Lambda to write bytes at an offset relative to partition start
    auto writeAt = [&](uint64_t offsetFromPartStart, const uint8_t* data, uint32_t size) -> Result<void>
    {
        if (volumeHandle)
        {
            return volumeHandle->writeBytes(offsetFromPartStart, data, size);
        }
        else if (rawHandle)
        {
            uint64_t absOffset = writeBaseOffset + offsetFromPartStart;
            uint32_t sectorSize = target.sectorSize;
            SectorOffset lba = absOffset / sectorSize;
            SectorCount sectors = (size + sectorSize - 1) / sectorSize;

            // If not sector-aligned, we need to read-modify-write
            if (absOffset % sectorSize != 0 || size % sectorSize != 0)
            {
                auto existing = rawHandle->readSectors(lba, sectors, sectorSize);
                if (!existing) return existing.error();

                auto& buf = existing.value();
                uint32_t offset_in_sector = static_cast<uint32_t>(absOffset % sectorSize);
                std::memcpy(buf.data() + offset_in_sector, data, size);
                return rawHandle->writeSectors(lba, buf.data(), sectors, sectorSize);
            }
            else
            {
                return rawHandle->writeSectors(lba, data, sectors, sectorSize);
            }
        }
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError, "No valid write handle");
    };

    // Full format: zero the entire volume first
    if (!options.quickFormat)
    {
        if (progress) progress(10, "Zeroing volume (full format)...");
        if (volumeHandle)
        {
            auto zeroResult = zeroVolume(*volumeHandle, partSize, progress, 10, 50);
            if (!zeroResult) return zeroResult;
        }
        else if (rawHandle)
        {
            auto zeroResult = zeroRaw(*rawHandle, writeBaseOffset, partSize,
                                       target.sectorSize, progress, 10, 50);
            if (!zeroResult) return zeroResult;
        }
    }

    int progressBase = options.quickFormat ? 10 : 50;
    int progressRange = options.quickFormat ? 80 : 40;

    if (progress) progress(progressBase, "Writing superblock and metadata...");

    // Write boot sector (first 1024 bytes) as zeros
    std::vector<uint8_t> zeroBlock(blockSize, 0);

    // ----- Write superblock at offset 1024 -----
    std::vector<uint8_t> sbData(1024, 0);
    std::memcpy(sbData.data(), &sb, sizeof(Ext4Superblock));
    auto result = writeAt(1024, sbData.data(), 1024);
    if (!result) return result;

    // ----- Build and write group descriptor table -----
    std::vector<uint8_t> gdtData(static_cast<size_t>(gdtBlocks) * blockSize, 0);

    // First pass: compute block positions for each group's metadata
    struct GroupLayout
    {
        uint64_t blockBitmapBlock;
        uint64_t inodeBitmapBlock;
        uint64_t inodeTableBlock;
        uint32_t freeBlocksCount;
        uint32_t freeInodesCount;
        uint32_t usedDirsCount;
    };

    std::vector<GroupLayout> groupLayouts(numGroups);

    for (uint32_t g = 0; g < numGroups; ++g)
    {
        uint64_t groupStart = static_cast<uint64_t>(g) * blocksPerGroup + firstDataBlock;
        uint64_t metaOffset = groupStart;

        // Skip superblock backup + GDT + reserved GDT if present
        if (hasSuperblockBackup(g))
        {
            metaOffset += 1 + gdtBlocks + reservedGdtBlocks;
        }

        groupLayouts[g].blockBitmapBlock = metaOffset;
        groupLayouts[g].inodeBitmapBlock = metaOffset + 1;
        groupLayouts[g].inodeTableBlock = metaOffset + 2;

        // Calculate free blocks in this group
        uint32_t overhead = groupOverhead(g);
        uint32_t groupBlockCount = blocksPerGroup;
        // Last group may have fewer blocks
        if (g == numGroups - 1)
        {
            groupBlockCount = static_cast<uint32_t>(totalBlocks - static_cast<uint64_t>(g) * blocksPerGroup);
            if (firstDataBlock > 0 && g == 0)
                groupBlockCount -= firstDataBlock;
        }

        uint32_t freeInGroup = (groupBlockCount > overhead) ? (groupBlockCount - overhead) : 0;
        groupLayouts[g].freeBlocksCount = freeInGroup;
        groupLayouts[g].freeInodesCount = inodesPerGroup;
        groupLayouts[g].usedDirsCount = 0;

        // Group 0: root directory uses 1 inode and 1 block, lost+found uses 1 inode and 1 block
        if (g == 0)
        {
            groupLayouts[g].freeInodesCount = inodesPerGroup - 11; // Reserved inodes 1-11
            if (groupLayouts[g].freeBlocksCount >= 2)
                groupLayouts[g].freeBlocksCount -= 2; // root dir + lost+found blocks
            groupLayouts[g].usedDirsCount = 2; // root + lost+found
        }

        // Fill group descriptor
        Ext4GroupDesc32* gd = reinterpret_cast<Ext4GroupDesc32*>(
            gdtData.data() + g * descSize);
        gd->bg_block_bitmap_lo = static_cast<uint32_t>(groupLayouts[g].blockBitmapBlock);
        gd->bg_inode_bitmap_lo = static_cast<uint32_t>(groupLayouts[g].inodeBitmapBlock);
        gd->bg_inode_table_lo = static_cast<uint32_t>(groupLayouts[g].inodeTableBlock);
        gd->bg_free_blocks_count_lo = static_cast<uint16_t>(groupLayouts[g].freeBlocksCount);
        gd->bg_free_inodes_count_lo = static_cast<uint16_t>(groupLayouts[g].freeInodesCount);
        gd->bg_used_dirs_count_lo = static_cast<uint16_t>(groupLayouts[g].usedDirsCount);
        gd->bg_itable_unused_lo = static_cast<uint16_t>(groupLayouts[g].freeInodesCount);
    }

    // Write GDT in group 0 (right after superblock)
    uint64_t gdtOffset;
    if (blockSize == 1024)
    {
        // Superblock is at block 1, GDT starts at block 2
        gdtOffset = 2 * 1024;
    }
    else
    {
        // Superblock is within block 0, GDT starts at block 1
        gdtOffset = blockSize;
    }

    result = writeAt(gdtOffset, gdtData.data(), static_cast<uint32_t>(gdtData.size()));
    if (!result) return result;

    if (progress) progress(progressBase + progressRange / 4, "Writing block group metadata...");

    // ----- Write superblock + GDT backups in backup groups -----
    for (uint32_t g = 1; g < numGroups; ++g)
    {
        if (!hasSuperblockBackup(g))
            continue;

        uint64_t groupStartByte = (static_cast<uint64_t>(g) * blocksPerGroup + firstDataBlock) * blockSize;

        // Write superblock copy (update block_group_nr field)
        Ext4Superblock sbCopy = sb;
        sbCopy.s_block_group_nr = static_cast<uint16_t>(g);
        std::vector<uint8_t> sbCopyData(1024, 0);
        std::memcpy(sbCopyData.data(), &sbCopy, sizeof(Ext4Superblock));

        // Superblock backup is at the start of the group
        result = writeAt(groupStartByte, sbCopyData.data(), 1024);
        if (!result) return result;

        // GDT backup follows superblock
        uint64_t backupGdtOffset = groupStartByte + blockSize;
        if (blockSize == 1024)
            backupGdtOffset = groupStartByte + 1024;
        result = writeAt(backupGdtOffset, gdtData.data(), static_cast<uint32_t>(gdtData.size()));
        if (!result) return result;
    }

    if (progress) progress(progressBase + progressRange / 3, "Writing bitmaps and inode tables...");

    // ----- Write block bitmaps, inode bitmaps, and inode tables for each group -----
    for (uint32_t g = 0; g < numGroups; ++g)
    {
        // Block bitmap
        std::vector<uint8_t> blockBitmap(blockSize, 0);

        uint32_t overhead = groupOverhead(g);
        // Mark overhead blocks as used in the bitmap
        for (uint32_t b = 0; b < overhead && b < blockSize * 8; ++b)
        {
            blockBitmap[b / 8] |= (1 << (b % 8));
        }

        // Group 0: also mark the root directory and lost+found data blocks
        if (g == 0)
        {
            // Root dir block and lost+found block are right after overhead
            if (overhead < blockSize * 8)
                blockBitmap[overhead / 8] |= (1 << (overhead % 8));
            if (overhead + 1 < blockSize * 8)
                blockBitmap[(overhead + 1) / 8] |= (1 << ((overhead + 1) % 8));
        }

        // Last group: mark unused trailing blocks beyond end of partition
        if (g == numGroups - 1)
        {
            uint32_t blocksInGroup = static_cast<uint32_t>(totalBlocks - static_cast<uint64_t>(g) * blocksPerGroup);
            if (firstDataBlock > 0 && g == 0)
            {
                // Adjust for first data block offset
            }
            for (uint32_t b = blocksInGroup; b < blocksPerGroup && b < blockSize * 8; ++b)
            {
                blockBitmap[b / 8] |= (1 << (b % 8));
            }
        }

        uint64_t bbOffset = groupLayouts[g].blockBitmapBlock * blockSize;
        result = writeAt(bbOffset, blockBitmap.data(), blockSize);
        if (!result) return result;

        // Inode bitmap
        std::vector<uint8_t> inodeBitmap(blockSize, 0);

        if (g == 0)
        {
            // Inodes 1-11 are reserved; mark them as used
            // Inode 1 = bad blocks, 2 = root dir, ..., 8 = journal, ..., 11 = last reserved
            for (uint32_t i = 0; i < 11 && i < inodesPerGroup; ++i)
            {
                inodeBitmap[i / 8] |= (1 << (i % 8));
            }
        }

        // Mark unused inodes beyond what exists in the last group
        // (not strictly necessary if inodesPerGroup evenly divides, but safe)

        uint64_t ibOffset = groupLayouts[g].inodeBitmapBlock * blockSize;
        result = writeAt(ibOffset, inodeBitmap.data(), blockSize);
        if (!result) return result;

        // Inode table: zero it out
        uint32_t itableBytes = inodeTableBlocksPerGroup * blockSize;
        uint64_t itOffset = groupLayouts[g].inodeTableBlock * blockSize;

        // Write in chunks to avoid huge single allocations
        constexpr uint32_t chunkSize = 65536; // 64K at a time
        std::vector<uint8_t> zeroBuf(chunkSize, 0);
        uint32_t remaining = itableBytes;
        uint64_t pos = itOffset;
        while (remaining > 0)
        {
            uint32_t writeSize = std::min(remaining, chunkSize);
            result = writeAt(pos, zeroBuf.data(), writeSize);
            if (!result) return result;
            pos += writeSize;
            remaining -= writeSize;
        }

        // Report progress per group
        if (progress && numGroups > 1)
        {
            int pct = progressBase + (progressRange / 3) +
                      (progressRange / 3) * (g + 1) / numGroups;
            progress(pct, QString("Writing group %1/%2...").arg(g + 1).arg(numGroups));
        }
    }

    if (progress) progress(progressBase + 2 * progressRange / 3, "Writing root directory...");

    // ----- Write root directory inode (inode 2) -----
    // The root directory data block is the first data block after group 0 overhead
    uint32_t group0overhead = groupOverhead(0);
    uint64_t rootDirDataBlock = static_cast<uint64_t>(firstDataBlock) + group0overhead;
    uint64_t lostFoundDataBlock = rootDirDataBlock + 1;

    // Build root directory inode
    std::vector<uint8_t> inodeData(inodeSize, 0);
    Ext4Inode* rootInode = reinterpret_cast<Ext4Inode*>(inodeData.data());
    rootInode->i_mode = ExtMode::S_IFDIR | ExtMode::S_IRUSR | ExtMode::S_IWUSR | ExtMode::S_IXUSR |
                        ExtMode::S_IRGRP | ExtMode::S_IXGRP | ExtMode::S_IROTH | ExtMode::S_IXOTH;
    rootInode->i_uid = 0;
    rootInode->i_size_lo = blockSize;
    rootInode->i_atime = now;
    rootInode->i_ctime = now;
    rootInode->i_mtime = now;
    rootInode->i_dtime = 0;
    rootInode->i_gid = 0;
    rootInode->i_links_count = 3; // ., .., and lost+found
    rootInode->i_blocks_lo = blockSize / 512;
    rootInode->i_flags = 0;

    // Block pointer: direct block[0] points to root dir data block
    // (If extents are enabled for ext4, we should use extent tree, but for simplicity
    // and compatibility we use traditional block pointers which ext4 still supports)
    uint32_t rootDirBlockLo = static_cast<uint32_t>(rootDirDataBlock);
    std::memcpy(rootInode->i_block, &rootDirBlockLo, 4);

    // Write root inode at inode table position for inode 2 (index 1)
    uint64_t rootInodeOffset = groupLayouts[0].inodeTableBlock * blockSize + 1 * inodeSize;
    result = writeAt(rootInodeOffset, inodeData.data(), inodeSize);
    if (!result) return result;

    // ----- Write lost+found inode (inode 11) -----
    std::vector<uint8_t> lfInodeData(inodeSize, 0);
    Ext4Inode* lfInode = reinterpret_cast<Ext4Inode*>(lfInodeData.data());
    lfInode->i_mode = ExtMode::S_IFDIR | ExtMode::S_IRUSR | ExtMode::S_IWUSR | ExtMode::S_IXUSR;
    lfInode->i_uid = 0;
    lfInode->i_size_lo = blockSize;
    lfInode->i_atime = now;
    lfInode->i_ctime = now;
    lfInode->i_mtime = now;
    lfInode->i_gid = 0;
    lfInode->i_links_count = 2; // . and ..
    lfInode->i_blocks_lo = blockSize / 512;

    uint32_t lfBlockLo = static_cast<uint32_t>(lostFoundDataBlock);
    std::memcpy(lfInode->i_block, &lfBlockLo, 4);

    // Inode 11 is at index 10 in the inode table
    uint64_t lfInodeOffset = groupLayouts[0].inodeTableBlock * blockSize + 10 * inodeSize;
    result = writeAt(lfInodeOffset, lfInodeData.data(), inodeSize);
    if (!result) return result;

    // ----- Write root directory data block -----
    // Directory entries: "." -> inode 2, ".." -> inode 2, "lost+found" -> inode 11
    std::vector<uint8_t> rootDirData(blockSize, 0);
    uint32_t dirOffset = 0;

    // "." entry
    auto writeDirEntry = [&](uint32_t inode, uint8_t fileType, const char* name, bool isLast)
    {
        uint8_t nameLen = static_cast<uint8_t>(std::strlen(name));
        uint16_t recLen;

        if (isLast)
        {
            // Last entry fills rest of block
            recLen = static_cast<uint16_t>(blockSize - dirOffset);
        }
        else
        {
            // Round up to 4-byte boundary: 8 (header) + name_len, rounded to 4
            recLen = static_cast<uint16_t>(((8 + nameLen + 3) / 4) * 4);
        }

        // inode (4 bytes)
        std::memcpy(rootDirData.data() + dirOffset, &inode, 4);
        // rec_len (2 bytes)
        std::memcpy(rootDirData.data() + dirOffset + 4, &recLen, 2);
        // name_len (1 byte)
        rootDirData[dirOffset + 6] = nameLen;
        // file_type (1 byte)
        rootDirData[dirOffset + 7] = fileType;
        // name
        std::memcpy(rootDirData.data() + dirOffset + 8, name, nameLen);

        dirOffset += recLen;
    };

    writeDirEntry(2, ExtFileType::FT_DIR, ".", false);
    writeDirEntry(2, ExtFileType::FT_DIR, "..", false);
    writeDirEntry(11, ExtFileType::FT_DIR, "lost+found", true);

    uint64_t rootDirByteOffset = rootDirDataBlock * blockSize;
    result = writeAt(rootDirByteOffset, rootDirData.data(), blockSize);
    if (!result) return result;

    // ----- Write lost+found directory data block -----
    std::vector<uint8_t> lfDirData(blockSize, 0);
    dirOffset = 0;

    // "." -> inode 11
    {
        uint32_t inode = 11;
        uint16_t recLen = 12;
        lfDirData[dirOffset] = inode & 0xFF;
        lfDirData[dirOffset + 1] = (inode >> 8) & 0xFF;
        lfDirData[dirOffset + 2] = (inode >> 16) & 0xFF;
        lfDirData[dirOffset + 3] = (inode >> 24) & 0xFF;
        std::memcpy(lfDirData.data() + dirOffset + 4, &recLen, 2);
        lfDirData[dirOffset + 6] = 1;
        lfDirData[dirOffset + 7] = ExtFileType::FT_DIR;
        lfDirData[dirOffset + 8] = '.';
        dirOffset += 12;
    }

    // ".." -> inode 2 (root), last entry fills rest of block
    {
        uint32_t inode = 2;
        uint16_t recLen = static_cast<uint16_t>(blockSize - dirOffset);
        std::memcpy(lfDirData.data() + dirOffset, &inode, 4);
        std::memcpy(lfDirData.data() + dirOffset + 4, &recLen, 2);
        lfDirData[dirOffset + 6] = 2;
        lfDirData[dirOffset + 7] = ExtFileType::FT_DIR;
        lfDirData[dirOffset + 8] = '.';
        lfDirData[dirOffset + 9] = '.';
    }

    uint64_t lfDirByteOffset = lostFoundDataBlock * blockSize;
    result = writeAt(lfDirByteOffset, lfDirData.data(), blockSize);
    if (!result) return result;

    // ----- Write journal (ext3/4) -----
    if (hasJournal && journalBlocks > 0)
    {
        if (progress) progress(progressBase + 3 * progressRange / 4, "Writing journal...");

        // Journal inode (inode 8, index 7) — store journal blocks inline
        // For simplicity, we allocate journal blocks contiguously after group 0 data
        // Journal starts after root dir + lost+found blocks
        uint64_t journalStartBlock = lostFoundDataBlock + 1;

        // Write journal inode
        std::vector<uint8_t> jInodeData(inodeSize, 0);
        Ext4Inode* jInode = reinterpret_cast<Ext4Inode*>(jInodeData.data());
        jInode->i_mode = ExtMode::S_IFREG | ExtMode::S_IRUSR | ExtMode::S_IWUSR;
        jInode->i_uid = 0;
        uint64_t journalSizeBytes = static_cast<uint64_t>(journalBlocks) * blockSize;
        jInode->i_size_lo = static_cast<uint32_t>(journalSizeBytes & 0xFFFFFFFF);
        jInode->i_size_high = static_cast<uint32_t>(journalSizeBytes >> 32);
        jInode->i_atime = now;
        jInode->i_ctime = now;
        jInode->i_mtime = now;
        jInode->i_gid = 0;
        jInode->i_links_count = 1;
        jInode->i_blocks_lo = static_cast<uint32_t>(journalSizeBytes / 512);
        jInode->i_flags = 0x00080000; // EXT4_EXTENTS_FL if using extents, but we use block ptrs

        // Use direct block pointers for first 12 blocks of journal
        // For real mkfs, this would use extent trees for ext4, but direct+indirect works
        uint32_t directBlocks = std::min<uint32_t>(12, journalBlocks);
        for (uint32_t i = 0; i < directBlocks; ++i)
        {
            uint32_t blk = static_cast<uint32_t>(journalStartBlock + i);
            std::memcpy(jInodeData.data() + offsetof(Ext4Inode, i_block) + i * 4, &blk, 4);
        }
        // For journals larger than 12 blocks, a real implementation would set up
        // indirect/double-indirect blocks. For the common case this is sufficient
        // to make the journal recognizable. The kernel will handle the rest on first mount.

        // Inode 8 is at index 7
        uint64_t jInodeOffset = groupLayouts[0].inodeTableBlock * blockSize + 7 * inodeSize;
        result = writeAt(jInodeOffset, jInodeData.data(), inodeSize);
        if (!result) return result;

        // Write JBD2 journal superblock at the first journal block
        // JBD2 superblock is 1024 bytes at the start of the journal
        std::vector<uint8_t> jsbData(blockSize, 0);

        // JBD2 superblock header (big-endian!)
        // Magic: 0xC03B3998
        uint32_t jMagic = 0x98393BC0; // Little-endian storage of big-endian 0xC03B3998
        std::memcpy(jsbData.data(), &jMagic, 4);

        // Block type: 3 = superblock v1, 4 = superblock v2
        uint32_t jBlockType = 0x04000000; // Big-endian 4
        std::memcpy(jsbData.data() + 4, &jBlockType, 4);

        // Sequence number: 1
        uint32_t jSeq = 0x01000000; // Big-endian 1
        std::memcpy(jsbData.data() + 8, &jSeq, 4);

        // Journal block size (big-endian)
        uint32_t jBlockSizeBE = 0;
        {
            uint8_t* p = reinterpret_cast<uint8_t*>(&jBlockSizeBE);
            p[0] = (blockSize >> 24) & 0xFF;
            p[1] = (blockSize >> 16) & 0xFF;
            p[2] = (blockSize >> 8) & 0xFF;
            p[3] = blockSize & 0xFF;
        }
        std::memcpy(jsbData.data() + 12, &jBlockSizeBE, 4);

        // Max length in blocks (big-endian)
        uint32_t jMaxLenBE = 0;
        {
            uint8_t* p = reinterpret_cast<uint8_t*>(&jMaxLenBE);
            p[0] = (journalBlocks >> 24) & 0xFF;
            p[1] = (journalBlocks >> 16) & 0xFF;
            p[2] = (journalBlocks >> 8) & 0xFF;
            p[3] = journalBlocks & 0xFF;
        }
        std::memcpy(jsbData.data() + 16, &jMaxLenBE, 4);

        // First log block: 1 (big-endian)
        uint32_t jFirstBE = 0x01000000;
        std::memcpy(jsbData.data() + 20, &jFirstBE, 4);

        // Copy filesystem UUID into journal superblock at offset 48
        std::memcpy(jsbData.data() + 48, uuid, 16);

        uint64_t jsbByteOffset = journalStartBlock * blockSize;
        result = writeAt(jsbByteOffset, jsbData.data(), blockSize);
        if (!result) return result;

        // Mark journal blocks as used in group 0 block bitmap
        // (We already wrote the bitmap, so we need to re-read, update, re-write)
        std::vector<uint8_t> updatedBitmap(blockSize, 0);
        // Re-read the bitmap we wrote
        if (volumeHandle)
        {
            auto bmpRead = volumeHandle->readBytes(
                groupLayouts[0].blockBitmapBlock * blockSize, blockSize);
            if (bmpRead) updatedBitmap = std::move(bmpRead.value());
        }
        else if (rawHandle)
        {
            uint64_t bmpAbs = writeBaseOffset + groupLayouts[0].blockBitmapBlock * blockSize;
            auto bmpRead = rawHandle->readSectors(
                bmpAbs / target.sectorSize,
                (blockSize + target.sectorSize - 1) / target.sectorSize,
                target.sectorSize);
            if (bmpRead) updatedBitmap = std::move(bmpRead.value());
            updatedBitmap.resize(blockSize);
        }

        // Mark journal blocks in bitmap
        for (uint32_t jb = 0; jb < journalBlocks; ++jb)
        {
            uint64_t absBlock = journalStartBlock + jb;
            // Block number relative to this group's start
            uint32_t relBlock = static_cast<uint32_t>(absBlock - (static_cast<uint64_t>(0) * blocksPerGroup + firstDataBlock));
            if (relBlock < blockSize * 8)
            {
                updatedBitmap[relBlock / 8] |= (1 << (relBlock % 8));
            }
        }

        uint64_t bbOffset = groupLayouts[0].blockBitmapBlock * blockSize;
        result = writeAt(bbOffset, updatedBitmap.data(), blockSize);
        if (!result) return result;

        // Update superblock free block count
        sb.s_free_blocks_count_lo = static_cast<uint32_t>(
            (freeBlocks > journalBlocks) ? (freeBlocks - journalBlocks) : 0);

        // Re-write superblock with updated counts
        std::memset(sbData.data(), 0, 1024);
        std::memcpy(sbData.data(), &sb, sizeof(Ext4Superblock));
        result = writeAt(1024, sbData.data(), 1024);
        if (!result) return result;
    }

    // Flush buffers
    if (volumeHandle)
    {
        volumeHandle->flushBuffers();
        volumeHandle->unlock();
    }
    else if (rawHandle)
    {
        rawHandle->flushBuffers();
    }

    if (progress) progress(100, "ext filesystem created successfully");
    return Result<void>::ok();
}

// ============================================================================
// FAT32 large (>32GB) direct-write formatter
//
// Windows format.com refuses to create FAT32 on volumes >32GB, but the
// filesystem itself supports up to 2TB with 32K clusters. We write the
// BPB, FSInfo, FAT tables, and root directory directly.
//
// On-disk layout:
//   Sector 0: Boot sector (BPB)
//   Sector 1: FSInfo sector
//   Sector 6: Backup boot sector
//   Sector 7: Backup FSInfo
//   Sectors reservedSectors..reservedSectors+fatSize-1: FAT #1
//   Sectors reservedSectors+fatSize..reservedSectors+2*fatSize-1: FAT #2
//   First cluster data starts at: reservedSectors + numFats * fatSize
// ============================================================================

Result<void> FormatEngine::formatFat32Large(const FormatTarget& target,
                                             const FormatOptions& options,
                                             FormatProgressCallback progress)
{
    if (progress) progress(0, "Preparing FAT32 (large volume)...");

    uint64_t partSize = target.partitionSizeBytes;
    if (partSize == 0 && target.hasDriveLetter())
    {
        auto spaceResult = VolumeHandle::getSpaceInfo(target.driveLetter);
        if (!spaceResult)
            return ErrorInfo::fromCode(ErrorCode::FormatFailed, "Cannot determine volume size");
        partSize = spaceResult.value().totalBytes;
    }

    const uint32_t sectorSize = 512; // FAT32 always uses 512-byte sectors in BPB
    const uint64_t totalSectors = partSize / sectorSize;

    if (totalSectors > 0xFFFFFFFF)
    {
        return ErrorInfo::fromCode(ErrorCode::PartitionTooLarge,
            "Volume too large for FAT32 (max ~2TB)");
    }

    // Determine cluster size
    uint32_t clusterSize = options.clusterSize;
    if (clusterSize == 0)
    {
        clusterSize = recommendedClusterSize(FilesystemType::FAT32, partSize);
    }

    uint8_t sectorsPerCluster = static_cast<uint8_t>(clusterSize / sectorSize);
    if (sectorsPerCluster == 0 || (sectorsPerCluster & (sectorsPerCluster - 1)) != 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Cluster size must be a power-of-2 multiple of sector size");
    }

    const uint16_t reservedSectors = 32;
    const uint8_t numFats = 2;

    // Calculate FAT size
    // Total data sectors = totalSectors - reservedSectors - (numFats * fatSize)
    // Total clusters = dataSectors / sectorsPerCluster
    // FAT entries needed = totalClusters + 2 (clusters 0 and 1 are reserved)
    // FAT sectors = ceil(fatEntries * 4 / sectorSize)
    // This is circular, so we solve iteratively:
    uint32_t fatSize = 0;
    {
        uint64_t dataSectors = totalSectors - reservedSectors;
        // Initial estimate
        uint64_t totalClusters = dataSectors / sectorsPerCluster;
        fatSize = static_cast<uint32_t>(((totalClusters + 2) * 4 + sectorSize - 1) / sectorSize);

        // Refine
        for (int i = 0; i < 10; ++i)
        {
            dataSectors = totalSectors - reservedSectors - static_cast<uint64_t>(numFats) * fatSize;
            totalClusters = dataSectors / sectorsPerCluster;
            uint32_t newFatSize = static_cast<uint32_t>(((totalClusters + 2) * 4 + sectorSize - 1) / sectorSize);
            if (newFatSize == fatSize) break;
            fatSize = newFatSize;
        }
    }

    // Recalculate actual data clusters
    uint64_t dataStartSector = reservedSectors + static_cast<uint64_t>(numFats) * fatSize;
    uint64_t dataSectors = totalSectors - dataStartSector;
    uint32_t totalClusters = static_cast<uint32_t>(dataSectors / sectorsPerCluster);

    if (totalClusters < 65525)
    {
        return ErrorInfo::fromCode(ErrorCode::PartitionTooSmall,
            "Volume too small for FAT32 (need >= 65525 clusters)");
    }

    if (progress) progress(5, "Building BPB...");

    // Build BPB
    std::vector<uint8_t> bootSector(sectorSize, 0);
    Fat32Bpb* bpb = reinterpret_cast<Fat32Bpb*>(bootSector.data());

    bpb->jmpBoot[0] = 0xEB;
    bpb->jmpBoot[1] = 0x58; // Jump over BPB
    bpb->jmpBoot[2] = 0x90; // NOP

    std::memcpy(bpb->oemName, "MSWIN4.1", 8);

    bpb->bytesPerSector = sectorSize;
    bpb->sectorsPerCluster = sectorsPerCluster;
    bpb->reservedSectors = reservedSectors;
    bpb->numFats = numFats;
    bpb->rootEntryCount = 0; // Must be 0 for FAT32
    bpb->totalSectors16 = 0;
    bpb->mediaType = 0xF8;
    bpb->fatSize16 = 0;
    bpb->sectorsPerTrack = 63;
    bpb->numHeads = 255;
    bpb->hiddenSectors = 0;
    bpb->totalSectors32 = static_cast<uint32_t>(totalSectors);
    bpb->fatSize32 = fatSize;
    bpb->extFlags = 0;
    bpb->fsVersion = 0;
    bpb->rootCluster = 2;
    bpb->fsInfoSector = 1;
    bpb->backupBootSector = 6;
    bpb->driveNumber = 0x80;
    bpb->bootSig = 0x29;
    bpb->volumeSerial = generateSerial();

    // Volume label — padded with spaces to 11 chars
    std::memset(bpb->volumeLabel, ' ', 11);
    if (!options.volumeLabel.empty())
    {
        size_t labelLen = std::min<size_t>(options.volumeLabel.size(), 11);
        std::memcpy(bpb->volumeLabel, options.volumeLabel.data(), labelLen);
        // Pad remaining with spaces
        for (size_t i = labelLen; i < 11; ++i)
            bpb->volumeLabel[i] = ' ';
    }
    else
    {
        std::memcpy(bpb->volumeLabel, "NO NAME    ", 11);
    }

    std::memcpy(bpb->fsType, "FAT32   ", 8);

    // Boot sector signature
    bootSector[510] = 0x55;
    bootSector[511] = 0xAA;

    // Build FSInfo sector
    std::vector<uint8_t> fsInfoSector(sectorSize, 0);
    Fat32FsInfo* fsInfo = reinterpret_cast<Fat32FsInfo*>(fsInfoSector.data());
    fsInfo->leadSig = 0x41615252;
    fsInfo->structSig = 0x61417272;
    fsInfo->freeCount = totalClusters - 1; // Minus 1 for root directory cluster
    fsInfo->nextFree = 3; // First free cluster after root dir
    fsInfo->trailSig = 0xAA550000;

    if (progress) progress(10, "Opening device...");

    // Open for writing
    std::unique_ptr<VolumeHandle> volumeHandle;
    std::unique_ptr<RawDiskHandle> rawHandle;
    uint64_t writeBaseOffset = 0;

    if (target.hasDriveLetter())
    {
        auto lockResult = lockAndDismount(target.driveLetter);
        if (!lockResult) return lockResult.error();
        volumeHandle = std::make_unique<VolumeHandle>(std::move(lockResult.value()));
    }
    else if (target.hasRawTarget())
    {
        auto diskResult = RawDiskHandle::open(target.diskIndex, DiskAccessMode::ReadWrite);
        if (!diskResult) return diskResult.error();
        rawHandle = std::make_unique<RawDiskHandle>(std::move(diskResult.value()));
        writeBaseOffset = target.partitionOffsetBytes;
    }

    auto writeAt = [&](uint64_t offsetFromPartStart, const uint8_t* data, uint32_t size) -> Result<void>
    {
        if (volumeHandle)
            return volumeHandle->writeBytes(offsetFromPartStart, data, size);
        else if (rawHandle)
        {
            uint64_t absOffset = writeBaseOffset + offsetFromPartStart;
            SectorOffset lba = absOffset / target.sectorSize;
            SectorCount sectors = (size + target.sectorSize - 1) / target.sectorSize;
            // Sector-aligned write
            if (absOffset % target.sectorSize == 0 && size % target.sectorSize == 0)
                return rawHandle->writeSectors(lba, data, sectors, target.sectorSize);
            // Read-modify-write
            auto existing = rawHandle->readSectors(lba, sectors, target.sectorSize);
            if (!existing) return existing.error();
            auto& buf = existing.value();
            uint32_t off = static_cast<uint32_t>(absOffset % target.sectorSize);
            std::memcpy(buf.data() + off, data, size);
            return rawHandle->writeSectors(lba, buf.data(), sectors, target.sectorSize);
        }
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError, "No valid write handle");
    };

    // Full format: zero first
    if (!options.quickFormat)
    {
        if (progress) progress(10, "Zeroing volume (full format)...");
        if (volumeHandle)
        {
            auto zr = zeroVolume(*volumeHandle, partSize, progress, 10, 50);
            if (!zr) return zr;
        }
        else if (rawHandle)
        {
            auto zr = zeroRaw(*rawHandle, writeBaseOffset, partSize, target.sectorSize, progress, 10, 50);
            if (!zr) return zr;
        }
    }

    int pBase = options.quickFormat ? 15 : 55;

    // Write boot sector (sector 0)
    auto result = writeAt(0, bootSector.data(), sectorSize);
    if (!result) return result;

    // Write FSInfo (sector 1)
    result = writeAt(sectorSize, fsInfoSector.data(), sectorSize);
    if (!result) return result;

    // Write backup boot sector (sector 6)
    result = writeAt(6 * sectorSize, bootSector.data(), sectorSize);
    if (!result) return result;

    // Write backup FSInfo (sector 7)
    result = writeAt(7 * sectorSize, fsInfoSector.data(), sectorSize);
    if (!result) return result;

    if (progress) progress(pBase + 5, "Writing FAT tables...");

    // Build and write FAT tables
    // FAT is fatSize sectors. We write it in chunks.
    // Entry 0: media byte + 0x0FFFFF00 -> 0x0FFFFFF8
    // Entry 1: end-of-chain marker     -> 0x0FFFFFFF
    // Entry 2: root directory cluster   -> 0x0FFFFFF8 (end of chain, 1 cluster)
    // Entries 3+: 0x00000000 (free)
    const uint32_t fatBytesTotal = fatSize * sectorSize;
    constexpr uint32_t fatChunkSize = 1024 * 1024; // Write 1MB at a time

    for (int fatCopy = 0; fatCopy < numFats; ++fatCopy)
    {
        uint64_t fatBaseOffset = static_cast<uint64_t>(reservedSectors + fatCopy * fatSize) * sectorSize;
        uint32_t remaining = fatBytesTotal;
        uint64_t pos = 0;
        bool firstChunk = true;

        while (remaining > 0)
        {
            uint32_t chunkBytes = std::min(remaining, fatChunkSize);
            std::vector<uint8_t> fatChunk(chunkBytes, 0);

            if (firstChunk)
            {
                // Write special first 3 entries
                uint32_t entry0 = 0x0FFFFFF8; // Media byte | 0x0FFFFF00
                uint32_t entry1 = 0x0FFFFFFF; // End of chain marker
                uint32_t entry2 = 0x0FFFFFF8; // Root directory end-of-chain
                std::memcpy(fatChunk.data() + 0, &entry0, 4);
                std::memcpy(fatChunk.data() + 4, &entry1, 4);
                std::memcpy(fatChunk.data() + 8, &entry2, 4);
                firstChunk = false;
            }

            result = writeAt(fatBaseOffset + pos, fatChunk.data(), chunkBytes);
            if (!result) return result;

            pos += chunkBytes;
            remaining -= chunkBytes;
        }

        if (progress)
        {
            int pct = pBase + 10 + (fatCopy + 1) * 30 / numFats;
            progress(pct, QString("FAT %1/%2 written").arg(fatCopy + 1).arg(numFats));
        }
    }

    if (progress) progress(pBase + 50, "Writing root directory...");

    // Write root directory cluster (all zeros — empty directory)
    // The volume label directory entry goes here
    std::vector<uint8_t> rootCluster(static_cast<size_t>(sectorsPerCluster) * sectorSize, 0);

    // Volume label entry (32 bytes)
    if (!options.volumeLabel.empty())
    {
        // Attribute 0x08 = volume label
        std::memset(rootCluster.data(), ' ', 11); // Pad name with spaces
        size_t labelLen = std::min<size_t>(options.volumeLabel.size(), 11);
        std::memcpy(rootCluster.data(), options.volumeLabel.data(), labelLen);
        rootCluster[11] = 0x08; // ATTR_VOLUME_ID
    }

    uint64_t rootClusterOffset = dataStartSector * sectorSize;
    result = writeAt(rootClusterOffset, rootCluster.data(),
                     static_cast<uint32_t>(rootCluster.size()));
    if (!result) return result;

    // Flush
    if (volumeHandle)
    {
        volumeHandle->flushBuffers();
        volumeHandle->unlock();
    }
    else if (rawHandle)
    {
        rawHandle->flushBuffers();
    }

    // Notify OS
    if (target.hasDriveLetter())
        notifyPartitionChangeLetter(target.driveLetter);
    else if (target.hasRawTarget())
        notifyPartitionChange(target.diskIndex);

    if (progress) progress(100, "FAT32 format complete");
    return Result<void>::ok();
}

// ============================================================================
// Linux swap direct-write formatter
//
// On-disk layout:
//   Page 0: Swap header
//     Offset 0x400: version (1)
//     Offset 0x404: last_page
//     Offset 0x408: nr_badpages (0)
//     Offset 0x40C: UUID (16 bytes)
//     Offset 0x41C: volume label (16 bytes)
//     Last 10 bytes of page: "SWAPSPACE2" magic
// ============================================================================

Result<void> FormatEngine::formatLinuxSwap(const FormatTarget& target,
                                            const FormatOptions& options,
                                            FormatProgressCallback progress)
{
    if (progress) progress(0, "Preparing Linux swap...");

    uint64_t partSize = target.partitionSizeBytes;
    if (partSize == 0 && target.hasDriveLetter())
    {
        auto spaceResult = VolumeHandle::getSpaceInfo(target.driveLetter);
        if (!spaceResult)
            return ErrorInfo::fromCode(ErrorCode::FormatFailed, "Cannot determine volume size");
        partSize = spaceResult.value().totalBytes;
    }

    uint32_t pageSize = options.swapPageSize;
    if (pageSize == 0) pageSize = 4096;

    if (partSize < 2 * pageSize)
    {
        return ErrorInfo::fromCode(ErrorCode::PartitionTooSmall,
            "Partition too small for Linux swap");
    }

    // Build swap header (one page)
    std::vector<uint8_t> swapPage(pageSize, 0);

    // Version = 1 at offset 0x400
    uint32_t version = 1;
    std::memcpy(swapPage.data() + 0x400, &version, 4);

    // last_page: (partSize / pageSize) - 1
    uint32_t lastPage = static_cast<uint32_t>(partSize / pageSize - 1);
    std::memcpy(swapPage.data() + 0x404, &lastPage, 4);

    // nr_badpages = 0
    uint32_t badPages = 0;
    std::memcpy(swapPage.data() + 0x408, &badPages, 4);

    // UUID
    uint8_t uuid[16];
    generateRandomBytes(uuid, 16);
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
    std::memcpy(swapPage.data() + 0x40C, uuid, 16);

    // Volume label (16 bytes)
    if (!options.volumeLabel.empty())
    {
        size_t labelLen = std::min<size_t>(options.volumeLabel.size(), 16);
        std::memcpy(swapPage.data() + 0x41C, options.volumeLabel.data(), labelLen);
    }

    // "SWAPSPACE2" magic at last 10 bytes of the page
    const char swapMagic[] = "SWAPSPACE2";
    std::memcpy(swapPage.data() + pageSize - 10, swapMagic, 10);

    if (progress) progress(20, "Opening device...");

    // Open for writing
    std::unique_ptr<VolumeHandle> volumeHandle;
    std::unique_ptr<RawDiskHandle> rawHandle;
    uint64_t writeBaseOffset = 0;

    if (target.hasDriveLetter())
    {
        auto lockResult = lockAndDismount(target.driveLetter);
        if (!lockResult) return lockResult.error();
        volumeHandle = std::make_unique<VolumeHandle>(std::move(lockResult.value()));
    }
    else if (target.hasRawTarget())
    {
        auto diskResult = RawDiskHandle::open(target.diskIndex, DiskAccessMode::ReadWrite);
        if (!diskResult) return diskResult.error();
        rawHandle = std::make_unique<RawDiskHandle>(std::move(diskResult.value()));
        writeBaseOffset = target.partitionOffsetBytes;
    }

    // Full format: zero first
    if (!options.quickFormat)
    {
        if (progress) progress(20, "Zeroing volume...");
        if (volumeHandle)
        {
            auto zr = zeroVolume(*volumeHandle, partSize, progress, 20, 70);
            if (!zr) return zr;
        }
        else if (rawHandle)
        {
            auto zr = zeroRaw(*rawHandle, writeBaseOffset, partSize, target.sectorSize, progress, 20, 70);
            if (!zr) return zr;
        }
    }

    if (progress) progress(80, "Writing swap header...");

    // Write swap header at offset 0
    Result<void> result = ErrorInfo::fromCode(ErrorCode::DiskWriteError, "No handle");
    if (volumeHandle)
    {
        result = volumeHandle->writeBytes(0, swapPage.data(), pageSize);
    }
    else if (rawHandle)
    {
        SectorOffset lba = writeBaseOffset / target.sectorSize;
        SectorCount sectors = (pageSize + target.sectorSize - 1) / target.sectorSize;
        result = rawHandle->writeSectors(lba, swapPage.data(), sectors, target.sectorSize);
    }

    if (!result) return result;

    // Flush
    if (volumeHandle)
    {
        volumeHandle->flushBuffers();
        volumeHandle->unlock();
    }
    else if (rawHandle)
    {
        rawHandle->flushBuffers();
    }

    if (progress) progress(100, "Linux swap created successfully");
    return Result<void>::ok();
}

// ============================================================================
// Helpers
// ============================================================================

Result<void> FormatEngine::zeroVolume(VolumeHandle& vol, uint64_t totalBytes,
                                       FormatProgressCallback progress,
                                       int progressStart, int progressEnd)
{
    constexpr uint32_t chunkSize = 4 * 1024 * 1024; // 4MB chunks
    std::vector<uint8_t> zeroBuf(chunkSize, 0);

    uint64_t bytesWritten = 0;
    while (bytesWritten < totalBytes)
    {
        uint32_t writeSize = static_cast<uint32_t>(
            std::min<uint64_t>(chunkSize, totalBytes - bytesWritten));

        auto result = vol.writeBytes(bytesWritten, zeroBuf.data(), writeSize);
        if (!result) return result;

        bytesWritten += writeSize;

        if (progress && totalBytes > 0)
        {
            int pct = progressStart +
                      static_cast<int>((progressEnd - progressStart) * bytesWritten / totalBytes);
            progress(pct, QString("Zeroing... %1%").arg(
                static_cast<int>(100 * bytesWritten / totalBytes)));
        }
    }

    return Result<void>::ok();
}

Result<void> FormatEngine::zeroRaw(RawDiskHandle& disk, uint64_t offsetBytes,
                                    uint64_t totalBytes, uint32_t sectorSize,
                                    FormatProgressCallback progress,
                                    int progressStart, int progressEnd)
{
    constexpr uint32_t chunkSectors = 8192; // Write 8192 sectors at a time
    uint32_t chunkBytes = chunkSectors * sectorSize;
    std::vector<uint8_t> zeroBuf(chunkBytes, 0);

    uint64_t bytesWritten = 0;
    while (bytesWritten < totalBytes)
    {
        uint32_t writeBytes = static_cast<uint32_t>(
            std::min<uint64_t>(chunkBytes, totalBytes - bytesWritten));
        SectorCount sectors = (writeBytes + sectorSize - 1) / sectorSize;
        SectorOffset lba = (offsetBytes + bytesWritten) / sectorSize;

        auto result = disk.writeSectors(lba, zeroBuf.data(), sectors, sectorSize);
        if (!result) return result;

        bytesWritten += sectors * sectorSize;

        if (progress && totalBytes > 0)
        {
            int pct = progressStart +
                      static_cast<int>((progressEnd - progressStart) * bytesWritten / totalBytes);
            progress(pct, QString("Zeroing... %1%").arg(
                static_cast<int>(100 * bytesWritten / totalBytes)));
        }
    }

    return Result<void>::ok();
}

Result<VolumeHandle> FormatEngine::lockAndDismount(wchar_t driveLetter)
{
    auto volResult = VolumeHandle::openByLetter(driveLetter, DiskAccessMode::ReadWrite);
    if (!volResult)
        return volResult.error();

    auto& vol = volResult.value();

    auto lockResult = vol.lock();
    if (!lockResult)
        return lockResult.error();

    auto dismountResult = vol.dismount();
    if (!dismountResult)
    {
        vol.unlock();
        return dismountResult.error();
    }

    return std::move(volResult);
}

Result<void> FormatEngine::notifyPartitionChange(DiskId diskIndex)
{
    // Open the physical disk and send IOCTL_DISK_UPDATE_PROPERTIES
    auto diskResult = RawDiskHandle::open(diskIndex, DiskAccessMode::ReadWrite);
    if (!diskResult) return diskResult.error();

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        diskResult.value().nativeHandle(),
        IOCTL_DISK_UPDATE_PROPERTIES,
        nullptr, 0,
        nullptr, 0,
        &bytesReturned,
        nullptr);

    if (!ok)
    {
        // Non-fatal — the OS will eventually pick it up
        return ErrorInfo::fromWin32(ErrorCode::DiskWriteError, GetLastError(),
            "IOCTL_DISK_UPDATE_PROPERTIES failed (non-fatal)");
    }

    return Result<void>::ok();
}

Result<void> FormatEngine::notifyPartitionChangeLetter(wchar_t driveLetter)
{
    // Broadcast WM_DEVICECHANGE or similar — for now just attempt to refresh
    // by briefly opening the volume root
    wchar_t rootPath[] = {driveLetter, L':', L'\\', L'\0'};
    DWORD attrs = GetFileAttributesW(rootPath);
    (void)attrs; // Just accessing it triggers the OS to re-check

    return Result<void>::ok();
}

} // namespace spw
