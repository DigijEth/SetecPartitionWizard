#include "EncryptedVault.h"
#include "../common/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cstring>

// Link: bcrypt.lib, virtdisk.lib
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "virtdisk.lib")

namespace spw
{

// ============================================================
// VaultHeader serialization
// ============================================================

std::vector<uint8_t> VaultHeader::serialize() const
{
    // Produce a VAULT_HEADER_SIZE (512) byte buffer, zero-padded.
    std::vector<uint8_t> buf(VAULT_HEADER_SIZE, 0);
    size_t offset = 0;

    // Magic (9 bytes)
    std::memcpy(buf.data() + offset, VAULT_MAGIC, VAULT_MAGIC_LEN);
    offset += VAULT_MAGIC_LEN;

    // Version (1 byte)
    buf[offset++] = version;

    // Algorithm (1 byte)
    buf[offset++] = static_cast<uint8_t>(algorithm);

    // Flags (1 byte)
    buf[offset++] = flags;

    // PBKDF2 iterations (4 bytes, little-endian)
    std::memcpy(buf.data() + offset, &pbkdf2Iterations, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Salt (32 bytes)
    std::memcpy(buf.data() + offset, salt, VAULT_SALT_LEN);
    offset += VAULT_SALT_LEN;

    // IV (16 bytes)
    std::memcpy(buf.data() + offset, iv, VAULT_IV_LEN);
    offset += VAULT_IV_LEN;

    // Volume size (8 bytes, little-endian)
    std::memcpy(buf.data() + offset, &volumeSize, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Data offset (8 bytes, little-endian)
    std::memcpy(buf.data() + offset, &dataOffset, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // HMAC (32 bytes) — filled in after the rest of the header is finalized
    std::memcpy(buf.data() + offset, hmac, VAULT_HMAC_LEN);
    // offset += VAULT_HMAC_LEN;

    return buf;
}

Result<VaultHeader> VaultHeader::deserialize(const uint8_t* data, size_t len)
{
    if (!data || len < VAULT_HEADER_SIZE)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Buffer too small for vault header");
    }

    VaultHeader hdr;
    size_t offset = 0;

    // Magic
    std::memcpy(hdr.magic, data + offset, VAULT_MAGIC_LEN);
    offset += VAULT_MAGIC_LEN;

    if (std::memcmp(hdr.magic, VAULT_MAGIC, VAULT_MAGIC_LEN) != 0)
    {
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed, "Invalid vault magic — not a SPWVAULT file");
    }

    // Version
    hdr.version = data[offset++];
    if (hdr.version != VAULT_VERSION)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                   "Unsupported vault version " + std::to_string(hdr.version));
    }

    // Algorithm
    uint8_t algoId = data[offset++];
    if (algoId < 0x01 || algoId > 0x03)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                   "Unknown vault algorithm ID " + std::to_string(algoId));
    }
    hdr.algorithm = static_cast<VaultAlgorithm>(algoId);

    // Flags
    hdr.flags = data[offset++];

    // PBKDF2 iterations
    std::memcpy(&hdr.pbkdf2Iterations, data + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    if (hdr.pbkdf2Iterations < 10000)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                   "PBKDF2 iterations suspiciously low (" +
                                   std::to_string(hdr.pbkdf2Iterations) + ")");
    }

    // Salt
    std::memcpy(hdr.salt, data + offset, VAULT_SALT_LEN);
    offset += VAULT_SALT_LEN;

    // IV
    std::memcpy(hdr.iv, data + offset, VAULT_IV_LEN);
    offset += VAULT_IV_LEN;

    // Volume size
    std::memcpy(&hdr.volumeSize, data + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Data offset
    std::memcpy(&hdr.dataOffset, data + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // HMAC
    std::memcpy(hdr.hmac, data + offset, VAULT_HMAC_LEN);

    return hdr;
}

// ============================================================
// EncryptedVault — constructor / destructor / move
// ============================================================

EncryptedVault::EncryptedVault() = default;

EncryptedVault::~EncryptedVault()
{
    // Best-effort unmount on destruction
    unmountAll();
}

EncryptedVault::EncryptedVault(EncryptedVault&& other) noexcept
{
    std::lock_guard<std::mutex> lock(other.m_mutex);
    m_mounted = std::move(other.m_mounted);
}

EncryptedVault& EncryptedVault::operator=(EncryptedVault&& other) noexcept
{
    if (this != &other)
    {
        unmountAll();
        std::lock_guard<std::mutex> lockThis(m_mutex);
        std::lock_guard<std::mutex> lockOther(other.m_mutex);
        m_mounted = std::move(other.m_mounted);
    }
    return *this;
}

// ============================================================
// BCrypt helper: generate random bytes
// ============================================================

Result<void> EncryptedVault::generateRandom(uint8_t* out, size_t len) const
{
    NTSTATUS status = BCryptGenRandom(nullptr, out, static_cast<ULONG>(len),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::KeyGenerationFailed,
                                   "BCryptGenRandom failed: NTSTATUS 0x" +
                                   std::to_string(static_cast<unsigned long>(status)));
    }
    return Result<void>::ok();
}

// ============================================================
// BCrypt helper: PBKDF2-SHA256 key derivation
// ============================================================

Result<std::vector<uint8_t>> EncryptedVault::deriveKey(
    const QString& password,
    const uint8_t* salt,
    size_t saltLen,
    uint32_t iterations,
    size_t keyLen,
    const QString& keyFilePath) const
{
    // Open SHA-256 algorithm for PBKDF2
    BCRYPT_ALG_HANDLE hAlgo = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &hAlgo, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::KeyGenerationFailed,
                                   "BCryptOpenAlgorithmProvider SHA256 failed");
    }

    // Convert password to UTF-8 bytes for the key derivation input
    QByteArray passBytes = password.toUtf8();

    std::vector<uint8_t> derivedKey(keyLen, 0);

    status = BCryptDeriveKeyPBKDF2(
        hAlgo,
        reinterpret_cast<PUCHAR>(passBytes.data()),
        static_cast<ULONG>(passBytes.size()),
        const_cast<PUCHAR>(salt),
        static_cast<ULONG>(saltLen),
        iterations,
        derivedKey.data(),
        static_cast<ULONG>(keyLen),
        0);

    BCryptCloseAlgorithmProvider(hAlgo, 0);

    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::KeyGenerationFailed,
                                   "BCryptDeriveKeyPBKDF2 failed");
    }

    // If a key file is provided, XOR its SHA-256 hash into the derived key
    if (!keyFilePath.isEmpty())
    {
        auto keyFileHashResult = hashKeyFile(keyFilePath);
        if (keyFileHashResult.isError())
            return keyFileHashResult.error();

        const auto& kfHash = keyFileHashResult.value();
        for (size_t i = 0; i < keyLen && i < kfHash.size(); ++i)
        {
            derivedKey[i] ^= kfHash[i % kfHash.size()];
        }
    }

    return derivedKey;
}

// ============================================================
// BCrypt helper: HMAC-SHA256
// ============================================================

Result<std::vector<uint8_t>> EncryptedVault::computeHmac(
    const uint8_t* key, size_t keyLen,
    const uint8_t* data, size_t dataLen) const
{
    BCRYPT_ALG_HANDLE hAlgo = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &hAlgo, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "Failed to open HMAC-SHA256 provider");
    }

    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(
        hAlgo, &hHash,
        nullptr, 0,
        const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen),
        0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "Failed to create HMAC hash object");
    }

    status = BCryptHashData(hHash, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen), 0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "BCryptHashData failed for HMAC");
    }

    std::vector<uint8_t> hmac(VAULT_HMAC_LEN, 0);
    status = BCryptFinishHash(hHash, hmac.data(), static_cast<ULONG>(hmac.size()), 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlgo, 0);

    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "BCryptFinishHash failed for HMAC");
    }

    return hmac;
}

// ============================================================
// BCrypt helper: SHA-256 hash of key file
// ============================================================

Result<std::vector<uint8_t>> EncryptedVault::hashKeyFile(const QString& keyFilePath) const
{
    QFile file(keyFilePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        return ErrorInfo::fromCode(ErrorCode::FileNotFound,
                                   "Cannot open key file: " + keyFilePath.toStdString());
    }

    QByteArray fileData = file.readAll();
    file.close();

    if (fileData.isEmpty())
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Key file is empty");
    }

    // Hash with BCrypt SHA-256
    BCRYPT_ALG_HANDLE hAlgo = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &hAlgo, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "Failed to open SHA-256 provider for key file");
    }

    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlgo, &hHash, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "Failed to create SHA-256 hash for key file");
    }

    status = BCryptHashData(hHash,
                            reinterpret_cast<PUCHAR>(fileData.data()),
                            static_cast<ULONG>(fileData.size()), 0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "SHA-256 hash of key file failed");
    }

    std::vector<uint8_t> hash(32, 0);
    status = BCryptFinishHash(hHash, hash.data(), static_cast<ULONG>(hash.size()), 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlgo, 0);

    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "SHA-256 finish failed for key file");
    }

    return hash;
}

// ============================================================
// Encrypt / Decrypt buffer (CBC and GCM modes)
// ============================================================

Result<std::vector<uint8_t>> EncryptedVault::encryptBuffer(
    const uint8_t* plaintext, size_t len,
    const uint8_t* key, size_t keyLen,
    const uint8_t* iv,
    VaultAlgorithm algo) const
{
    if (algo == VaultAlgorithm::AES_256_XTS)
    {
        // XTS is handled per-sector via encryptSectorXts
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                   "Use encryptSectorXts for XTS mode");
    }

    BCRYPT_ALG_HANDLE hAlgo = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &hAlgo, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "Failed to open AES provider");
    }

    // Set chaining mode
    const wchar_t* chainingMode = nullptr;
    if (algo == VaultAlgorithm::AES_256_CBC)
    {
        chainingMode = BCRYPT_CHAIN_MODE_CBC;
    }
    else if (algo == VaultAlgorithm::AES_256_GCM)
    {
        chainingMode = BCRYPT_CHAIN_MODE_GCM;
    }

    status = BCryptSetProperty(
        hAlgo, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(chainingMode)),
        static_cast<ULONG>((wcslen(chainingMode) + 1) * sizeof(wchar_t)),
        0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "Failed to set AES chaining mode");
    }

    // Import the key
    BCRYPT_KEY_HANDLE hKey = nullptr;
    status = BCryptGenerateSymmetricKey(
        hAlgo, &hKey, nullptr, 0,
        const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "Failed to import AES key");
    }

    // Make a mutable copy of the IV (BCrypt modifies it in place)
    std::vector<uint8_t> ivCopy(iv, iv + VAULT_IV_LEN);

    // For GCM, set up the auth info structure
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo = {};
    std::vector<uint8_t> gcmTag(16, 0);

    if (algo == VaultAlgorithm::AES_256_GCM)
    {
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = ivCopy.data();
        authInfo.cbNonce = static_cast<ULONG>(ivCopy.size());
        authInfo.pbTag   = gcmTag.data();
        authInfo.cbTag   = static_cast<ULONG>(gcmTag.size());
    }

    // Determine output size
    ULONG ciphertextLen = 0;
    ULONG flags = (algo == VaultAlgorithm::AES_256_CBC) ? BCRYPT_BLOCK_PADDING : 0;

    status = BCryptEncrypt(
        hKey,
        const_cast<PUCHAR>(plaintext), static_cast<ULONG>(len),
        (algo == VaultAlgorithm::AES_256_GCM) ? &authInfo : nullptr,
        ivCopy.data(), static_cast<ULONG>(ivCopy.size()),
        nullptr, 0, &ciphertextLen, flags);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "BCryptEncrypt size query failed");
    }

    // Reset IV copy for actual encryption
    std::memcpy(ivCopy.data(), iv, VAULT_IV_LEN);

    if (algo == VaultAlgorithm::AES_256_GCM)
    {
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = ivCopy.data();
        authInfo.cbNonce = static_cast<ULONG>(ivCopy.size());
        authInfo.pbTag   = gcmTag.data();
        authInfo.cbTag   = static_cast<ULONG>(gcmTag.size());
    }

    // For GCM, append the 16-byte auth tag at the end of the output
    size_t totalOutputLen = ciphertextLen;
    if (algo == VaultAlgorithm::AES_256_GCM)
        totalOutputLen += 16;

    std::vector<uint8_t> ciphertext(totalOutputLen, 0);

    status = BCryptEncrypt(
        hKey,
        const_cast<PUCHAR>(plaintext), static_cast<ULONG>(len),
        (algo == VaultAlgorithm::AES_256_GCM) ? &authInfo : nullptr,
        ivCopy.data(), static_cast<ULONG>(ivCopy.size()),
        ciphertext.data(), ciphertextLen, &ciphertextLen, flags);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlgo, 0);

    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "BCryptEncrypt failed");
    }

    // For GCM, append the tag
    if (algo == VaultAlgorithm::AES_256_GCM)
    {
        std::memcpy(ciphertext.data() + ciphertextLen, gcmTag.data(), 16);
        ciphertext.resize(ciphertextLen + 16);
    }
    else
    {
        ciphertext.resize(ciphertextLen);
    }

    return ciphertext;
}

Result<std::vector<uint8_t>> EncryptedVault::decryptBuffer(
    const uint8_t* ciphertext, size_t len,
    const uint8_t* key, size_t keyLen,
    const uint8_t* iv,
    VaultAlgorithm algo) const
{
    if (algo == VaultAlgorithm::AES_256_XTS)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                   "Use decryptSectorXts for XTS mode");
    }

    BCRYPT_ALG_HANDLE hAlgo = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &hAlgo, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "Failed to open AES provider for decryption");
    }

    const wchar_t* chainingMode = nullptr;
    if (algo == VaultAlgorithm::AES_256_CBC)
        chainingMode = BCRYPT_CHAIN_MODE_CBC;
    else if (algo == VaultAlgorithm::AES_256_GCM)
        chainingMode = BCRYPT_CHAIN_MODE_GCM;

    status = BCryptSetProperty(
        hAlgo, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(chainingMode)),
        static_cast<ULONG>((wcslen(chainingMode) + 1) * sizeof(wchar_t)),
        0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "Failed to set decryption chaining mode");
    }

    BCRYPT_KEY_HANDLE hKey = nullptr;
    status = BCryptGenerateSymmetricKey(
        hAlgo, &hKey, nullptr, 0,
        const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "Failed to import AES key for decryption");
    }

    std::vector<uint8_t> ivCopy(iv, iv + VAULT_IV_LEN);

    // For GCM, extract the last 16 bytes as the auth tag
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo = {};
    std::vector<uint8_t> gcmTag(16, 0);
    size_t cipherLen = len;

    if (algo == VaultAlgorithm::AES_256_GCM)
    {
        if (len < 16)
        {
            BCryptDestroyKey(hKey);
            BCryptCloseAlgorithmProvider(hAlgo, 0);
            return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                       "GCM ciphertext too short for auth tag");
        }
        cipherLen = len - 16;
        std::memcpy(gcmTag.data(), ciphertext + cipherLen, 16);

        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = ivCopy.data();
        authInfo.cbNonce = static_cast<ULONG>(ivCopy.size());
        authInfo.pbTag   = gcmTag.data();
        authInfo.cbTag   = static_cast<ULONG>(gcmTag.size());
    }

    ULONG flags = (algo == VaultAlgorithm::AES_256_CBC) ? BCRYPT_BLOCK_PADDING : 0;

    ULONG plaintextLen = 0;
    status = BCryptDecrypt(
        hKey,
        const_cast<PUCHAR>(ciphertext), static_cast<ULONG>(cipherLen),
        (algo == VaultAlgorithm::AES_256_GCM) ? &authInfo : nullptr,
        ivCopy.data(), static_cast<ULONG>(ivCopy.size()),
        nullptr, 0, &plaintextLen, flags);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "BCryptDecrypt size query failed");
    }

    // Reset IV copy
    std::memcpy(ivCopy.data(), iv, VAULT_IV_LEN);
    if (algo == VaultAlgorithm::AES_256_GCM)
    {
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = ivCopy.data();
        authInfo.cbNonce = static_cast<ULONG>(ivCopy.size());
        authInfo.pbTag   = gcmTag.data();
        authInfo.cbTag   = static_cast<ULONG>(gcmTag.size());
    }

    std::vector<uint8_t> plaintext(plaintextLen, 0);
    status = BCryptDecrypt(
        hKey,
        const_cast<PUCHAR>(ciphertext), static_cast<ULONG>(cipherLen),
        (algo == VaultAlgorithm::AES_256_GCM) ? &authInfo : nullptr,
        ivCopy.data(), static_cast<ULONG>(ivCopy.size()),
        plaintext.data(), plaintextLen, &plaintextLen, flags);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlgo, 0);

    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "BCryptDecrypt failed — wrong password or corrupted data");
    }

    plaintext.resize(plaintextLen);
    return plaintext;
}

// ============================================================
// XTS mode encrypt / decrypt per-sector
// ============================================================

Result<void> EncryptedVault::encryptSectorXts(
    uint8_t* buffer, size_t len,
    const uint8_t* key, uint64_t sectorNumber) const
{
    // AES-XTS uses a 512-bit key (two 256-bit keys: data key + tweak key).
    // BCrypt on Windows 10+ supports XTS natively.
    BCRYPT_ALG_HANDLE hAlgo = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &hAlgo, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "Failed to open AES provider for XTS");
    }

    // Set XTS chaining mode
    const wchar_t xtsMode[] = L"ChainingModeXTS";
    status = BCryptSetProperty(
        hAlgo, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(xtsMode)),
        static_cast<ULONG>(sizeof(xtsMode)),
        0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "XTS chaining mode not supported on this Windows version");
    }

    // Import the full 64-byte XTS key
    BCRYPT_KEY_HANDLE hKey = nullptr;
    status = BCryptGenerateSymmetricKey(
        hAlgo, &hKey, nullptr, 0,
        const_cast<PUCHAR>(key), static_cast<ULONG>(VAULT_XTS_KEY_LEN), 0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "Failed to import XTS key");
    }

    // The IV for XTS is the sector number as a 16-byte LE value
    uint8_t tweak[16] = {};
    std::memcpy(tweak, &sectorNumber, sizeof(uint64_t));

    ULONG resultLen = 0;
    status = BCryptEncrypt(
        hKey,
        buffer, static_cast<ULONG>(len),
        nullptr,
        tweak, sizeof(tweak),
        buffer, static_cast<ULONG>(len),
        &resultLen, 0);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlgo, 0);

    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "XTS sector encryption failed");
    }

    return Result<void>::ok();
}

Result<void> EncryptedVault::decryptSectorXts(
    uint8_t* buffer, size_t len,
    const uint8_t* key, uint64_t sectorNumber) const
{
    BCRYPT_ALG_HANDLE hAlgo = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &hAlgo, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "Failed to open AES provider for XTS decrypt");
    }

    const wchar_t xtsMode[] = L"ChainingModeXTS";
    status = BCryptSetProperty(
        hAlgo, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(xtsMode)),
        static_cast<ULONG>(sizeof(xtsMode)),
        0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "XTS mode not available for decryption");
    }

    BCRYPT_KEY_HANDLE hKey = nullptr;
    status = BCryptGenerateSymmetricKey(
        hAlgo, &hKey, nullptr, 0,
        const_cast<PUCHAR>(key), static_cast<ULONG>(VAULT_XTS_KEY_LEN), 0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "Failed to import XTS key for decryption");
    }

    uint8_t tweak[16] = {};
    std::memcpy(tweak, &sectorNumber, sizeof(uint64_t));

    ULONG resultLen = 0;
    status = BCryptDecrypt(
        hKey,
        buffer, static_cast<ULONG>(len),
        nullptr,
        tweak, sizeof(tweak),
        buffer, static_cast<ULONG>(len),
        &resultLen, 0);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlgo, 0);

    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "XTS sector decryption failed");
    }

    return Result<void>::ok();
}

// ============================================================
// Create a vault
// ============================================================

Result<void> EncryptedVault::create(
    const QString& vaultPath,
    uint64_t sizeBytes,
    const QString& password,
    VaultAlgorithm algorithm,
    uint32_t pbkdf2Iterations,
    const QString& keyFilePath,
    VaultProgressCallback progress)
{
    if (password.isEmpty())
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Password must not be empty");
    }

    if (sizeBytes < VAULT_SECTOR_SIZE * 2)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                   "Vault size too small (minimum 1024 bytes)");
    }

    // Round volume size up to sector boundary
    uint64_t volumeSize = (sizeBytes + VAULT_SECTOR_SIZE - 1) & ~(uint64_t)(VAULT_SECTOR_SIZE - 1);

    log::info("Creating encrypted vault: " + vaultPath);

    // Generate random salt and IV
    VaultHeader header;
    std::memcpy(header.magic, VAULT_MAGIC, VAULT_MAGIC_LEN);
    header.version          = VAULT_VERSION;
    header.algorithm        = algorithm;
    header.pbkdf2Iterations = pbkdf2Iterations;
    header.volumeSize       = volumeSize;
    header.dataOffset       = VAULT_HEADER_SIZE;

    auto randResult = generateRandom(header.salt, VAULT_SALT_LEN);
    if (randResult.isError()) return randResult.error();

    randResult = generateRandom(header.iv, VAULT_IV_LEN);
    if (randResult.isError()) return randResult.error();

    // Derive key material.
    // For XTS we need 64 bytes (two 256-bit keys); for others 32 bytes encryption + 32 bytes HMAC.
    size_t totalKeyLen = (algorithm == VaultAlgorithm::AES_256_XTS)
                         ? VAULT_XTS_KEY_LEN + VAULT_KEY_LEN   // 64 enc + 32 hmac
                         : VAULT_KEY_LEN + VAULT_KEY_LEN;       // 32 enc + 32 hmac

    auto keyResult = deriveKey(password, header.salt, VAULT_SALT_LEN,
                               pbkdf2Iterations, totalKeyLen, keyFilePath);
    if (keyResult.isError()) return keyResult.error();

    const auto& keyMaterial = keyResult.value();
    size_t encKeyLen = (algorithm == VaultAlgorithm::AES_256_XTS) ? VAULT_XTS_KEY_LEN : VAULT_KEY_LEN;
    const uint8_t* encKey  = keyMaterial.data();
    const uint8_t* hmacKey = keyMaterial.data() + encKeyLen;

    // Compute HMAC over header (with HMAC field zeroed)
    auto headerBytes = header.serialize(); // HMAC field is zeros at this point
    auto hmacResult = computeHmac(hmacKey, VAULT_KEY_LEN,
                                  headerBytes.data(),
                                  VAULT_HEADER_SIZE - VAULT_HMAC_LEN);
    if (hmacResult.isError()) return hmacResult.error();

    // Store HMAC into header and re-serialize
    std::memcpy(header.hmac, hmacResult.value().data(), VAULT_HMAC_LEN);
    headerBytes = header.serialize();

    // Create the vault file
    QFile vaultFile(vaultPath);
    if (!vaultFile.open(QIODevice::WriteOnly))
    {
        return ErrorInfo::fromCode(ErrorCode::FileCreateFailed,
                                   "Cannot create vault file: " + vaultPath.toStdString());
    }

    // Write header
    qint64 written = vaultFile.write(reinterpret_cast<const char*>(headerBytes.data()),
                                     static_cast<qint64>(headerBytes.size()));
    if (written != static_cast<qint64>(VAULT_HEADER_SIZE))
    {
        vaultFile.close();
        QFile::remove(vaultPath);
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError, "Failed to write vault header");
    }

    // Write encrypted zero-filled sectors
    uint64_t sectorsToWrite = volumeSize / VAULT_SECTOR_SIZE;
    std::vector<uint8_t> sectorBuf(VAULT_SECTOR_SIZE, 0);

    for (uint64_t sector = 0; sector < sectorsToWrite; ++sector)
    {
        // Zero the sector buffer each iteration (in case encryption is in-place)
        std::memset(sectorBuf.data(), 0, VAULT_SECTOR_SIZE);

        if (algorithm == VaultAlgorithm::AES_256_XTS)
        {
            auto encResult = encryptSectorXts(sectorBuf.data(), VAULT_SECTOR_SIZE,
                                              encKey, sector);
            if (encResult.isError())
            {
                vaultFile.close();
                QFile::remove(vaultPath);
                return encResult.error();
            }
        }
        else
        {
            // For CBC/GCM, encrypt sector-by-sector using IV derived from sector number
            uint8_t sectorIv[VAULT_IV_LEN];
            std::memcpy(sectorIv, header.iv, VAULT_IV_LEN);
            // Mix sector number into IV to give each sector a unique IV
            for (size_t i = 0; i < sizeof(uint64_t); ++i)
            {
                sectorIv[i] ^= static_cast<uint8_t>((sector >> (i * 8)) & 0xFF);
            }

            auto encResult = encryptBuffer(sectorBuf.data(), VAULT_SECTOR_SIZE,
                                           encKey, VAULT_KEY_LEN, sectorIv, algorithm);
            if (encResult.isError())
            {
                vaultFile.close();
                QFile::remove(vaultPath);
                return encResult.error();
            }

            sectorBuf = std::move(encResult.value());
        }

        written = vaultFile.write(reinterpret_cast<const char*>(sectorBuf.data()),
                                  static_cast<qint64>(sectorBuf.size()));
        if (written != static_cast<qint64>(sectorBuf.size()))
        {
            vaultFile.close();
            QFile::remove(vaultPath);
            return ErrorInfo::fromCode(ErrorCode::DiskWriteError,
                                       "Failed to write vault sector " + std::to_string(sector));
        }

        // Progress callback
        if (progress)
        {
            uint64_t bytesProcessed = (sector + 1) * VAULT_SECTOR_SIZE;
            if (!progress(bytesProcessed, volumeSize))
            {
                vaultFile.close();
                QFile::remove(vaultPath);
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                                           "Vault creation canceled by user");
            }
        }
    }

    vaultFile.flush();
    vaultFile.close();

    log::info("Vault created successfully: " + vaultPath);
    return Result<void>::ok();
}

// ============================================================
// Read and verify vault header
// ============================================================

Result<VaultHeader> EncryptedVault::readHeader(
    const QString& vaultPath,
    const QString& password,
    const QString& keyFilePath) const
{
    QFile file(vaultPath);
    if (!file.open(QIODevice::ReadOnly))
    {
        return ErrorInfo::fromCode(ErrorCode::FileNotFound,
                                   "Cannot open vault file: " + vaultPath.toStdString());
    }

    if (file.size() < static_cast<qint64>(VAULT_HEADER_SIZE))
    {
        file.close();
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "File too small to be a vault container");
    }

    QByteArray headerRaw = file.read(VAULT_HEADER_SIZE);
    file.close();

    if (headerRaw.size() != static_cast<int>(VAULT_HEADER_SIZE))
    {
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Failed to read vault header");
    }

    auto headerResult = VaultHeader::deserialize(
        reinterpret_cast<const uint8_t*>(headerRaw.constData()),
        static_cast<size_t>(headerRaw.size()));
    if (headerResult.isError())
        return headerResult.error();

    VaultHeader header = headerResult.value();

    // Derive key to verify HMAC
    size_t encKeyLen = (header.algorithm == VaultAlgorithm::AES_256_XTS)
                       ? VAULT_XTS_KEY_LEN : VAULT_KEY_LEN;
    size_t totalKeyLen = encKeyLen + VAULT_KEY_LEN;

    auto keyResult = deriveKey(password, header.salt, VAULT_SALT_LEN,
                               header.pbkdf2Iterations, totalKeyLen, keyFilePath);
    if (keyResult.isError()) return keyResult.error();

    const uint8_t* hmacKey = keyResult.value().data() + encKeyLen;

    // Compute HMAC over header bytes up to (but not including) the HMAC field.
    // The HMAC covers the first (VAULT_HEADER_SIZE - VAULT_HMAC_LEN) bytes,
    // but we need to zero the HMAC field in the serialized buffer to verify.
    auto serialized = header.serialize();
    // Zero the HMAC bytes in the buffer before recomputing
    size_t hmacFieldOffset = 0x50; // from the format spec
    std::memset(serialized.data() + hmacFieldOffset, 0, VAULT_HMAC_LEN);

    auto hmacResult = computeHmac(hmacKey, VAULT_KEY_LEN,
                                  serialized.data(),
                                  VAULT_HEADER_SIZE - VAULT_HMAC_LEN);
    if (hmacResult.isError()) return hmacResult.error();

    // Constant-time comparison of HMAC
    const auto& computedHmac = hmacResult.value();
    uint8_t diff = 0;
    for (size_t i = 0; i < VAULT_HMAC_LEN; ++i)
    {
        diff |= header.hmac[i] ^ computedHmac[i];
    }

    if (diff != 0)
    {
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "HMAC verification failed — wrong password or corrupted vault");
    }

    return header;
}

// ============================================================
// Create and attach a VHD from decrypted data
// ============================================================

Result<QString> EncryptedVault::createAndAttachVhd(
    const std::vector<uint8_t>& decryptedData,
    const QString& vaultPath, bool readOnly) const
{
    // Create a temporary VHD file
    QFileInfo vaultInfo(vaultPath);
    QString tempVhdPath = QDir::tempPath() + "/" +
                          "spw_vault_" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".vhd";

    // Write raw data as a fixed VHD.
    // A fixed VHD is the raw data followed by a 512-byte VHD footer.
    QFile vhdFile(tempVhdPath);
    if (!vhdFile.open(QIODevice::WriteOnly))
    {
        return ErrorInfo::fromCode(ErrorCode::FileCreateFailed,
                                   "Cannot create temporary VHD: " + tempVhdPath.toStdString());
    }

    // Write the decrypted raw disk data
    qint64 written = vhdFile.write(reinterpret_cast<const char*>(decryptedData.data()),
                                   static_cast<qint64>(decryptedData.size()));
    if (written != static_cast<qint64>(decryptedData.size()))
    {
        vhdFile.close();
        QFile::remove(tempVhdPath);
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError, "Failed to write VHD data");
    }

    // Write VHD fixed-disk footer (512 bytes)
    // The VHD spec requires a footer at the end with specific fields.
    uint8_t footer[512] = {};

    // Cookie: "conectix" (8 bytes)
    const char cookie[] = "conectix";
    std::memcpy(footer, cookie, 8);

    // Features: 0x00000002 (reserved, must be set)
    footer[8] = 0x00; footer[9] = 0x00; footer[10] = 0x00; footer[11] = 0x02;

    // File format version: 0x00010000 (1.0)
    footer[12] = 0x00; footer[13] = 0x01; footer[14] = 0x00; footer[15] = 0x00;

    // Data offset: 0xFFFFFFFFFFFFFFFF for fixed disks
    std::memset(footer + 16, 0xFF, 8);

    // Timestamp: seconds since Jan 1, 2000 12:00:00 — we use 0 for simplicity
    // Creator application: "spw " (4 bytes)
    footer[28] = 's'; footer[29] = 'p'; footer[30] = 'w'; footer[31] = ' ';
    // Creator version: 1.0
    footer[32] = 0x00; footer[33] = 0x01; footer[34] = 0x00; footer[35] = 0x00;
    // Creator host OS: Wi2k (Windows)
    footer[36] = 'W'; footer[37] = 'i'; footer[38] = '2'; footer[39] = 'k';

    // Original size (8 bytes, big-endian)
    uint64_t diskSize = decryptedData.size();
    for (int i = 0; i < 8; ++i)
        footer[40 + i] = static_cast<uint8_t>((diskSize >> (56 - i * 8)) & 0xFF);

    // Current size (same as original for fixed)
    std::memcpy(footer + 48, footer + 40, 8);

    // Disk geometry: CHS
    // Use standard CHS calculation
    uint64_t totalSectors = diskSize / 512;
    uint16_t cylinders = 0;
    uint8_t  heads = 0;
    uint8_t  sectorsPerTrack = 0;

    if (totalSectors > 65535 * 16 * 255)
    {
        totalSectors = 65535 * 16 * 255;
    }
    if (totalSectors >= 65535 * 16 * 63)
    {
        sectorsPerTrack = 255;
        heads = 16;
        cylinders = static_cast<uint16_t>(totalSectors / (heads * sectorsPerTrack));
    }
    else
    {
        sectorsPerTrack = 17;
        uint64_t cylindersTimesHeads = totalSectors / sectorsPerTrack;
        heads = static_cast<uint8_t>((cylindersTimesHeads + 1023) / 1024);
        if (heads < 4) heads = 4;
        if (cylindersTimesHeads >= (static_cast<uint64_t>(heads) * 1024) || heads > 16)
        {
            sectorsPerTrack = 31;
            heads = 16;
            cylindersTimesHeads = totalSectors / sectorsPerTrack;
        }
        if (cylindersTimesHeads >= (static_cast<uint64_t>(heads) * 1024))
        {
            sectorsPerTrack = 63;
            heads = 16;
            cylindersTimesHeads = totalSectors / sectorsPerTrack;
        }
        cylinders = static_cast<uint16_t>(cylindersTimesHeads / heads);
    }

    footer[56] = static_cast<uint8_t>((cylinders >> 8) & 0xFF);
    footer[57] = static_cast<uint8_t>(cylinders & 0xFF);
    footer[58] = heads;
    footer[59] = sectorsPerTrack;

    // Disk type: Fixed (0x00000002)
    footer[60] = 0x00; footer[61] = 0x00; footer[62] = 0x00; footer[63] = 0x02;

    // Unique ID (16 bytes) — generate random
    generateRandom(footer + 68, 16);

    // Checksum: one's complement of the sum of all bytes in the footer (excluding checksum)
    // Checksum is at offset 64, 4 bytes
    uint32_t checksum = 0;
    for (int i = 0; i < 512; ++i)
    {
        if (i >= 64 && i < 68) continue; // skip checksum field
        checksum += footer[i];
    }
    checksum = ~checksum;
    footer[64] = static_cast<uint8_t>((checksum >> 24) & 0xFF);
    footer[65] = static_cast<uint8_t>((checksum >> 16) & 0xFF);
    footer[66] = static_cast<uint8_t>((checksum >>  8) & 0xFF);
    footer[67] = static_cast<uint8_t>(checksum & 0xFF);

    written = vhdFile.write(reinterpret_cast<const char*>(footer), 512);
    vhdFile.flush();
    vhdFile.close();

    if (written != 512)
    {
        QFile::remove(tempVhdPath);
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError, "Failed to write VHD footer");
    }

    // Attach the VHD using the Virtual Disk API
    std::wstring vhdPathW = tempVhdPath.toStdWString();

    VIRTUAL_STORAGE_TYPE storageType = {};
    storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHD;
    storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    OPEN_VIRTUAL_DISK_PARAMETERS openParams = {};
    openParams.Version = OPEN_VIRTUAL_DISK_VERSION_1;

    HANDLE hVhd = INVALID_HANDLE_VALUE;
    DWORD openResult = OpenVirtualDisk(
        &storageType,
        vhdPathW.c_str(),
        VIRTUAL_DISK_ACCESS_ALL,
        OPEN_VIRTUAL_DISK_FLAG_NONE,
        &openParams,
        &hVhd);

    if (openResult != ERROR_SUCCESS)
    {
        QFile::remove(tempVhdPath);
        return ErrorInfo::fromWin32(ErrorCode::EncryptionFailed, openResult,
                                    "Failed to open virtual disk");
    }

    ATTACH_VIRTUAL_DISK_PARAMETERS attachParams = {};
    attachParams.Version = ATTACH_VIRTUAL_DISK_VERSION_1;

    DWORD attachFlags = ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME;
    if (readOnly)
        attachFlags |= ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY;

    DWORD attachResult = AttachVirtualDisk(
        hVhd, nullptr, static_cast<ATTACH_VIRTUAL_DISK_FLAG>(attachFlags), 0, &attachParams, nullptr);

    if (attachResult != ERROR_SUCCESS)
    {
        CloseHandle(hVhd);
        QFile::remove(tempVhdPath);
        return ErrorInfo::fromWin32(ErrorCode::EncryptionFailed, attachResult,
                                    "Failed to attach virtual disk");
    }

    // Get the physical path of the attached VHD to determine mount point
    wchar_t physicalPath[MAX_PATH] = {};
    ULONG physPathSize = MAX_PATH * sizeof(wchar_t);
    DWORD pathResult = GetVirtualDiskPhysicalPath(hVhd, &physPathSize, physicalPath);

    CloseHandle(hVhd);

    if (pathResult != ERROR_SUCCESS)
    {
        // Still attached, just cannot determine path
        return QString::fromStdWString(physicalPath);
    }

    QString mountPoint = QString::fromWCharArray(physicalPath);
    log::info("Vault mounted via VHD at: " + mountPoint);
    return mountPoint;
}

// ============================================================
// Detach a VHD
// ============================================================

Result<void> EncryptedVault::detachVhd(const QString& vhdPath) const
{
    std::wstring vhdPathW = vhdPath.toStdWString();

    VIRTUAL_STORAGE_TYPE storageType = {};
    storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHD;
    storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    OPEN_VIRTUAL_DISK_PARAMETERS openParams = {};
    openParams.Version = OPEN_VIRTUAL_DISK_VERSION_1;

    HANDLE hVhd = INVALID_HANDLE_VALUE;
    DWORD result = OpenVirtualDisk(
        &storageType,
        vhdPathW.c_str(),
        VIRTUAL_DISK_ACCESS_DETACH,
        OPEN_VIRTUAL_DISK_FLAG_NONE,
        &openParams,
        &hVhd);

    if (result != ERROR_SUCCESS)
    {
        return ErrorInfo::fromWin32(ErrorCode::EncryptionFailed, result,
                                    "Failed to open VHD for detaching");
    }

    result = DetachVirtualDisk(hVhd, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    CloseHandle(hVhd);

    if (result != ERROR_SUCCESS)
    {
        return ErrorInfo::fromWin32(ErrorCode::EncryptionFailed, result,
                                    "Failed to detach virtual disk");
    }

    // Delete the temporary VHD file
    QFile::remove(vhdPath);

    return Result<void>::ok();
}

// ============================================================
// Mount a vault
// ============================================================

Result<QString> EncryptedVault::mount(
    const QString& vaultPath,
    const QString& password,
    bool readOnly,
    const QString& keyFilePath,
    VaultProgressCallback progress)
{
    std::string vaultKey = vaultPath.toStdString();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_mounted.find(vaultKey) != m_mounted.end())
        {
            return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                       "Vault is already mounted: " + vaultKey);
        }
    }

    // Read and verify the header
    auto headerResult = readHeader(vaultPath, password, keyFilePath);
    if (headerResult.isError()) return headerResult.error();

    const VaultHeader& header = headerResult.value();

    // Derive encryption key
    size_t encKeyLen = (header.algorithm == VaultAlgorithm::AES_256_XTS)
                       ? VAULT_XTS_KEY_LEN : VAULT_KEY_LEN;
    size_t totalKeyLen = encKeyLen + VAULT_KEY_LEN;

    auto keyResult = deriveKey(password, header.salt, VAULT_SALT_LEN,
                               header.pbkdf2Iterations, totalKeyLen, keyFilePath);
    if (keyResult.isError()) return keyResult.error();

    const uint8_t* encKey = keyResult.value().data();

    // Read the encrypted data
    QFile vaultFile(vaultPath);
    if (!vaultFile.open(QIODevice::ReadOnly))
    {
        return ErrorInfo::fromCode(ErrorCode::FileNotFound,
                                   "Cannot open vault file for mounting");
    }

    vaultFile.seek(static_cast<qint64>(header.dataOffset));

    uint64_t dataSize = header.volumeSize;
    std::vector<uint8_t> decryptedData(static_cast<size_t>(dataSize), 0);

    // Read and decrypt sector by sector
    uint64_t sectorsTotal = dataSize / VAULT_SECTOR_SIZE;

    for (uint64_t sector = 0; sector < sectorsTotal; ++sector)
    {
        QByteArray sectorData = vaultFile.read(VAULT_SECTOR_SIZE);
        if (sectorData.size() != static_cast<int>(VAULT_SECTOR_SIZE))
        {
            vaultFile.close();
            return ErrorInfo::fromCode(ErrorCode::DiskReadError,
                                       "Failed to read vault sector " + std::to_string(sector));
        }

        size_t outOffset = static_cast<size_t>(sector * VAULT_SECTOR_SIZE);
        std::memcpy(decryptedData.data() + outOffset, sectorData.constData(), VAULT_SECTOR_SIZE);

        if (header.algorithm == VaultAlgorithm::AES_256_XTS)
        {
            auto decResult = decryptSectorXts(decryptedData.data() + outOffset,
                                              VAULT_SECTOR_SIZE, encKey, sector);
            if (decResult.isError())
            {
                vaultFile.close();
                return decResult.error();
            }
        }
        else
        {
            // Reconstruct per-sector IV
            uint8_t sectorIv[VAULT_IV_LEN];
            std::memcpy(sectorIv, header.iv, VAULT_IV_LEN);
            for (size_t i = 0; i < sizeof(uint64_t); ++i)
            {
                sectorIv[i] ^= static_cast<uint8_t>((sector >> (i * 8)) & 0xFF);
            }

            auto decResult = decryptBuffer(
                reinterpret_cast<const uint8_t*>(sectorData.constData()),
                VAULT_SECTOR_SIZE, encKey, VAULT_KEY_LEN, sectorIv, header.algorithm);
            if (decResult.isError())
            {
                vaultFile.close();
                return decResult.error();
            }

            const auto& decrypted = decResult.value();
            size_t copyLen = std::min(decrypted.size(), static_cast<size_t>(VAULT_SECTOR_SIZE));
            std::memcpy(decryptedData.data() + outOffset, decrypted.data(), copyLen);
        }

        if (progress)
        {
            uint64_t bytesProcessed = (sector + 1) * VAULT_SECTOR_SIZE;
            if (!progress(bytesProcessed, dataSize))
            {
                vaultFile.close();
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                                           "Vault mount canceled");
            }
        }
    }

    vaultFile.close();

    // Securely clear the key material from the key result vector
    // (keyResult.value() will go out of scope, but let's be explicit)
    SecureZeroMemory(const_cast<uint8_t*>(keyResult.value().data()),
                     keyResult.value().size());

    // Create a temp VHD and attach it
    auto vhdResult = createAndAttachVhd(decryptedData, vaultPath, readOnly);

    // Wipe decrypted data from memory
    SecureZeroMemory(decryptedData.data(), decryptedData.size());

    if (vhdResult.isError()) return vhdResult.error();

    QString mountPoint = vhdResult.value();

    // Record the mount
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        MountEntry entry;
        entry.info.vaultPath   = vaultPath;
        entry.info.mountPoint  = mountPoint;
        entry.info.algorithm   = header.algorithm;
        entry.info.volumeSize  = header.volumeSize;
        entry.info.readOnly    = readOnly;
        // The temp VHD path is stored so we can detach later
        // We derive it from the mount point or store it separately
        entry.tempVhdPath = QDir::tempPath() + "/" + QFileInfo(vaultPath).baseName() + ".vhd";
        m_mounted[vaultKey] = std::move(entry);
    }

    log::info("Vault mounted: " + vaultPath + " -> " + mountPoint);
    return mountPoint;
}

// ============================================================
// Unmount a vault
// ============================================================

Result<void> EncryptedVault::unmount(const QString& vaultPathOrMountPoint)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Search by vault path first, then by mount point
    std::string searchKey = vaultPathOrMountPoint.toStdString();
    auto it = m_mounted.find(searchKey);

    if (it == m_mounted.end())
    {
        // Search by mount point
        for (auto iter = m_mounted.begin(); iter != m_mounted.end(); ++iter)
        {
            if (iter->second.info.mountPoint == vaultPathOrMountPoint)
            {
                it = iter;
                break;
            }
        }
    }

    if (it == m_mounted.end())
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                   "No mounted vault found for: " +
                                   vaultPathOrMountPoint.toStdString());
    }

    auto detachResult = detachVhd(it->second.tempVhdPath);
    m_mounted.erase(it);

    if (detachResult.isError())
    {
        log::warn("Failed to cleanly detach VHD, entry removed from tracking");
        return detachResult.error();
    }

    log::info("Vault unmounted: " + vaultPathOrMountPoint);
    return Result<void>::ok();
}

Result<void> EncryptedVault::unmountAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    ErrorInfo lastError = ErrorInfo::ok();

    for (auto& [key, entry] : m_mounted)
    {
        auto result = detachVhd(entry.tempVhdPath);
        if (result.isError())
        {
            lastError = result.error();
            log::warn("Failed to detach VHD during unmountAll: " + entry.tempVhdPath);
        }
    }

    m_mounted.clear();

    if (lastError.isError())
        return lastError;

    return Result<void>::ok();
}

// ============================================================
// Change password
// ============================================================

Result<void> EncryptedVault::changePassword(
    const QString& vaultPath,
    const QString& currentPassword,
    const QString& newPassword,
    const QString& currentKeyFile,
    const QString& newKeyFile)
{
    if (newPassword.isEmpty())
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                   "New password must not be empty");
    }

    // Verify the current password by reading the header
    auto headerResult = readHeader(vaultPath, currentPassword, currentKeyFile);
    if (headerResult.isError()) return headerResult.error();

    VaultHeader header = headerResult.value();

    // Generate new salt (IV stays the same — it's per-sector for XTS, and data
    // is not re-encrypted, only the header key material changes)
    auto randResult = generateRandom(header.salt, VAULT_SALT_LEN);
    if (randResult.isError()) return randResult.error();

    // Derive new key material
    size_t encKeyLen = (header.algorithm == VaultAlgorithm::AES_256_XTS)
                       ? VAULT_XTS_KEY_LEN : VAULT_KEY_LEN;
    size_t totalKeyLen = encKeyLen + VAULT_KEY_LEN;

    // We need the OLD encryption key to re-encrypt the data, and the NEW key for the header.
    // However, since we're only changing the header HMAC (the data encryption key is
    // derived from the password), we actually need to re-encrypt all the data.
    // This is a full re-encryption operation.

    // Step 1: Derive old key to decrypt data
    auto oldKeyResult = deriveKey(currentPassword, headerResult.value().salt, VAULT_SALT_LEN,
                                  header.pbkdf2Iterations, totalKeyLen, currentKeyFile);
    if (oldKeyResult.isError()) return oldKeyResult.error();

    // Step 2: Derive new key
    auto newKeyResult = deriveKey(newPassword, header.salt, VAULT_SALT_LEN,
                                  header.pbkdf2Iterations, totalKeyLen, newKeyFile);
    if (newKeyResult.isError()) return newKeyResult.error();

    const uint8_t* oldEncKey = oldKeyResult.value().data();
    const uint8_t* newEncKey = newKeyResult.value().data();
    const uint8_t* newHmacKey = newKeyResult.value().data() + encKeyLen;

    // Read the vault data, decrypt with old key, re-encrypt with new key
    QFile vaultFile(vaultPath);
    if (!vaultFile.open(QIODevice::ReadWrite))
    {
        return ErrorInfo::fromCode(ErrorCode::DiskAccessDenied,
                                   "Cannot open vault for password change");
    }

    uint64_t sectorsTotal = header.volumeSize / VAULT_SECTOR_SIZE;

    for (uint64_t sector = 0; sector < sectorsTotal; ++sector)
    {
        vaultFile.seek(static_cast<qint64>(header.dataOffset + sector * VAULT_SECTOR_SIZE));
        QByteArray sectorData = vaultFile.read(VAULT_SECTOR_SIZE);

        if (sectorData.size() != static_cast<int>(VAULT_SECTOR_SIZE))
        {
            vaultFile.close();
            return ErrorInfo::fromCode(ErrorCode::DiskReadError,
                                       "Short read during password change at sector " +
                                       std::to_string(sector));
        }

        std::vector<uint8_t> sectorBuf(
            reinterpret_cast<const uint8_t*>(sectorData.constData()),
            reinterpret_cast<const uint8_t*>(sectorData.constData()) + VAULT_SECTOR_SIZE);

        if (header.algorithm == VaultAlgorithm::AES_256_XTS)
        {
            // Decrypt with old key
            auto r = decryptSectorXts(sectorBuf.data(), VAULT_SECTOR_SIZE, oldEncKey, sector);
            if (r.isError()) { vaultFile.close(); return r.error(); }

            // Re-encrypt with new key
            r = encryptSectorXts(sectorBuf.data(), VAULT_SECTOR_SIZE, newEncKey, sector);
            if (r.isError()) { vaultFile.close(); return r.error(); }
        }
        else
        {
            // Reconstruct per-sector IV for old key
            uint8_t sectorIv[VAULT_IV_LEN];
            std::memcpy(sectorIv, header.iv, VAULT_IV_LEN);
            for (size_t i = 0; i < sizeof(uint64_t); ++i)
                sectorIv[i] ^= static_cast<uint8_t>((sector >> (i * 8)) & 0xFF);

            auto decResult = decryptBuffer(sectorBuf.data(), VAULT_SECTOR_SIZE,
                                           oldEncKey, VAULT_KEY_LEN, sectorIv, header.algorithm);
            if (decResult.isError()) { vaultFile.close(); return decResult.error(); }

            // Re-encrypt with new key using same IV
            auto encResult = encryptBuffer(decResult.value().data(), decResult.value().size(),
                                           newEncKey, VAULT_KEY_LEN, sectorIv, header.algorithm);
            if (encResult.isError()) { vaultFile.close(); return encResult.error(); }

            sectorBuf = std::move(encResult.value());
        }

        // Write back
        vaultFile.seek(static_cast<qint64>(header.dataOffset + sector * VAULT_SECTOR_SIZE));
        qint64 written = vaultFile.write(reinterpret_cast<const char*>(sectorBuf.data()),
                                         static_cast<qint64>(sectorBuf.size()));
        if (written != static_cast<qint64>(sectorBuf.size()))
        {
            vaultFile.close();
            return ErrorInfo::fromCode(ErrorCode::DiskWriteError,
                                       "Write failed during password change");
        }
    }

    // Re-compute header HMAC with new key
    std::memset(header.hmac, 0, VAULT_HMAC_LEN);
    auto headerBytes = header.serialize();
    auto hmacResult = computeHmac(newHmacKey, VAULT_KEY_LEN,
                                  headerBytes.data(),
                                  VAULT_HEADER_SIZE - VAULT_HMAC_LEN);
    if (hmacResult.isError())
    {
        vaultFile.close();
        return hmacResult.error();
    }

    std::memcpy(header.hmac, hmacResult.value().data(), VAULT_HMAC_LEN);
    headerBytes = header.serialize();

    // Write new header
    vaultFile.seek(0);
    qint64 written = vaultFile.write(reinterpret_cast<const char*>(headerBytes.data()),
                                     static_cast<qint64>(VAULT_HEADER_SIZE));
    vaultFile.flush();
    vaultFile.close();

    // Securely clear key material
    SecureZeroMemory(const_cast<uint8_t*>(oldKeyResult.value().data()),
                     oldKeyResult.value().size());
    SecureZeroMemory(const_cast<uint8_t*>(newKeyResult.value().data()),
                     newKeyResult.value().size());

    if (written != static_cast<qint64>(VAULT_HEADER_SIZE))
    {
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError,
                                   "Failed to write updated vault header");
    }

    log::info("Vault password changed successfully: " + vaultPath);
    return Result<void>::ok();
}

// ============================================================
// List mounted vaults
// ============================================================

std::vector<MountedVaultInfo> EncryptedVault::listMountedVaults() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<MountedVaultInfo> result;
    result.reserve(m_mounted.size());

    for (const auto& [key, entry] : m_mounted)
    {
        result.push_back(entry.info);
    }

    return result;
}

} // namespace spw
