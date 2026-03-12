# Setec Partition Wizard For Windows

**A free, open-source disk utility that does everything the paid tools do — without the monthly subscription.**

Tired of Acronis, EaseUS, and Partition Magic charging you $50/year for basic disk operations? So were we. Setec Partition Wizard is a comprehensive C++17/Qt6 disk utility covering partition management, formatting, recovery, imaging, diagnostics, security, maintenance, and SD card recovery — all in one tool, completely free.

[![VirusTotal Scan](https://i.ibb.co/TdDKXWb/virustotal.jpg)](https://ibb.co/GNfswHt)

---

## Features

### Partition Management
- **Create, delete, resize, move, merge, and split** partitions with a visual disk map
- Full **MBR, GPT, and Apple Partition Map** support
- GParted-style **operation queue** — preview all changes before applying them
- Partition alignment optimization for SSDs and 4K sector drives
- Raw partition table editing for advanced users

### Formatting — Every Filesystem You Can Think Of

**Modern Windows:**
NTFS, FAT32/16/12, exFAT, ReFS

**Linux / Open Source:**
ext2/3/4, Btrfs, XFS, ZFS, JFS, ReiserFS, Reiser4, F2FS, JFFS2, NILFS2, Linux swap

**Apple:**
HFS+, APFS (read-only detection), HFS Classic, MFS

**Legacy & Retro (because why not):**
HPFS (OS/2), Minix, Amiga Fast File System (AFFS/OFS), BeOS BFS, QNX4/6, UFS (BSD), FFS, Xenix, Coherent, SysV, VxFS, ADFS (Acorn), UDF, ISO9660, RomFS, CramFS, SquashFS, VFAT, UMSDOS

**Console & Gaming:**
FATX (Xbox / Xbox 360), STFS (Xbox 360 packages), GDFX (Xbox Game Disc), PS2 Memory Card

**Virtual Disk Images:**
VHD, VHDX (Hyper-V), VMDK (VMware), QCOW2 (QEMU), VDI (VirtualBox)

**Disc & Archive Images:**
RVZ/WIA (Dolphin Wii), WUA (Cemu Wii U), WBFS (Wii Backup), NRG (Nero), MDF (Alcohol 120%), CDI (DiscJuggler)

### SD Card Recovery
- **Detects SD/microSD cards Windows cannot see** — finds cards with corrupted partition tables, interrupted formats, and RAW/uninitialized media
- Scans by bus type, removable flag, and model keywords — even finds cards with no partition table at all
- **Full repair workflow:** clean partition table → reinitialize → format to FAT32/exFAT/NTFS
- Auto-selects exFAT for cards over 32 GB
- Uses raw IOCTL operations (`IOCTL_DISK_CREATE_DISK`, `IOCTL_DISK_SET_DRIVE_LAYOUT_EX`) to bypass Windows volume manager limitations

### Recovery
- **Deleted partition recovery** — scans for lost MBR/GPT partition entries
- **File carving** — recovers files by signature from MFT records, FAT directories, ext inodes, and raw byte-pattern carving
- **Boot repair** — rebuilds MBR boot code, fixes Windows BCD, restores GPT backup headers
- Supports recovery across MBR, GPT, and ext superblock structures

### Disk Imaging
- **Full disk imaging** — create and restore complete disk/partition images
- **ISO flashing** — write ISO/IMG files to USB drives and SD cards with hybrid ISO and UEFI boot detection
- **Disk cloning** — sector-by-sector or smart clone (skips unallocated space)
- **Checksum verification** — SHA-256, MD5, and CRC32 on all imaging operations

### S.M.A.R.T. & Diagnostics
- **S.M.A.R.T. monitoring** for both ATA and NVMe drives — read all attributes, thresholds, and health status
- **Disk benchmarks** — sequential and random read/write, queue depth 1 and 32
- **Surface scan** — find bad sectors before they eat your data

### Security
- **FIDO2/WebAuthn** — enumerate connected security keys, manage PINs, factory reset, create/verify credentials via the Windows WebAuthn API
- **Encrypted vaults** — create AES-256 encrypted virtual disk containers (XTS, CBC, and GCM modes) with PBKDF2 key derivation
- **Boot authentication keys** — create USB-based HMAC-SHA256 tokens for application access control
- All cryptography uses **Windows CNG (BCrypt)** — zero external dependencies, no OpenSSL

### Maintenance
- **Secure erase** — multiple wipe standards:
  - Single-pass zero fill
  - DoD 5220.22-M (3-pass and 7-pass variants)
  - Gutmann 35-pass method
  - Random fill and custom byte patterns
- **Boot repair** with MBR reconstruction and BCD rebuilding
- **SD Card Recovery** — repair cards that Windows refuses to recognize

---

## Screenshots

*Coming soon — the UI features a tabbed interface with visual disk maps, operation queues, and real-time progress tracking.*

---

## System Requirements

| Requirement | Details |
|-------------|---------|
| **OS** | Windows 10/11 x64 |
| **Privileges** | Administrator (required for raw disk access) |
| **Disk Space** | ~100 MB |
| **Runtime** | Qt 6 libraries (included in release builds) |

---

## Building From Source

```bash
# Debug build
cmake --preset default
cmake --build --preset default

# Release build
cmake --preset release
cmake --build --preset release
```

### Build Requirements
- CMake 3.25+
- Qt 6 (msvc2022_64 kit)
- MSVC (Visual Studio 2022+)
- Ninja (recommended, bundled with Qt)

See [docs/build.md](docs/build.md) for detailed build instructions, troubleshooting, and architecture overview.

---

## Architecture

```
src/
├── core/               # spw_core static library
│   ├── common/         # Types, Result<T>, Error, Constants, Logging
│   ├── disk/           # RawDiskHandle, VolumeHandle, DiskEnumerator,
│   │                   # SmartReader, PartitionTable, FilesystemDetector
│   ├── filesystem/     # FormatEngine (all filesystem formatters)
│   ├── operations/     # GParted-style operation queue
│   ├── recovery/       # Partition recovery, file carving, boot repair
│   ├── diagnostics/    # Benchmarks, surface scan
│   ├── imaging/        # Disk cloner, image creator/restorer, ISO flasher
│   ├── maintenance/    # Secure erase, SD card recovery
│   └── security/       # Encrypted vaults, FIDO2, boot auth
├── ui/                 # spw_ui static library (Qt Widgets)
│   ├── MainWindow      # Tab container with visual disk map
│   ├── tabs/           # One tab per feature area
│   └── widgets/        # DiskMapWidget (GParted-style visual partition map)
└── app/                # Executable entry point
```

### Design Decisions
- **No exceptions** — monadic `Result<T>` error handling throughout
- **No OpenSSL** — all crypto via Windows BCrypt (CNG API)
- **RAII everything** — disk handles auto-close on destruction
- **Operation queue** — changes are queued, previewed, then applied atomically
- **Removable-only safety** — ISO flasher refuses to write to fixed disks
- **Admin required** — raw disk I/O requires elevation; app checks and prompts
- **SD card recovery uses raw IOCTLs** — bypasses volume manager to reach cards Windows won't mount

---

## A Note for the Curious

> *"Don't forget to look UP UP at space."*

Press **F5** while the application is running. Something unexpected happens.

A riddle. A dark void. And a very particular file that ships with every build — one that looks like garbage in a hex editor, but says something in a text editor. Find it. Read it. Then find the file that only *your* build can produce.

Those who grew up cleaning floors on the SCS Deepship 86 might feel right at home. The janitor always did have a knack for finding hidden things.

*Roger Wilco was here.*

---

## Contributing

Contributions are welcome! This project exists because security and disk management tools should be free and accessible to everyone.

1. Fork the repository
2. Create a feature branch
3. Submit a pull request

Please note: the `third_party/hwdiag/` module is distributed as a pre-compiled library. Source files for that module are not included in the repository.

---

## License

Copyright (c) 2026 Setec

Don't forget to look UP UP at space,
