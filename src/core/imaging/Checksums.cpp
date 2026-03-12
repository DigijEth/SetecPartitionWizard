#include "Checksums.h"

#include "../common/Constants.h"

#include <sstream>
#include <iomanip>
#include <memory>

// Link against bcrypt.lib for BCryptHashData, etc.
#pragma comment(lib, "bcrypt.lib")

namespace spw
{

// ---------------------------------------------------------------------------
// Hex string conversion helpers
// ---------------------------------------------------------------------------
std::string hashToHexString(const uint8_t* data, size_t length)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < length; ++i)
    {
        oss << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return oss.str();
}

std::string sha256ToHex(const SHA256Hash& hash)
{
    return hashToHexString(hash.data(), hash.size());
}

std::string md5ToHex(const MD5Hash& hash)
{
    return hashToHexString(hash.data(), hash.size());
}

// ---------------------------------------------------------------------------
// RAII wrapper for BCrypt algorithm and hash handles.
// BCrypt is the modern Windows hashing API — it's always available on
// Windows Vista+ and doesn't require the legacy CryptoAPI.
// ---------------------------------------------------------------------------
class BcryptHasher
{
public:
    ~BcryptHasher()
    {
        if (m_hashHandle)
            ::BCryptDestroyHash(m_hashHandle);
        if (m_algHandle)
            ::BCryptCloseAlgorithmProvider(m_algHandle, 0);
    }

    // algorithmId is e.g. BCRYPT_SHA256_ALGORITHM or BCRYPT_MD5_ALGORITHM
    Result<void> init(const wchar_t* algorithmId)
    {
        NTSTATUS status = ::BCryptOpenAlgorithmProvider(
            &m_algHandle, algorithmId, nullptr, 0);

        if (!BCRYPT_SUCCESS(status))
        {
            return ErrorInfo::fromCode(ErrorCode::Unknown,
                "BCryptOpenAlgorithmProvider failed");
        }

        // Query the hash object size so we can allocate the internal state buffer
        DWORD hashObjectSize = 0;
        DWORD cbData = 0;
        status = ::BCryptGetProperty(
            m_algHandle, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&hashObjectSize),
            sizeof(hashObjectSize), &cbData, 0);

        if (!BCRYPT_SUCCESS(status))
        {
            return ErrorInfo::fromCode(ErrorCode::Unknown,
                "BCryptGetProperty(OBJECT_LENGTH) failed");
        }

        // Query the hash output length
        status = ::BCryptGetProperty(
            m_algHandle, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&m_hashLength),
            sizeof(m_hashLength), &cbData, 0);

        if (!BCRYPT_SUCCESS(status))
        {
            return ErrorInfo::fromCode(ErrorCode::Unknown,
                "BCryptGetProperty(HASH_LENGTH) failed");
        }

        m_hashObject.resize(hashObjectSize);

        status = ::BCryptCreateHash(
            m_algHandle, &m_hashHandle,
            m_hashObject.data(), hashObjectSize,
            nullptr, 0, 0);

        if (!BCRYPT_SUCCESS(status))
        {
            return ErrorInfo::fromCode(ErrorCode::Unknown,
                "BCryptCreateHash failed");
        }

        return Result<void>::ok();
    }

    Result<void> update(const uint8_t* data, size_t length)
    {
        // BCryptHashData takes a non-const pointer but doesn't modify the data.
        // The const_cast is safe here.
        NTSTATUS status = ::BCryptHashData(
            m_hashHandle,
            const_cast<PUCHAR>(data),
            static_cast<ULONG>(length), 0);

        if (!BCRYPT_SUCCESS(status))
        {
            return ErrorInfo::fromCode(ErrorCode::Unknown, "BCryptHashData failed");
        }

        return Result<void>::ok();
    }

    Result<void> finish(uint8_t* outputHash, size_t outputLength)
    {
        if (outputLength < m_hashLength)
        {
            return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                "Output buffer too small for hash result");
        }

        NTSTATUS status = ::BCryptFinishHash(
            m_hashHandle, outputHash, m_hashLength, 0);

        if (!BCRYPT_SUCCESS(status))
        {
            return ErrorInfo::fromCode(ErrorCode::Unknown, "BCryptFinishHash failed");
        }

        return Result<void>::ok();
    }

    DWORD hashLength() const { return m_hashLength; }

private:
    BCRYPT_ALG_HANDLE m_algHandle = nullptr;
    BCRYPT_HASH_HANDLE m_hashHandle = nullptr;
    std::vector<uint8_t> m_hashObject;
    DWORD m_hashLength = 0;
};

// ---------------------------------------------------------------------------
// Internal: hash a file using BCrypt with a given algorithm
// ---------------------------------------------------------------------------
static Result<std::vector<uint8_t>> hashFileGeneric(
    const wchar_t* algorithmId,
    const std::wstring& filePath,
    HashProgressCallback progressCb)
{
    // Open file for sequential reading
    HANDLE hFile = ::CreateFileW(
        filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return ErrorInfo::fromWin32(ErrorCode::FileNotFound,
            ::GetLastError(), "Failed to open file for hashing");
    }

    // Get file size for progress reporting
    LARGE_INTEGER fileSize;
    if (!::GetFileSizeEx(hFile, &fileSize))
    {
        ::CloseHandle(hFile);
        return ErrorInfo::fromWin32(ErrorCode::ImageReadError,
            ::GetLastError(), "Failed to get file size");
    }

    BcryptHasher hasher;
    auto initResult = hasher.init(algorithmId);
    if (initResult.isError())
    {
        ::CloseHandle(hFile);
        return initResult.error();
    }

    // Read in 4 MiB chunks — same chunk size as our imaging pipeline
    constexpr DWORD kReadBufSize = IMAGE_CHUNK_SIZE;
    std::vector<uint8_t> readBuffer(kReadBufSize);
    uint64_t totalRead = 0;

    for (;;)
    {
        DWORD bytesRead = 0;
        BOOL ok = ::ReadFile(hFile, readBuffer.data(), kReadBufSize, &bytesRead, nullptr);
        if (!ok)
        {
            ::CloseHandle(hFile);
            return ErrorInfo::fromWin32(ErrorCode::ImageReadError,
                ::GetLastError(), "ReadFile failed during hashing");
        }

        if (bytesRead == 0)
            break; // EOF

        auto updateResult = hasher.update(readBuffer.data(), bytesRead);
        if (updateResult.isError())
        {
            ::CloseHandle(hFile);
            return updateResult.error();
        }

        totalRead += bytesRead;

        if (progressCb)
        {
            if (!progressCb(totalRead, static_cast<uint64_t>(fileSize.QuadPart)))
            {
                ::CloseHandle(hFile);
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                    "Hash operation canceled by user");
            }
        }
    }

    ::CloseHandle(hFile);

    std::vector<uint8_t> hash(hasher.hashLength());
    auto finishResult = hasher.finish(hash.data(), hash.size());
    if (finishResult.isError())
        return finishResult.error();

    return hash;
}

// ---------------------------------------------------------------------------
// Internal: hash a range of sectors from a raw disk
// ---------------------------------------------------------------------------
static Result<std::vector<uint8_t>> hashDiskRangeGeneric(
    const wchar_t* algorithmId,
    const RawDiskHandle& disk,
    SectorOffset startLba,
    SectorCount sectorCount,
    uint32_t sectorSize,
    HashProgressCallback progressCb)
{
    if (!disk.isValid())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Invalid disk handle");
    }
    if (sectorCount == 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Cannot hash zero sectors");
    }

    BcryptHasher hasher;
    auto initResult = hasher.init(algorithmId);
    if (initResult.isError())
        return initResult.error();

    const uint64_t totalBytes = sectorCount * sectorSize;

    // Read in chunks of IMAGE_CHUNK_SIZE bytes, rounded down to sector boundary
    const SectorCount sectorsPerChunk = IMAGE_CHUNK_SIZE / sectorSize;
    SectorOffset currentLba = startLba;
    SectorCount remaining = sectorCount;
    uint64_t bytesProcessed = 0;

    while (remaining > 0)
    {
        const SectorCount chunkSectors = (remaining > sectorsPerChunk)
            ? sectorsPerChunk : remaining;

        auto readResult = disk.readSectors(currentLba, chunkSectors, sectorSize);
        if (readResult.isError())
            return readResult.error();

        const auto& data = readResult.value();
        auto updateResult = hasher.update(data.data(), data.size());
        if (updateResult.isError())
            return updateResult.error();

        currentLba += chunkSectors;
        remaining -= chunkSectors;
        bytesProcessed += data.size();

        if (progressCb)
        {
            if (!progressCb(bytesProcessed, totalBytes))
            {
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                    "Hash operation canceled by user");
            }
        }
    }

    std::vector<uint8_t> hash(hasher.hashLength());
    auto finishResult = hasher.finish(hash.data(), hash.size());
    if (finishResult.isError())
        return finishResult.error();

    return hash;
}

// ---------------------------------------------------------------------------
// CRC32 lookup table — computed at compile time using the standard polynomial.
// Polynomial: 0xEDB88320 (reversed representation of ISO 3309 / V.42).
// ---------------------------------------------------------------------------
static constexpr uint32_t kCrc32Polynomial = 0xEDB88320u;

struct Crc32Table
{
    uint32_t entries[256] = {};

    constexpr Crc32Table()
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit)
            {
                if (crc & 1)
                    crc = (crc >> 1) ^ kCrc32Polynomial;
                else
                    crc >>= 1;
            }
            entries[i] = crc;
        }
    }
};

static constexpr Crc32Table kCrc32Table{};

namespace Checksums
{

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

Result<SHA256Hash> sha256Buffer(const uint8_t* data, size_t length)
{
    if (!data && length > 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Null data pointer with non-zero length");
    }

    BcryptHasher hasher;
    auto initResult = hasher.init(BCRYPT_SHA256_ALGORITHM);
    if (initResult.isError())
        return initResult.error();

    if (length > 0)
    {
        auto updateResult = hasher.update(data, length);
        if (updateResult.isError())
            return updateResult.error();
    }

    SHA256Hash hash = {};
    auto finishResult = hasher.finish(hash.data(), hash.size());
    if (finishResult.isError())
        return finishResult.error();

    return hash;
}

Result<SHA256Hash> sha256File(const std::wstring& filePath,
                              HashProgressCallback progressCb)
{
    auto result = hashFileGeneric(BCRYPT_SHA256_ALGORITHM, filePath, progressCb);
    if (result.isError())
        return result.error();

    SHA256Hash hash = {};
    const auto& vec = result.value();
    if (vec.size() >= 32)
        std::memcpy(hash.data(), vec.data(), 32);

    return hash;
}

Result<SHA256Hash> sha256DiskRange(const RawDiskHandle& disk,
                                   SectorOffset startLba,
                                   SectorCount sectorCount,
                                   uint32_t sectorSize,
                                   HashProgressCallback progressCb)
{
    auto result = hashDiskRangeGeneric(
        BCRYPT_SHA256_ALGORITHM, disk, startLba, sectorCount, sectorSize, progressCb);
    if (result.isError())
        return result.error();

    SHA256Hash hash = {};
    const auto& vec = result.value();
    if (vec.size() >= 32)
        std::memcpy(hash.data(), vec.data(), 32);

    return hash;
}

// ---------------------------------------------------------------------------
// MD5
// ---------------------------------------------------------------------------

Result<MD5Hash> md5Buffer(const uint8_t* data, size_t length)
{
    if (!data && length > 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Null data pointer with non-zero length");
    }

    BcryptHasher hasher;
    auto initResult = hasher.init(BCRYPT_MD5_ALGORITHM);
    if (initResult.isError())
        return initResult.error();

    if (length > 0)
    {
        auto updateResult = hasher.update(data, length);
        if (updateResult.isError())
            return updateResult.error();
    }

    MD5Hash hash = {};
    auto finishResult = hasher.finish(hash.data(), hash.size());
    if (finishResult.isError())
        return finishResult.error();

    return hash;
}

Result<MD5Hash> md5File(const std::wstring& filePath,
                        HashProgressCallback progressCb)
{
    auto result = hashFileGeneric(BCRYPT_MD5_ALGORITHM, filePath, progressCb);
    if (result.isError())
        return result.error();

    MD5Hash hash = {};
    const auto& vec = result.value();
    if (vec.size() >= 16)
        std::memcpy(hash.data(), vec.data(), 16);

    return hash;
}

Result<MD5Hash> md5DiskRange(const RawDiskHandle& disk,
                             SectorOffset startLba,
                             SectorCount sectorCount,
                             uint32_t sectorSize,
                             HashProgressCallback progressCb)
{
    auto result = hashDiskRangeGeneric(
        BCRYPT_MD5_ALGORITHM, disk, startLba, sectorCount, sectorSize, progressCb);
    if (result.isError())
        return result.error();

    MD5Hash hash = {};
    const auto& vec = result.value();
    if (vec.size() >= 16)
        std::memcpy(hash.data(), vec.data(), 16);

    return hash;
}

// ---------------------------------------------------------------------------
// CRC32
// ---------------------------------------------------------------------------

uint32_t crc32Update(uint32_t previousCrc, const uint8_t* data, size_t length)
{
    // Standard CRC32 algorithm: XOR-in, table lookup, XOR-out.
    // The initial value is the bitwise inverse of the previous CRC so that
    // the first call with previousCrc=0 starts with 0xFFFFFFFF as required.
    uint32_t crc = previousCrc ^ 0xFFFFFFFF;

    for (size_t i = 0; i < length; ++i)
    {
        const uint8_t tableIndex = static_cast<uint8_t>(crc ^ data[i]);
        crc = (crc >> 8) ^ kCrc32Table.entries[tableIndex];
    }

    return crc ^ 0xFFFFFFFF;
}

uint32_t crc32Buffer(const uint8_t* data, size_t length)
{
    return crc32Update(0, data, length);
}

Result<uint32_t> crc32File(const std::wstring& filePath,
                           HashProgressCallback progressCb)
{
    HANDLE hFile = ::CreateFileW(
        filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return ErrorInfo::fromWin32(ErrorCode::FileNotFound,
            ::GetLastError(), "Failed to open file for CRC32");
    }

    LARGE_INTEGER fileSize;
    if (!::GetFileSizeEx(hFile, &fileSize))
    {
        ::CloseHandle(hFile);
        return ErrorInfo::fromWin32(ErrorCode::ImageReadError,
            ::GetLastError(), "Failed to get file size");
    }

    constexpr DWORD kReadBufSize = IMAGE_CHUNK_SIZE;
    std::vector<uint8_t> readBuffer(kReadBufSize);
    uint32_t crc = 0;
    uint64_t totalRead = 0;

    for (;;)
    {
        DWORD bytesRead = 0;
        BOOL ok = ::ReadFile(hFile, readBuffer.data(), kReadBufSize, &bytesRead, nullptr);
        if (!ok)
        {
            ::CloseHandle(hFile);
            return ErrorInfo::fromWin32(ErrorCode::ImageReadError,
                ::GetLastError(), "ReadFile failed during CRC32");
        }

        if (bytesRead == 0)
            break;

        crc = crc32Update(crc, readBuffer.data(), bytesRead);
        totalRead += bytesRead;

        if (progressCb)
        {
            if (!progressCb(totalRead, static_cast<uint64_t>(fileSize.QuadPart)))
            {
                ::CloseHandle(hFile);
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                    "CRC32 operation canceled by user");
            }
        }
    }

    ::CloseHandle(hFile);
    return crc;
}

Result<uint32_t> crc32DiskRange(const RawDiskHandle& disk,
                                SectorOffset startLba,
                                SectorCount sectorCount,
                                uint32_t sectorSize,
                                HashProgressCallback progressCb)
{
    if (!disk.isValid())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Invalid disk handle");
    }
    if (sectorCount == 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Cannot CRC32 zero sectors");
    }

    const uint64_t totalBytes = sectorCount * sectorSize;
    const SectorCount sectorsPerChunk = IMAGE_CHUNK_SIZE / sectorSize;
    SectorOffset currentLba = startLba;
    SectorCount remaining = sectorCount;
    uint64_t bytesProcessed = 0;
    uint32_t crc = 0;

    while (remaining > 0)
    {
        const SectorCount chunkSectors = (remaining > sectorsPerChunk)
            ? sectorsPerChunk : remaining;

        auto readResult = disk.readSectors(currentLba, chunkSectors, sectorSize);
        if (readResult.isError())
            return readResult.error();

        const auto& data = readResult.value();
        crc = crc32Update(crc, data.data(), data.size());

        currentLba += chunkSectors;
        remaining -= chunkSectors;
        bytesProcessed += data.size();

        if (progressCb)
        {
            if (!progressCb(bytesProcessed, totalBytes))
            {
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                    "CRC32 operation canceled by user");
            }
        }
    }

    return crc;
}

} // namespace Checksums
} // namespace spw
