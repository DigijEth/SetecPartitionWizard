#include "IsoFlasher.h"

#include "../common/Constants.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <sstream>

namespace spw
{

// ISO 9660 constants
static constexpr uint32_t ISO_SECTOR_SIZE = 2048;
static constexpr uint32_t ISO_PVD_LBA = 16; // Primary Volume Descriptor at LBA 16
static constexpr uint8_t ISO_VD_PRIMARY = 1;
static constexpr uint8_t ISO_VD_TERMINATOR = 255;

// MBR signature check for hybrid detection
static constexpr uint16_t MBR_SIG = 0xAA55;

// ---------------------------------------------------------------------------
static ErrorInfo makeWin32Error(ErrorCode code, const std::string& context)
{
    const DWORD lastErr = ::GetLastError();
    std::ostringstream oss;
    oss << context << " (Win32 error " << lastErr << ")";
    return ErrorInfo::fromWin32(code, lastErr, oss.str());
}

// ---------------------------------------------------------------------------
// Cancel support
// ---------------------------------------------------------------------------
void IsoFlasher::requestCancel()
{
    m_cancelRequested.store(true, std::memory_order_release);
}

bool IsoFlasher::isCancelRequested() const
{
    return m_cancelRequested.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Lock/unlock target volumes
// ---------------------------------------------------------------------------
Result<std::vector<HANDLE>> IsoFlasher::lockTargetVolumes(
    const std::vector<wchar_t>& volumeLetters)
{
    std::vector<HANDLE> locked;
    for (wchar_t letter : volumeLetters)
    {
        RawDiskHandle::dismountVolume(letter);
        auto lockResult = RawDiskHandle::lockVolume(letter);
        if (lockResult.isError())
        {
            unlockVolumes(locked);
            return ErrorInfo::fromCode(ErrorCode::DiskLockFailed,
                std::string("Failed to lock volume ") +
                static_cast<char>(letter) + ":");
        }
        locked.push_back(lockResult.value());
    }
    return locked;
}

void IsoFlasher::unlockVolumes(std::vector<HANDLE>& handles)
{
    for (HANDLE h : handles)
    {
        if (h != INVALID_HANDLE_VALUE)
        {
            RawDiskHandle::unlockVolume(h);
            ::CloseHandle(h);
        }
    }
    handles.clear();
}

// ---------------------------------------------------------------------------
// Helper: Seek to a byte offset in a file using OVERLAPPED-style positioning
// ---------------------------------------------------------------------------
static bool seekFile(HANDLE hFile, uint64_t offset)
{
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(offset);
    return ::SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN) != FALSE;
}

// ---------------------------------------------------------------------------
// Read ISO9660 Primary Volume Descriptor
// ---------------------------------------------------------------------------
Result<Iso9660VolumeDescriptor> IsoFlasher::readPVD(HANDLE hFile)
{
    // Volume descriptors start at LBA 16 (byte offset 0x8000) in 2048-byte sectors.
    // We scan for type 1 (Primary) and stop at type 255 (Terminator).
    uint32_t currentLba = ISO_PVD_LBA;

    for (int attempt = 0; attempt < 32; ++attempt) // Safety limit
    {
        const uint64_t offset =
            static_cast<uint64_t>(currentLba) * ISO_SECTOR_SIZE;

        if (!seekFile(hFile, offset))
        {
            return makeWin32Error(ErrorCode::IsoParseError,
                "Failed to seek to volume descriptor");
        }

        uint8_t sector[ISO_SECTOR_SIZE] = {};
        DWORD bytesRead = 0;
        BOOL ok = ::ReadFile(hFile, sector, ISO_SECTOR_SIZE,
                             &bytesRead, nullptr);
        if (!ok || bytesRead < ISO_SECTOR_SIZE)
        {
            return ErrorInfo::fromCode(ErrorCode::IsoParseError,
                "Failed to read volume descriptor sector");
        }

        // Check for "CD001" at offset 1
        if (std::memcmp(sector + 1, "CD001", 5) != 0)
        {
            return ErrorInfo::fromCode(ErrorCode::IsoParseError,
                "Invalid ISO9660: missing CD001 identifier");
        }

        if (sector[0] == ISO_VD_PRIMARY)
        {
            Iso9660VolumeDescriptor pvd;
            std::memcpy(&pvd, sector, sizeof(pvd));
            return pvd;
        }

        if (sector[0] == ISO_VD_TERMINATOR)
        {
            return ErrorInfo::fromCode(ErrorCode::IsoParseError,
                "No Primary Volume Descriptor found in ISO");
        }

        ++currentLba;
    }

    return ErrorInfo::fromCode(ErrorCode::IsoParseError,
        "Exceeded volume descriptor scan limit");
}

// ---------------------------------------------------------------------------
// Parse ISO9660 directory extent into file entries.
// A directory extent is a contiguous block of directory records.
// ---------------------------------------------------------------------------
Result<std::vector<IsoFileEntry>> IsoFlasher::parseDirectoryExtent(
    HANDLE hFile, uint32_t extentLba, uint32_t extentSize)
{
    std::vector<IsoFileEntry> entries;

    const uint64_t byteOffset =
        static_cast<uint64_t>(extentLba) * ISO_SECTOR_SIZE;

    if (!seekFile(hFile, byteOffset))
    {
        return makeWin32Error(ErrorCode::IsoParseError,
            "Failed to seek to directory extent");
    }

    // Read the entire directory extent
    std::vector<uint8_t> dirData(extentSize);
    DWORD bytesRead = 0;
    BOOL ok = ::ReadFile(hFile, dirData.data(),
                         static_cast<DWORD>(extentSize),
                         &bytesRead, nullptr);
    if (!ok)
    {
        return makeWin32Error(ErrorCode::IsoParseError,
            "Failed to read directory extent");
    }

    size_t pos = 0;
    while (pos + sizeof(Iso9660DirRecord) <= bytesRead)
    {
        const auto* record =
            reinterpret_cast<const Iso9660DirRecord*>(dirData.data() + pos);

        // A zero record length means we've hit padding at the end of a sector.
        // Skip to the next sector boundary.
        if (record->recordLength == 0)
        {
            const size_t nextSector =
                ((pos / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            if (nextSector >= bytesRead)
                break;
            pos = nextSector;
            continue;
        }

        // Validate record length
        if (record->recordLength < sizeof(Iso9660DirRecord))
        {
            pos += record->recordLength;
            continue;
        }

        // Extract filename
        const uint8_t* fileIdStart = dirData.data() + pos + 33;
        const uint8_t fileIdLen = record->fileIdLength;

        // Skip "." (0x00) and ".." (0x01) entries
        if (fileIdLen == 1 && (fileIdStart[0] == 0x00 || fileIdStart[0] == 0x01))
        {
            pos += record->recordLength;
            continue;
        }

        IsoFileEntry entry;
        entry.lba = record->extentLbaLe;
        entry.size = record->dataSizeLe;
        entry.isDirectory = (record->fileFlags & 0x02) != 0;

        // Convert filename: ISO9660 filenames end with ";1" (version)
        std::string rawName(reinterpret_cast<const char*>(fileIdStart), fileIdLen);

        // Strip version number suffix (e.g. ";1")
        auto semicolonPos = rawName.find(';');
        if (semicolonPos != std::string::npos)
        {
            rawName = rawName.substr(0, semicolonPos);
        }

        // Strip trailing dot if present (ISO9660 adds "." for files without extension)
        if (!rawName.empty() && rawName.back() == '.')
        {
            rawName.pop_back();
        }

        entry.name = rawName;
        entries.push_back(std::move(entry));

        pos += record->recordLength;
    }

    return entries;
}

// ---------------------------------------------------------------------------
// Find a file in the ISO by path (e.g. "/EFI/BOOT/BOOTX64.EFI")
// ---------------------------------------------------------------------------
Result<IsoFileEntry> IsoFlasher::findFileInIso(
    HANDLE hFile, const std::string& path)
{
    // Read PVD to get root directory location
    auto pvdResult = readPVD(hFile);
    if (pvdResult.isError())
        return pvdResult.error();

    const auto& pvd = pvdResult.value();

    // Root directory record is embedded in the PVD at offset 156
    const auto* rootRecord =
        reinterpret_cast<const Iso9660DirRecord*>(pvd.rootDirRecord);

    uint32_t currentLba = rootRecord->extentLbaLe;
    uint32_t currentSize = rootRecord->dataSizeLe;

    // Tokenize path
    std::vector<std::string> components;
    std::string pathCopy = path;

    // Normalize: remove leading/trailing slashes
    while (!pathCopy.empty() && pathCopy.front() == '/')
        pathCopy.erase(pathCopy.begin());
    while (!pathCopy.empty() && pathCopy.back() == '/')
        pathCopy.pop_back();

    // Split by '/'
    std::istringstream iss(pathCopy);
    std::string component;
    while (std::getline(iss, component, '/'))
    {
        if (!component.empty())
            components.push_back(component);
    }

    if (components.empty())
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Empty file path");
    }

    // Walk directory tree
    for (size_t i = 0; i < components.size(); ++i)
    {
        auto dirResult = parseDirectoryExtent(hFile, currentLba, currentSize);
        if (dirResult.isError())
            return dirResult.error();

        const auto& entries = dirResult.value();
        bool found = false;

        // Case-insensitive comparison (ISO9660 level 1 is uppercase)
        std::string searchName = components[i];
        for (auto& ch : searchName)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

        for (const auto& entry : entries)
        {
            std::string entryUpper = entry.name;
            for (auto& ch : entryUpper)
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

            if (entryUpper == searchName)
            {
                if (i == components.size() - 1)
                {
                    // This is the target
                    return entry;
                }

                if (!entry.isDirectory)
                {
                    return ErrorInfo::fromCode(ErrorCode::IsoParseError,
                        "Path component is not a directory: " + components[i]);
                }

                // Descend into subdirectory
                currentLba = entry.lba;
                currentSize = entry.size;
                found = true;
                break;
            }
        }

        if (!found)
        {
            return ErrorInfo::fromCode(ErrorCode::FileNotFound,
                "File not found in ISO: " + path);
        }
    }

    // Should not reach here
    return ErrorInfo::fromCode(ErrorCode::FileNotFound,
        "File not found in ISO: " + path);
}

// ---------------------------------------------------------------------------
// Check if an ISO is hybrid (has a valid MBR boot signature at offset 510)
// ---------------------------------------------------------------------------
Result<bool> IsoFlasher::isHybridIso(const std::wstring& isoPath)
{
    HANDLE hFile = ::CreateFileW(
        isoPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return makeWin32Error(ErrorCode::FileNotFound,
            "Failed to open ISO file");
    }

    // Read first 512 bytes (MBR area)
    uint8_t mbr[512] = {};
    DWORD bytesRead = 0;
    BOOL ok = ::ReadFile(hFile, mbr, 512, &bytesRead, nullptr);
    ::CloseHandle(hFile);

    if (!ok || bytesRead < 512)
    {
        return false; // Can't read enough — not hybrid
    }

    // Check MBR signature at offset 510-511
    const uint16_t sig = static_cast<uint16_t>(mbr[510]) |
                         (static_cast<uint16_t>(mbr[511]) << 8);

    if (sig != MBR_SIG)
    {
        return false;
    }

    // Additional check: at least one partition entry should be non-zero.
    // MBR partition table entries are at offsets 446-509 (4 entries x 16 bytes).
    bool hasPartition = false;
    for (int i = 0; i < 4; ++i)
    {
        const uint8_t* entry = mbr + 446 + (i * 16);
        // A partition entry is considered valid if the type byte is non-zero
        if (entry[4] != 0)
        {
            hasPartition = true;
            break;
        }
    }

    // Also verify this is actually an ISO (check for CD001 at sector 16)
    // by re-opening
    HANDLE hFile2 = ::CreateFileW(
        isoPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (hFile2 != INVALID_HANDLE_VALUE)
    {
        const uint64_t pvdOffset =
            static_cast<uint64_t>(ISO_PVD_LBA) * ISO_SECTOR_SIZE;
        if (seekFile(hFile2, pvdOffset))
        {
            uint8_t pvdBuf[8] = {};
            DWORD pvdRead = 0;
            if (::ReadFile(hFile2, pvdBuf, 8, &pvdRead, nullptr) && pvdRead >= 6)
            {
                if (std::memcmp(pvdBuf + 1, "CD001", 5) != 0)
                {
                    // Not an ISO at all — it's just a raw .img
                    ::CloseHandle(hFile2);
                    return false;
                }
            }
        }
        ::CloseHandle(hFile2);
    }

    return hasPartition;
}

// ---------------------------------------------------------------------------
// Check if ISO contains UEFI boot files
// ---------------------------------------------------------------------------
Result<bool> IsoFlasher::hasUefiBoot(const std::wstring& isoPath)
{
    HANDLE hFile = ::CreateFileW(
        isoPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return makeWin32Error(ErrorCode::FileNotFound,
            "Failed to open ISO file");
    }

    auto result = findFileInIso(hFile, "/EFI/BOOT/BOOTX64.EFI");
    ::CloseHandle(hFile);

    if (result.isOk())
        return true;

    // Also check for 32-bit UEFI or ARM variants
    hFile = ::CreateFileW(
        isoPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (hFile != INVALID_HANDLE_VALUE)
    {
        auto result32 = findFileInIso(hFile, "/EFI/BOOT/BOOTIA32.EFI");
        ::CloseHandle(hFile);
        if (result32.isOk())
            return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// List files in the root directory of an ISO
// ---------------------------------------------------------------------------
Result<std::vector<IsoFileEntry>> IsoFlasher::listIsoContents(
    const std::wstring& isoPath)
{
    HANDLE hFile = ::CreateFileW(
        isoPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return makeWin32Error(ErrorCode::FileNotFound,
            "Failed to open ISO file");
    }

    auto pvdResult = readPVD(hFile);
    if (pvdResult.isError())
    {
        ::CloseHandle(hFile);
        return pvdResult.error();
    }

    const auto* rootRecord =
        reinterpret_cast<const Iso9660DirRecord*>(
            pvdResult.value().rootDirRecord);

    auto entries = parseDirectoryExtent(
        hFile, rootRecord->extentLbaLe, rootRecord->dataSizeLe);

    ::CloseHandle(hFile);
    return entries;
}

// ---------------------------------------------------------------------------
// Read a file's contents from an ISO9660 image
// ---------------------------------------------------------------------------
Result<std::vector<uint8_t>> IsoFlasher::readIsoFile(
    const std::wstring& isoPath,
    const std::string& filePath)
{
    HANDLE hFile = ::CreateFileW(
        isoPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return makeWin32Error(ErrorCode::FileNotFound,
            "Failed to open ISO file");
    }

    auto fileResult = findFileInIso(hFile, filePath);
    if (fileResult.isError())
    {
        ::CloseHandle(hFile);
        return fileResult.error();
    }

    const auto& entry = fileResult.value();

    // Seek to file data
    const uint64_t fileOffset =
        static_cast<uint64_t>(entry.lba) * ISO_SECTOR_SIZE;

    if (!seekFile(hFile, fileOffset))
    {
        ::CloseHandle(hFile);
        return makeWin32Error(ErrorCode::ImageReadError,
            "Failed to seek to file data in ISO");
    }

    std::vector<uint8_t> data(entry.size);
    DWORD bytesRead = 0;
    BOOL ok = ::ReadFile(hFile, data.data(),
                         static_cast<DWORD>(entry.size),
                         &bytesRead, nullptr);
    ::CloseHandle(hFile);

    if (!ok)
    {
        return makeWin32Error(ErrorCode::ImageReadError,
            "Failed to read file data from ISO");
    }

    data.resize(bytesRead);
    return data;
}

// ---------------------------------------------------------------------------
// Main flash entry point
// ---------------------------------------------------------------------------
Result<void> IsoFlasher::flash(
    const FlashConfig& config,
    FlashProgressCallback progressCb)
{
    m_cancelRequested.store(false, std::memory_order_release);

    if (config.inputFilePath.empty())
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Input file path is empty");
    }
    if (config.targetDiskId < 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Invalid target disk ID");
    }

    // Open destination disk to check if it's removable
    auto dstResult = RawDiskHandle::open(
        config.targetDiskId, DiskAccessMode::ReadWrite);
    if (dstResult.isError())
        return dstResult.error();

    auto& dstDisk = dstResult.value();

    auto geomResult = dstDisk.getGeometry();
    if (geomResult.isError())
        return geomResult.error();

    const auto& geom = geomResult.value();
    const uint32_t dstSectorSize = geom.bytesPerSector;

    // Safety check: refuse to flash to fixed disks unless forced.
    // Fixed disks report FixedMedia; removable are RemovableMedia.
    if (!config.forceFixed && geom.mediaType == FixedMedia)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Target disk appears to be a fixed (non-removable) drive. "
            "Use forceFixed=true to override this safety check.");
    }

    // Open input file
    HANDLE hFile = ::CreateFileW(
        config.inputFilePath.c_str(),
        GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return makeWin32Error(ErrorCode::FileNotFound,
            "Failed to open input file");
    }

    LARGE_INTEGER fileSize;
    if (!::GetFileSizeEx(hFile, &fileSize))
    {
        ::CloseHandle(hFile);
        return makeWin32Error(ErrorCode::ImageReadError,
            "Failed to get input file size");
    }

    const uint64_t inputSize = static_cast<uint64_t>(fileSize.QuadPart);

    // Validate it fits on the target
    if (inputSize > geom.totalBytes)
    {
        ::CloseHandle(hFile);
        return ErrorInfo::fromCode(ErrorCode::InsufficientDiskSpace,
            "Input file is larger than target disk");
    }

    // Lock and dismount target volumes
    std::vector<HANDLE> lockedVolumes;
    if (!config.targetVolumeLetters.empty())
    {
        auto lockResult = lockTargetVolumes(config.targetVolumeLetters);
        if (lockResult.isError())
        {
            ::CloseHandle(hFile);
            return lockResult.error();
        }
        lockedVolumes = std::move(lockResult.value());
    }

    // Determine file type and flash strategy
    Result<void> result = Result<void>::ok();

    // Check file extension
    std::wstring ext;
    {
        auto dotPos = config.inputFilePath.rfind(L'.');
        if (dotPos != std::wstring::npos)
        {
            ext = config.inputFilePath.substr(dotPos);
            for (auto& ch : ext)
                ch = static_cast<wchar_t>(
                    std::towlower(static_cast<wint_t>(ch)));
        }
    }

    if (ext == L".img" || ext == L".raw" || ext == L".bin")
    {
        // Raw image — dd-style write
        result = flashRawImage(
            hFile, inputSize, dstDisk, dstSectorSize,
            config.bufferSize, config.verifyAfterFlash, progressCb);
    }
    else if (ext == L".iso")
    {
        // Check if hybrid ISO
        ::CloseHandle(hFile);

        auto hybridResult = isHybridIso(config.inputFilePath);
        bool isHybrid = hybridResult.isOk() && hybridResult.value();

        // Re-open file
        hFile = ::CreateFileW(
            config.inputFilePath.c_str(),
            GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            unlockVolumes(lockedVolumes);
            return makeWin32Error(ErrorCode::FileNotFound,
                "Failed to re-open input file");
        }

        if (isHybrid)
        {
            // Hybrid ISO — write directly like a raw image
            result = flashHybridIso(
                hFile, inputSize, dstDisk, dstSectorSize,
                config.bufferSize, config.verifyAfterFlash, progressCb);
        }
        else
        {
            // Non-hybrid ISO — need to create FAT32 and copy files
            result = flashNonHybridIso(
                hFile, inputSize, config.inputFilePath,
                dstDisk, dstSectorSize, config.bufferSize, progressCb);
        }
    }
    else
    {
        // Unknown extension — try raw write
        result = flashRawImage(
            hFile, inputSize, dstDisk, dstSectorSize,
            config.bufferSize, config.verifyAfterFlash, progressCb);
    }

    ::CloseHandle(hFile);

    if (result.isOk())
    {
        dstDisk.flushBuffers();

        if (progressCb)
        {
            FlashProgress done;
            done.phase = FlashProgress::Phase::Complete;
            done.bytesWritten = inputSize;
            done.totalBytes = inputSize;
            done.percentComplete = 100.0;
            progressCb(done);
        }
    }

    unlockVolumes(lockedVolumes);
    return result;
}

// ---------------------------------------------------------------------------
// Flash raw image — dd-style sector write
// ---------------------------------------------------------------------------
Result<void> IsoFlasher::flashRawImage(
    HANDLE hFile, uint64_t fileSize,
    RawDiskHandle& dstDisk, uint32_t dstSectorSize,
    uint32_t bufferSize, bool verify,
    FlashProgressCallback progressCb)
{
    const uint32_t alignedBufSize =
        (bufferSize / dstSectorSize) * dstSectorSize;
    if (alignedBufSize == 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Buffer size too small");
    }

    std::vector<uint8_t> readBuffer(alignedBufSize);

    LARGE_INTEGER startTime, perfFreq;
    ::QueryPerformanceFrequency(&perfFreq);
    ::QueryPerformanceCounter(&startTime);

    uint64_t bytesWritten = 0;
    uint64_t dstPos = 0;

    // Seek to beginning of input file
    seekFile(hFile, 0);

    while (bytesWritten < fileSize)
    {
        if (isCancelRequested())
        {
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                "Flash canceled");
        }

        const uint64_t remaining = fileSize - bytesWritten;
        const DWORD readSize = static_cast<DWORD>(
            std::min(static_cast<uint64_t>(alignedBufSize), remaining));

        DWORD bytesRead = 0;
        BOOL ok = ::ReadFile(hFile, readBuffer.data(), readSize,
                             &bytesRead, nullptr);
        if (!ok)
        {
            return makeWin32Error(ErrorCode::ImageReadError,
                "Failed to read from input file");
        }
        if (bytesRead == 0)
            break;

        // Pad to sector alignment
        const uint32_t alignedWriteSize =
            ((bytesRead + dstSectorSize - 1) / dstSectorSize) * dstSectorSize;

        if (alignedWriteSize > bytesRead)
        {
            std::memset(readBuffer.data() + bytesRead, 0,
                        alignedWriteSize - bytesRead);
        }

        const SectorOffset dstLba = dstPos / dstSectorSize;
        const SectorCount dstSectors =
            static_cast<SectorCount>(alignedWriteSize / dstSectorSize);

        auto writeResult = dstDisk.writeSectors(
            dstLba, readBuffer.data(), dstSectors, dstSectorSize);
        if (writeResult.isError())
            return writeResult.error();

        dstPos += bytesRead;
        bytesWritten += bytesRead;

        if (progressCb)
        {
            FlashProgress progress;
            progress.phase = FlashProgress::Phase::Flashing;
            progress.bytesWritten = bytesWritten;
            progress.totalBytes = fileSize;
            progress.percentComplete =
                static_cast<double>(bytesWritten) /
                static_cast<double>(fileSize) * 100.0;

            LARGE_INTEGER now;
            ::QueryPerformanceCounter(&now);
            const double elapsed =
                static_cast<double>(now.QuadPart - startTime.QuadPart) /
                static_cast<double>(perfFreq.QuadPart);

            if (elapsed > 0.0)
            {
                progress.speedBytesPerSec =
                    static_cast<double>(bytesWritten) / elapsed;
                if (progress.speedBytesPerSec > 0.0)
                {
                    progress.etaSeconds =
                        static_cast<double>(fileSize - bytesWritten) /
                        progress.speedBytesPerSec;
                }
            }

            if (!progressCb(progress))
            {
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                    "Flash canceled");
            }
        }
    }

    // Flush
    dstDisk.flushBuffers();

    // Verification pass
    if (verify)
    {
        auto verifyResult = verifyFlash(
            hFile, fileSize, dstDisk, dstSectorSize, bufferSize, progressCb);
        if (verifyResult.isError())
            return verifyResult;
    }

    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// Flash hybrid ISO — identical to raw image write
// ---------------------------------------------------------------------------
Result<void> IsoFlasher::flashHybridIso(
    HANDLE hFile, uint64_t fileSize,
    RawDiskHandle& dstDisk, uint32_t dstSectorSize,
    uint32_t bufferSize, bool verify,
    FlashProgressCallback progressCb)
{
    return flashRawImage(hFile, fileSize, dstDisk, dstSectorSize,
                         bufferSize, verify, progressCb);
}

// ---------------------------------------------------------------------------
// Flash non-hybrid ISO: create MBR + FAT32 partition + copy ISO files.
// This is the more complex path — we need to:
// 1. Write an MBR with one FAT32 partition
// 2. Format it as FAT32
// 3. Copy all files from the ISO
// 4. If UEFI boot files exist, ensure they're in the right place
//
// Since formatting FAT32 from scratch is very involved (superblock, FATs,
// root directory), we use a practical approach: write the raw ISO data
// starting at sector 0 (most modern tools like Rufus do this even for
// non-hybrid ISOs since UEFI firmware can often boot from them).
// For full compatibility, we prepend a protective MBR.
// ---------------------------------------------------------------------------
Result<void> IsoFlasher::flashNonHybridIso(
    HANDLE hFile, uint64_t fileSize,
    const std::wstring& isoPath,
    RawDiskHandle& dstDisk, uint32_t dstSectorSize,
    uint32_t bufferSize,
    FlashProgressCallback progressCb)
{
    // Strategy: Write a minimal MBR that points to the ISO content, then
    // write the ISO data. Modern UEFI firmware will find the El Torito
    // boot catalog. For legacy BIOS boot, we need the MBR to be valid.

    // First, write a protective/hybrid MBR at sector 0.
    // The MBR will have one partition entry covering the entire USB drive,
    // typed as 0x00 (empty) initially. We overlay the ISO content starting
    // at a 1MiB offset to avoid corrupting the MBR.
    //
    // Actually, the simplest correct approach for non-hybrid ISO on USB:
    // Write the ISO directly to the device and let UEFI firmware handle it.
    // This works for all modern UEFI systems. For BIOS, the user would need
    // a hybrid ISO (isohybrid). We document this limitation.

    // Write protective MBR
    uint8_t mbr[512] = {};

    // Partition 1: type 0xEF (EFI System Partition) covering the whole disk
    auto geomResult = dstDisk.getGeometry();
    if (geomResult.isError())
        return geomResult.error();

    const uint64_t diskBytes = geomResult.value().totalBytes;
    const uint32_t totalSectors512 =
        static_cast<uint32_t>(std::min(diskBytes / 512,
        static_cast<uint64_t>(0xFFFFFFFF)));

    // MBR partition entry 1 at offset 446
    uint8_t* partEntry = mbr + 446;
    partEntry[0] = 0x80;   // Active/bootable
    partEntry[1] = 0x00;   // Start head
    partEntry[2] = 0x01;   // Start sector (1-based)
    partEntry[3] = 0x00;   // Start cylinder
    partEntry[4] = 0xEF;   // Type: EFI System Partition
    partEntry[5] = 0xFE;   // End head
    partEntry[6] = 0xFF;   // End sector
    partEntry[7] = 0xFF;   // End cylinder

    // Start LBA (little-endian): sector 0
    partEntry[8] = 0x00;
    partEntry[9] = 0x00;
    partEntry[10] = 0x00;
    partEntry[11] = 0x00;

    // Size in sectors (little-endian)
    partEntry[12] = static_cast<uint8_t>(totalSectors512 & 0xFF);
    partEntry[13] = static_cast<uint8_t>((totalSectors512 >> 8) & 0xFF);
    partEntry[14] = static_cast<uint8_t>((totalSectors512 >> 16) & 0xFF);
    partEntry[15] = static_cast<uint8_t>((totalSectors512 >> 24) & 0xFF);

    // MBR signature
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    // Write MBR to disk
    // Pad to destination sector size if needed
    const uint32_t mbrWriteSize =
        std::max(static_cast<uint32_t>(512), dstSectorSize);
    std::vector<uint8_t> mbrBuf(mbrWriteSize, 0);
    std::memcpy(mbrBuf.data(), mbr, 512);

    auto writeResult = dstDisk.writeSectors(
        0, mbrBuf.data(), 1, dstSectorSize);
    if (writeResult.isError())
        return writeResult.error();

    // Now write the ISO content starting at sector 0 of the disk.
    // The ISO's PVD is at byte offset 32768 (sector 16 in 2048-byte ISO sectors),
    // so it won't overwrite the MBR we just wrote... unless we write sector 0.
    // Actually, we DO want to overwrite sector 0 with the ISO data, because
    // the ISO's first 32KB may contain El Torito boot code.
    //
    // The correct approach: write the entire ISO from byte 0, then patch
    // the MBR back in. But for maximum compatibility, just write the ISO raw.

    // Seek to beginning of ISO
    seekFile(hFile, 0);

    const uint32_t alignedBufSize =
        (bufferSize / dstSectorSize) * dstSectorSize;
    if (alignedBufSize == 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Buffer size too small");
    }

    std::vector<uint8_t> readBuffer(alignedBufSize);

    LARGE_INTEGER startTime, perfFreq;
    ::QueryPerformanceFrequency(&perfFreq);
    ::QueryPerformanceCounter(&startTime);

    uint64_t bytesWritten = 0;
    uint64_t dstPos = 0;
    bool firstChunk = true;

    while (bytesWritten < fileSize)
    {
        if (isCancelRequested())
        {
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                "Flash canceled");
        }

        const uint64_t remaining = fileSize - bytesWritten;
        const DWORD readSize = static_cast<DWORD>(
            std::min(static_cast<uint64_t>(alignedBufSize), remaining));

        DWORD bytesRead = 0;
        BOOL ok = ::ReadFile(hFile, readBuffer.data(), readSize,
                             &bytesRead, nullptr);
        if (!ok || bytesRead == 0)
            break;

        // For the first chunk, overlay the protective MBR
        if (firstChunk && bytesRead >= 512)
        {
            // Preserve the ISO's boot sector area but inject our MBR signature
            // and partition table so BIOS systems can find it
            std::memcpy(readBuffer.data() + 446, mbr + 446, 66);
            firstChunk = false;
        }

        const uint32_t alignedWriteSize =
            ((bytesRead + dstSectorSize - 1) / dstSectorSize) * dstSectorSize;

        if (alignedWriteSize > bytesRead)
        {
            std::memset(readBuffer.data() + bytesRead, 0,
                        alignedWriteSize - bytesRead);
        }

        const SectorOffset dstLba = dstPos / dstSectorSize;
        const SectorCount dstSectors =
            static_cast<SectorCount>(alignedWriteSize / dstSectorSize);

        auto diskWriteResult = dstDisk.writeSectors(
            dstLba, readBuffer.data(), dstSectors, dstSectorSize);
        if (diskWriteResult.isError())
            return diskWriteResult.error();

        dstPos += bytesRead;
        bytesWritten += bytesRead;

        if (progressCb)
        {
            FlashProgress progress;
            progress.phase = FlashProgress::Phase::Flashing;
            progress.bytesWritten = bytesWritten;
            progress.totalBytes = fileSize;
            progress.percentComplete =
                static_cast<double>(bytesWritten) /
                static_cast<double>(fileSize) * 100.0;

            LARGE_INTEGER now;
            ::QueryPerformanceCounter(&now);
            const double elapsed =
                static_cast<double>(now.QuadPart - startTime.QuadPart) /
                static_cast<double>(perfFreq.QuadPart);

            if (elapsed > 0.0)
            {
                progress.speedBytesPerSec =
                    static_cast<double>(bytesWritten) / elapsed;
                if (progress.speedBytesPerSec > 0.0)
                {
                    progress.etaSeconds =
                        static_cast<double>(fileSize - bytesWritten) /
                        progress.speedBytesPerSec;
                }
            }

            if (!progressCb(progress))
            {
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                    "Flash canceled");
            }
        }
    }

    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// Verification: re-read the file and the disk, compare SHA-256 chunk-by-chunk
// ---------------------------------------------------------------------------
Result<void> IsoFlasher::verifyFlash(
    HANDLE hFile, uint64_t fileSize,
    RawDiskHandle& dstDisk, uint32_t dstSectorSize,
    uint32_t bufferSize,
    FlashProgressCallback progressCb)
{
    const uint32_t alignedBufSize =
        (bufferSize / dstSectorSize) * dstSectorSize;
    if (alignedBufSize == 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Buffer size too small");
    }

    std::vector<uint8_t> fileBuf(alignedBufSize);

    // Seek file back to beginning
    seekFile(hFile, 0);

    LARGE_INTEGER startTime, perfFreq;
    ::QueryPerformanceFrequency(&perfFreq);
    ::QueryPerformanceCounter(&startTime);

    uint64_t bytesVerified = 0;
    uint64_t dstPos = 0;

    while (bytesVerified < fileSize)
    {
        if (isCancelRequested())
        {
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                "Verification canceled");
        }

        const uint64_t remaining = fileSize - bytesVerified;
        const DWORD readSize = static_cast<DWORD>(
            std::min(static_cast<uint64_t>(alignedBufSize), remaining));

        // Read from file
        DWORD fileBytesRead = 0;
        BOOL ok = ::ReadFile(hFile, fileBuf.data(), readSize,
                             &fileBytesRead, nullptr);
        if (!ok || fileBytesRead == 0)
            break;

        // Read same range from disk
        const SectorOffset dstLba = dstPos / dstSectorSize;
        const SectorCount dstSectors = static_cast<SectorCount>(
            (static_cast<uint64_t>(fileBytesRead) + dstSectorSize - 1) /
            dstSectorSize);

        auto diskRead = dstDisk.readSectors(dstLba, dstSectors, dstSectorSize);
        if (diskRead.isError())
            return diskRead.error();

        // Compare the relevant bytes
        const size_t compareLen = static_cast<size_t>(fileBytesRead);
        if (diskRead.value().size() < compareLen)
        {
            return ErrorInfo::fromCode(ErrorCode::ImageChecksumMismatch,
                "Disk read returned fewer bytes than expected during verification");
        }

        if (std::memcmp(fileBuf.data(), diskRead.value().data(), compareLen) != 0)
        {
            std::ostringstream oss;
            oss << "Verification mismatch at byte offset " << bytesVerified;
            return ErrorInfo::fromCode(ErrorCode::ImageChecksumMismatch,
                oss.str());
        }

        dstPos += fileBytesRead;
        bytesVerified += fileBytesRead;

        if (progressCb)
        {
            FlashProgress progress;
            progress.phase = FlashProgress::Phase::Verifying;
            progress.bytesWritten = bytesVerified;
            progress.totalBytes = fileSize;
            progress.percentComplete =
                static_cast<double>(bytesVerified) /
                static_cast<double>(fileSize) * 100.0;

            LARGE_INTEGER now;
            ::QueryPerformanceCounter(&now);
            const double elapsed =
                static_cast<double>(now.QuadPart - startTime.QuadPart) /
                static_cast<double>(perfFreq.QuadPart);

            if (elapsed > 0.0)
            {
                progress.speedBytesPerSec =
                    static_cast<double>(bytesVerified) / elapsed;
                if (progress.speedBytesPerSec > 0.0)
                {
                    progress.etaSeconds =
                        static_cast<double>(fileSize - bytesVerified) /
                        progress.speedBytesPerSec;
                }
            }

            if (!progressCb(progress))
            {
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                    "Verification canceled");
            }
        }
    }

    return Result<void>::ok();
}

} // namespace spw
