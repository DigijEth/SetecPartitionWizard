#pragma once

#include <cstdint>

namespace spw
{

// Sector sizes
constexpr uint32_t SECTOR_SIZE_512 = 512;
constexpr uint32_t SECTOR_SIZE_4K = 4096;
constexpr uint32_t DEFAULT_SECTOR_SIZE = SECTOR_SIZE_512;

// Partition alignment (1 MiB default — optimal for SSDs and 4K drives)
constexpr uint64_t DEFAULT_ALIGNMENT_BYTES = 1048576; // 1 MiB
constexpr uint64_t DEFAULT_ALIGNMENT_SECTORS_512 = DEFAULT_ALIGNMENT_BYTES / SECTOR_SIZE_512;

// MBR constants
constexpr uint16_t MBR_SIGNATURE = 0xAA55;
constexpr uint32_t MBR_SIZE = 512;
constexpr int MBR_MAX_PRIMARY_PARTITIONS = 4;
constexpr uint32_t MBR_PARTITION_ENTRY_OFFSET = 446;
constexpr uint8_t MBR_PARTITION_ENTRY_SIZE = 16;

// GPT constants
constexpr uint64_t GPT_HEADER_SIGNATURE = 0x5452415020494645ULL; // "EFI PART"
constexpr uint32_t GPT_HEADER_SIZE = 92;
constexpr uint32_t GPT_ENTRY_SIZE = 128;
constexpr int GPT_MAX_PARTITIONS = 128;
constexpr uint64_t GPT_HEADER_LBA = 1;

// Apple Partition Map constants
constexpr uint16_t APM_SIGNATURE = 0x504D;     // "PM"
constexpr uint16_t APM_DDM_SIGNATURE = 0x4552; // "ER"

// Filesystem magic bytes
constexpr uint32_t NTFS_MAGIC_OFFSET = 3;       // "NTFS    " at byte 3
constexpr uint16_t EXT_SUPER_MAGIC = 0xEF53;    // ext2/3/4 superblock magic at offset 1080
constexpr uint32_t BTRFS_MAGIC_OFFSET = 0x10040; // "_BHRfS_M" at 64K + 64
constexpr uint32_t XFS_MAGIC = 0x58465342;       // "XFSB"
constexpr uint16_t HFS_PLUS_MAGIC = 0x482B;      // "H+"
constexpr uint16_t HFSX_MAGIC = 0x4858;          // "HX"
constexpr uint32_t APFS_MAGIC = 0x4253584E;      // "NXSB" (little-endian)
constexpr uint16_t FAT_SIGNATURE = 0xAA55;
constexpr uint32_t REFS_MAGIC = 0x53465265;      // "ReFS"
constexpr uint32_t HPFS_SUPER_MAGIC = 0xF995E849;
constexpr uint16_t MINIX_SUPER_MAGIC = 0x137F;
constexpr uint16_t MINIX2_SUPER_MAGIC = 0x2468;
constexpr uint32_t UFS_MAGIC = 0x00011954;
constexpr uint32_t UDF_MAGIC_BEA = 0x00424541;   // BEA01
constexpr uint32_t ISO9660_MAGIC = 0x30304443;    // "CD001" identifier
constexpr uint32_t BEOS_SUPER_MAGIC = 0x42465331; // BFS1
constexpr uint16_t QNX4_SUPER_MAGIC = 0x002F;
constexpr uint32_t REISERFS_MAGIC_OFFSET = 0x10034;
constexpr uint32_t JFS_MAGIC = 0x3153464A;       // "JFS1"

// Imaging
constexpr uint32_t IMAGE_CHUNK_SIZE = 4 * 1024 * 1024; // 4 MiB chunks for compressed images
constexpr char SPW_IMAGE_MAGIC[] = "SPWIMG01";

// Benchmark
constexpr uint32_t BENCH_BLOCK_SEQ = 1024 * 1024;   // 1 MiB sequential
constexpr uint32_t BENCH_BLOCK_RND = 4096;            // 4 KiB random
constexpr int BENCH_DEFAULT_DURATION_SEC = 5;

// Secure erase
constexpr int ERASE_PASS_ZERO = 1;
constexpr int ERASE_PASS_DOD_3 = 3;   // DoD 5220.22-M (3-pass)
constexpr int ERASE_PASS_DOD_7 = 7;   // DoD 5220.22-M ECE (7-pass)
constexpr int ERASE_PASS_GUTMANN = 35; // Gutmann method

} // namespace spw
