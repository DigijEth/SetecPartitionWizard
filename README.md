# Setec Partition Wizard For Windows

A comprehensive disk recovery, repair, flashing, and formatting tool for Windows.

## Features

- **Partition Management** — Create, delete, resize, move, merge, and split partitions
- **Formatting** — NTFS, FAT32/16/12, exFAT, ext2/3/4, Btrfs, XFS, HFS+, APFS (read), ReFS, and legacy filesystems (HPFS, Minix, AmigaFFS, BeOS BFS, and more)
- **Partition Tables** — Full MBR, GPT, and Apple Partition Map support
- **Recovery** — Deleted partition recovery, file carving, MBR/GPT repair
- **Imaging** — Disk/USB/SD card imaging, ISO flashing, disk cloning
- **Diagnostics** — S.M.A.R.T. monitoring, benchmarks, surface scan
- **Security Keys** — FIDO2/WebAuthn programming, encrypted vaults, boot authentication keys
- **Maintenance** — Secure erase (DoD 5220.22-M, Gutmann), boot repair

## Building

```bash
cmake --preset release
cmake --build --preset release
```

Requires:
- CMake 3.25+
- Qt 6
- MSVC (Visual Studio 2022+)
- Windows 10/11 x64

## License

Copyright (c) 2026 Setec

Don't forget to look UP UP at space,
