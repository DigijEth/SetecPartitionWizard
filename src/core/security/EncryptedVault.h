#pragma once

// EncryptedVault — Create, mount, unmount, and manage encrypted disk vault containers.
// Uses BCrypt API for AES-256-XTS, AES-256-CBC, and AES-256-GCM cipher modes.
// Key derivation via PBKDF2-SHA256 with configurable iterations (default 500,000).
// DISCLAIMER: This code is for authorized disk utility software only.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <bcrypt.h>
#include <virtdisk.h>

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"

#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace spw
{

// ------------------------------------------------------------------
// Vault file on-disk format (all little-endian):
//
//   Offset  Size   Field
//   0x00    9      Magic "SPWVAULT1"
//   0x09    1      Version (0x01)
//   0x0A    1      AlgorithmId (see VaultAlgorithm enum)
//   0x0B    1      Reserved / flags
//   0x0C    4      PBKDF2 iteration count (uint32_t)
//   0x10    32     Salt (random)
//   0x30    16     IV  (random, used by CBC/GCM; XTS derives tweak differently)
//   0x40    8      Encrypted volume size in bytes (uint64_t)
//   0x48    8      Data offset from file start (uint64_t, sector-aligned)
//   0x50    32     Header HMAC-SHA256 (keyed by a subkey derived from the password)
//   0x70    —      (padding to sector boundary = 512 bytes)
//   0x200   …      Encrypted sector data (512-byte aligned)
// ------------------------------------------------------------------

// Size constants
static constexpr size_t  VAULT_MAGIC_LEN         = 9;
static constexpr char    VAULT_MAGIC[]            = "SPWVAULT1";
static constexpr uint8_t VAULT_VERSION            = 0x01;
static constexpr size_t  VAULT_SALT_LEN           = 32;
static constexpr size_t  VAULT_IV_LEN             = 16;
static constexpr size_t  VAULT_HMAC_LEN           = 32;
static constexpr size_t  VAULT_HEADER_SIZE        = 512; // padded to one sector
static constexpr size_t  VAULT_KEY_LEN            = 32;  // 256 bits
static constexpr size_t  VAULT_XTS_KEY_LEN        = 64;  // 2 * 256 bits for XTS
static constexpr uint32_t VAULT_DEFAULT_ITERATIONS = 500000;
static constexpr size_t  VAULT_SECTOR_SIZE        = 512;

// Encryption algorithm identifiers stored in the vault header
enum class VaultAlgorithm : uint8_t
{
    AES_256_XTS = 0x01,  // Preferred — designed for disk encryption
    AES_256_CBC = 0x02,  // Fallback — widely supported
    AES_256_GCM = 0x03,  // Alternative to ChaCha20 when unavailable via BCrypt
};

// Packed on-disk header (do not rely on struct packing for I/O — serialize manually)
struct VaultHeader
{
    char     magic[VAULT_MAGIC_LEN] = {};
    uint8_t  version                = VAULT_VERSION;
    VaultAlgorithm algorithm        = VaultAlgorithm::AES_256_XTS;
    uint8_t  flags                  = 0;
    uint32_t pbkdf2Iterations       = VAULT_DEFAULT_ITERATIONS;
    uint8_t  salt[VAULT_SALT_LEN]   = {};
    uint8_t  iv[VAULT_IV_LEN]       = {};
    uint64_t volumeSize             = 0;
    uint64_t dataOffset             = VAULT_HEADER_SIZE;
    uint8_t  hmac[VAULT_HMAC_LEN]   = {};

    // Serialize header into a 512-byte buffer for writing to disk
    std::vector<uint8_t> serialize() const;

    // Deserialize from a 512-byte buffer
    static Result<VaultHeader> deserialize(const uint8_t* data, size_t len);
};

// Information about a currently mounted vault
struct MountedVaultInfo
{
    QString  vaultPath;            // Path to the .spwvault container file
    QString  mountPoint;           // Drive letter or mount path
    VaultAlgorithm algorithm;
    uint64_t volumeSize;
    bool     readOnly = false;
};

// Progress callback: (bytesProcessed, totalBytes) -> should continue?
using VaultProgressCallback = std::function<bool(uint64_t, uint64_t)>;

class EncryptedVault
{
public:
    EncryptedVault();
    ~EncryptedVault();

    // Non-copyable, movable
    EncryptedVault(const EncryptedVault&) = delete;
    EncryptedVault& operator=(const EncryptedVault&) = delete;
    EncryptedVault(EncryptedVault&&) noexcept;
    EncryptedVault& operator=(EncryptedVault&&) noexcept;

    // ---- Creation ----

    // Create a new vault container file at `vaultPath` with `sizeBytes` capacity.
    // The volume is zero-filled then encrypted.  `password` is the user passphrase;
    // `keyFilePath` is optional (empty string to skip).
    Result<void> create(const QString& vaultPath,
                        uint64_t sizeBytes,
                        const QString& password,
                        VaultAlgorithm algorithm = VaultAlgorithm::AES_256_XTS,
                        uint32_t pbkdf2Iterations = VAULT_DEFAULT_ITERATIONS,
                        const QString& keyFilePath = {},
                        VaultProgressCallback progress = nullptr);

    // ---- Mount / Unmount ----

    // Mount a vault: decrypt the header, verify HMAC, decrypt contents to a
    // temporary VHD, then attach via VHD API.  Returns the mount point.
    Result<QString> mount(const QString& vaultPath,
                          const QString& password,
                          bool readOnly = false,
                          const QString& keyFilePath = {},
                          VaultProgressCallback progress = nullptr);

    // Unmount a vault by its mount point or vault path.
    Result<void> unmount(const QString& vaultPathOrMountPoint);

    // Unmount every currently-mounted vault.
    Result<void> unmountAll();

    // ---- Management ----

    // Change the password of an existing vault container (re-encrypts the header).
    Result<void> changePassword(const QString& vaultPath,
                                const QString& currentPassword,
                                const QString& newPassword,
                                const QString& currentKeyFile = {},
                                const QString& newKeyFile = {});

    // List all currently mounted vaults.
    std::vector<MountedVaultInfo> listMountedVaults() const;

    // Check whether a vault file is valid (reads + verifies the header).
    Result<VaultHeader> readHeader(const QString& vaultPath,
                                   const QString& password,
                                   const QString& keyFilePath = {}) const;

private:
    // ---- BCrypt helpers ----

    // Derive encryption key + HMAC subkey from password (+optional keyfile)
    Result<std::vector<uint8_t>> deriveKey(const QString& password,
                                           const uint8_t* salt,
                                           size_t saltLen,
                                           uint32_t iterations,
                                           size_t keyLen,
                                           const QString& keyFilePath) const;

    // Compute HMAC-SHA256 of `data` under `key`
    Result<std::vector<uint8_t>> computeHmac(const uint8_t* key, size_t keyLen,
                                             const uint8_t* data, size_t dataLen) const;

    // Encrypt / decrypt a buffer using the specified algorithm
    Result<std::vector<uint8_t>> encryptBuffer(const uint8_t* plaintext, size_t len,
                                               const uint8_t* key, size_t keyLen,
                                               const uint8_t* iv,
                                               VaultAlgorithm algo) const;

    Result<std::vector<uint8_t>> decryptBuffer(const uint8_t* ciphertext, size_t len,
                                               const uint8_t* key, size_t keyLen,
                                               const uint8_t* iv,
                                               VaultAlgorithm algo) const;

    // Encrypt / decrypt one sector for XTS mode (sector number used as tweak)
    Result<void> encryptSectorXts(uint8_t* buffer, size_t len,
                                  const uint8_t* key, uint64_t sectorNumber) const;
    Result<void> decryptSectorXts(uint8_t* buffer, size_t len,
                                  const uint8_t* key, uint64_t sectorNumber) const;

    // Generate cryptographically random bytes via BCryptGenRandom
    Result<void> generateRandom(uint8_t* out, size_t len) const;

    // Create a VHD from decrypted data and attach it
    Result<QString> createAndAttachVhd(const std::vector<uint8_t>& decryptedData,
                                       const QString& vaultPath, bool readOnly) const;

    // Detach and delete the temporary VHD
    Result<void> detachVhd(const QString& vhdPath) const;

    // Read entire key file and hash it (SHA-256)
    Result<std::vector<uint8_t>> hashKeyFile(const QString& keyFilePath) const;

    // Track mounted vaults
    mutable std::mutex m_mutex;
    struct MountEntry
    {
        MountedVaultInfo info;
        QString          tempVhdPath;
    };
    std::unordered_map<std::string, MountEntry> m_mounted; // keyed by vault path (UTF-8)
};

} // namespace spw
