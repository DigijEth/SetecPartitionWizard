#pragma once

// BootAuthenticator — Create and verify USB boot authentication tokens.
// Writes a unique token to a USB drive that can gate application access
// (and in principle, pre-boot authentication once a custom bootloader is added).
// DISCLAIMER: This code is for authorized security utility software only.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <bcrypt.h>
#include <winioctl.h>
#include <setupapi.h>

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"

#include <QString>
#include <QSettings>
#include <cstdint>
#include <string>
#include <vector>

namespace spw
{

// ------------------------------------------------------------------
// USB boot token on-disk format (written to X:\.spwboot):
//
//   Offset  Size   Field
//   0x00    8      Magic "SPWBOOT1"
//   0x08    32     DeviceSerialHash (SHA-256 of USB serial number string)
//   0x28    32     Random token (256 bits)
//   0x48    32     HMAC-SHA256 over bytes 0x00..0x47, keyed by token
//   0x68    —      (total = 104 bytes)
// ------------------------------------------------------------------

static constexpr size_t  BOOT_MAGIC_LEN       = 8;
static constexpr char    BOOT_MAGIC[]          = "SPWBOOT1";
static constexpr size_t  BOOT_SERIAL_HASH_LEN  = 32;
static constexpr size_t  BOOT_TOKEN_LEN        = 32; // 256-bit random token
static constexpr size_t  BOOT_HMAC_LEN         = 32;
static constexpr size_t  BOOT_TOKEN_FILE_SIZE  = BOOT_MAGIC_LEN + BOOT_SERIAL_HASH_LEN
                                                + BOOT_TOKEN_LEN + BOOT_HMAC_LEN;
static constexpr wchar_t BOOT_TOKEN_FILENAME[] = L"\\.spwboot";

// Information about a USB drive suitable for boot key use
struct UsbDriveInfo
{
    QString  driveLetter;         // e.g. "E:"
    QString  volumeLabel;
    QString  serialNumber;        // Device serial (from USB descriptor)
    QString  manufacturer;
    QString  productName;
    uint64_t totalBytes  = 0;
    uint64_t freeBytes   = 0;
    bool     hasBootToken = false; // True if .spwboot already present
};

// Boot key configuration persisted in QSettings
struct BootKeyConfig
{
    bool     enabled          = false;
    QString  tokenHashHex;        // SHA-256 of the token (stored, not the token itself)
    QString  serialHashHex;       // SHA-256 of the allowed USB serial
    QString  createdTimestamp;     // ISO 8601
    QString  lastVerifiedTimestamp;
};

class BootAuthenticator
{
public:
    BootAuthenticator();
    ~BootAuthenticator();

    // Non-copyable
    BootAuthenticator(const BootAuthenticator&) = delete;
    BootAuthenticator& operator=(const BootAuthenticator&) = delete;

    // ---- USB drive enumeration ----

    // List USB drives available for boot key creation.
    Result<std::vector<UsbDriveInfo>> enumerateUsbDrives() const;

    // ---- Token creation ----

    // Prepare a USB drive as a boot key.  Writes the .spwboot token file
    // and saves the token hash in QSettings.
    Result<void> createBootKey(const QString& driveLetter);

    // ---- Token verification ----

    // Verify that a USB drive with a valid boot token is connected.
    // Returns the drive letter of the matching key, or an error.
    Result<QString> verifyBootKey() const;

    // Verify a specific drive's boot token against the stored configuration.
    Result<void> verifyDrive(const QString& driveLetter) const;

    // ---- Configuration ----

    // Check whether boot authentication is enabled.
    bool isEnabled() const;

    // Enable or disable boot authentication.  When disabling, the stored
    // token hash is cleared.
    Result<void> setEnabled(bool enabled);

    // Read current boot key configuration from QSettings.
    BootKeyConfig getConfig() const;

    // Remove all boot key configuration and optionally wipe the token
    // from the USB drive.
    Result<void> removeBootKey(bool wipeUsbToken = true);

    // ---- Low-level helpers (public for testing) ----

    // Read the .spwboot token file from a drive letter.
    Result<std::vector<uint8_t>> readTokenFile(const QString& driveLetter) const;

    // Validate the structure and HMAC of a token blob.
    Result<void> validateTokenBlob(const uint8_t* data, size_t len) const;

private:
    // Generate a fresh boot token blob (BOOT_TOKEN_FILE_SIZE bytes).
    Result<std::vector<uint8_t>> generateTokenBlob(const QString& usbSerial) const;

    // Get the USB serial number for a given drive letter.
    Result<QString> getUsbSerialForDrive(const QString& driveLetter) const;

    // Compute SHA-256 of arbitrary data using BCrypt.
    Result<std::vector<uint8_t>> sha256(const uint8_t* data, size_t len) const;

    // Compute HMAC-SHA256 using BCrypt.
    Result<std::vector<uint8_t>> hmacSha256(const uint8_t* key, size_t keyLen,
                                            const uint8_t* data, size_t dataLen) const;

    // Generate cryptographically random bytes.
    Result<void> generateRandom(uint8_t* out, size_t len) const;

    // Save/load config in QSettings under "Security/BootAuth" group.
    void saveConfig(const BootKeyConfig& config);
    BootKeyConfig loadConfig() const;

    // Constant-time comparison for HMAC verification.
    static bool constantTimeCompare(const uint8_t* a, const uint8_t* b, size_t len);
};

} // namespace spw
