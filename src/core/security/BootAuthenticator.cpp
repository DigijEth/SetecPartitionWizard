#include "BootAuthenticator.h"
#include "../common/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStorageInfo>

#include <cstring>
#include <iomanip>
#include <sstream>

// For USB serial number retrieval via SetupAPI
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "setupapi.lib")

namespace spw
{

// ============================================================
// Constructor / Destructor
// ============================================================

BootAuthenticator::BootAuthenticator() = default;
BootAuthenticator::~BootAuthenticator() = default;

// ============================================================
// BCrypt helper: SHA-256
// ============================================================

Result<std::vector<uint8_t>> BootAuthenticator::sha256(
    const uint8_t* data, size_t len) const
{
    BCRYPT_ALG_HANDLE hAlgo = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &hAlgo, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "Failed to open SHA-256 provider");
    }

    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlgo, &hHash, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "Failed to create SHA-256 hash");
    }

    status = BCryptHashData(hHash, const_cast<PUCHAR>(data),
                            static_cast<ULONG>(len), 0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "SHA-256 hash data failed");
    }

    std::vector<uint8_t> hash(32, 0);
    status = BCryptFinishHash(hHash, hash.data(), 32, 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlgo, 0);

    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "SHA-256 finish failed");
    }

    return hash;
}

// ============================================================
// BCrypt helper: HMAC-SHA256
// ============================================================

Result<std::vector<uint8_t>> BootAuthenticator::hmacSha256(
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
        hAlgo, &hHash, nullptr, 0,
        const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "Failed to create HMAC-SHA256 hash");
    }

    status = BCryptHashData(hHash, const_cast<PUCHAR>(data),
                            static_cast<ULONG>(dataLen), 0);
    if (!BCRYPT_SUCCESS(status))
    {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlgo, 0);
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "HMAC-SHA256 hash data failed");
    }

    std::vector<uint8_t> hmac(32, 0);
    status = BCryptFinishHash(hHash, hmac.data(), 32, 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlgo, 0);

    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::EncryptionFailed,
                                   "HMAC-SHA256 finish failed");
    }

    return hmac;
}

// ============================================================
// BCrypt helper: random bytes
// ============================================================

Result<void> BootAuthenticator::generateRandom(uint8_t* out, size_t len) const
{
    NTSTATUS status = BCryptGenRandom(nullptr, out, static_cast<ULONG>(len),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status))
    {
        return ErrorInfo::fromCode(ErrorCode::KeyGenerationFailed,
                                   "BCryptGenRandom failed");
    }
    return Result<void>::ok();
}

// ============================================================
// Constant-time comparison
// ============================================================

bool BootAuthenticator::constantTimeCompare(
    const uint8_t* a, const uint8_t* b, size_t len)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i)
    {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

// ============================================================
// Hex conversion helpers (local to this TU)
// ============================================================

static std::string toHex(const uint8_t* data, size_t len)
{
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i)
    {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<unsigned>(data[i]);
    }
    return oss.str();
}

static std::vector<uint8_t> fromHex(const std::string& hex)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        uint8_t byte = static_cast<uint8_t>(
            std::stoi(hex.substr(i, 2), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

// ============================================================
// USB serial number retrieval
// ============================================================

Result<QString> BootAuthenticator::getUsbSerialForDrive(
    const QString& driveLetter) const
{
    // Get the volume name for this drive letter
    QString rootPath = driveLetter;
    if (!rootPath.endsWith("\\"))
        rootPath += "\\";

    wchar_t volumeName[MAX_PATH] = {};
    if (!GetVolumeNameForVolumeMountPointW(
            rootPath.toStdWString().c_str(),
            volumeName, MAX_PATH))
    {
        DWORD err = GetLastError();
        return ErrorInfo::fromWin32(ErrorCode::DiskNotFound, err,
                                    "Cannot get volume name for " + driveLetter.toStdString());
    }

    // Remove the trailing backslash for QueryDosDevice
    std::wstring volName(volumeName);
    // Strip the "\\\\?\\" prefix and trailing "\\"
    if (volName.size() > 4 && volName.substr(0, 4) == L"\\\\?\\")
    {
        volName = volName.substr(4);
    }
    while (!volName.empty() && volName.back() == L'\\')
    {
        volName.pop_back();
    }

    // Use SetupAPI to enumerate USB disk devices and match by volume
    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_DISK, L"USB", nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (devInfo == INVALID_HANDLE_VALUE)
    {
        // Fallback: use GetVolumeInformationW serial
        DWORD volumeSerial = 0;
        if (GetVolumeInformationW(
                rootPath.toStdWString().c_str(),
                nullptr, 0, &volumeSerial,
                nullptr, nullptr, nullptr, 0))
        {
            char serialBuf[16] = {};
            snprintf(serialBuf, sizeof(serialBuf), "%08X", volumeSerial);
            return QString(serialBuf);
        }

        return ErrorInfo::fromCode(ErrorCode::DiskNotFound,
                                   "Cannot enumerate USB devices for serial number");
    }

    SP_DEVINFO_DATA devInfoData = {};
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    QString foundSerial;

    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devInfoData); ++i)
    {
        // Get the device instance ID, which contains the USB serial for USB devices
        // Format: USB\VID_xxxx&PID_xxxx\serial_number
        wchar_t instanceId[MAX_PATH] = {};
        if (!SetupDiGetDeviceInstanceIdW(devInfo, &devInfoData,
                                          instanceId, MAX_PATH, nullptr))
        {
            continue;
        }

        std::wstring instIdStr(instanceId);

        // Extract the serial number (last component after the last backslash)
        size_t lastBackslash = instIdStr.rfind(L'\\');
        if (lastBackslash == std::wstring::npos)
            continue;

        std::wstring serial = instIdStr.substr(lastBackslash + 1);

        // Check if this device corresponds to our drive letter.
        // We match by checking the device's drive letter via
        // CM_Get_Device_Interface_List, but a simpler heuristic is
        // to get the device number and compare.

        // For a practical approach, we enumerate disk interfaces for this device
        SP_DEVICE_INTERFACE_DATA interfaceData = {};
        interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        if (SetupDiEnumDeviceInterfaces(devInfo, &devInfoData,
                                         &GUID_DEVINTERFACE_DISK, 0, &interfaceData))
        {
            DWORD requiredSize = 0;
            SetupDiGetDeviceInterfaceDetailW(devInfo, &interfaceData,
                                              nullptr, 0, &requiredSize, nullptr);

            if (requiredSize > 0)
            {
                std::vector<uint8_t> detailBuf(requiredSize, 0);
                auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

                if (SetupDiGetDeviceInterfaceDetailW(devInfo, &interfaceData,
                                                      detail, requiredSize, nullptr, nullptr))
                {
                    // Open the disk to get its device number
                    HANDLE hDisk = CreateFileW(
                        detail->DevicePath,
                        0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, 0, nullptr);

                    if (hDisk != INVALID_HANDLE_VALUE)
                    {
                        STORAGE_DEVICE_NUMBER sdn = {};
                        DWORD bytesReturned = 0;
                        if (DeviceIoControl(hDisk, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                             nullptr, 0, &sdn, sizeof(sdn),
                                             &bytesReturned, nullptr))
                        {
                            // Now check if our drive letter is on this physical disk
                            std::wstring driveDevPath = L"\\\\.\\" + driveLetter.toStdWString();
                            // Remove trailing colon if just letter
                            if (driveDevPath.back() != L':')
                                driveDevPath += L':';

                            HANDLE hVol = CreateFileW(
                                driveDevPath.c_str(),
                                0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);

                            if (hVol != INVALID_HANDLE_VALUE)
                            {
                                STORAGE_DEVICE_NUMBER volSdn = {};
                                if (DeviceIoControl(hVol, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                                     nullptr, 0, &volSdn, sizeof(volSdn),
                                                     &bytesReturned, nullptr))
                                {
                                    if (sdn.DeviceNumber == volSdn.DeviceNumber)
                                    {
                                        foundSerial = QString::fromStdWString(serial);
                                    }
                                }
                                CloseHandle(hVol);
                            }
                        }
                        CloseHandle(hDisk);
                    }
                }
            }
        }

        if (!foundSerial.isEmpty())
            break;
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    if (foundSerial.isEmpty())
    {
        // Fallback: use volume serial number
        DWORD volumeSerial = 0;
        if (GetVolumeInformationW(
                rootPath.toStdWString().c_str(),
                nullptr, 0, &volumeSerial,
                nullptr, nullptr, nullptr, 0))
        {
            char serialBuf[16] = {};
            snprintf(serialBuf, sizeof(serialBuf), "%08X", volumeSerial);
            return QString(serialBuf);
        }

        return ErrorInfo::fromCode(ErrorCode::DiskNotFound,
                                   "Cannot determine USB serial for drive " +
                                   driveLetter.toStdString());
    }

    return foundSerial;
}

// ============================================================
// Enumerate USB drives
// ============================================================

Result<std::vector<UsbDriveInfo>> BootAuthenticator::enumerateUsbDrives() const
{
    std::vector<UsbDriveInfo> drives;

    // Get all mounted volumes
    for (const QStorageInfo& storage : QStorageInfo::mountedVolumes())
    {
        if (!storage.isValid() || !storage.isReady())
            continue;

        QString rootPath = storage.rootPath();
        if (rootPath.isEmpty())
            continue;

        // Check if this is a removable drive
        std::wstring rootW = rootPath.toStdWString();
        if (!rootW.empty() && rootW.back() != L'\\')
            rootW += L'\\';

        UINT driveType = GetDriveTypeW(rootW.c_str());
        if (driveType != DRIVE_REMOVABLE)
            continue;

        UsbDriveInfo info;
        info.driveLetter = rootPath.left(2); // "E:"
        info.volumeLabel = storage.name();
        info.totalBytes  = static_cast<uint64_t>(storage.bytesTotal());
        info.freeBytes   = static_cast<uint64_t>(storage.bytesAvailable());

        // Try to get USB serial
        auto serialResult = getUsbSerialForDrive(info.driveLetter);
        if (serialResult.isOk())
        {
            info.serialNumber = serialResult.value();
        }

        // Check if a boot token already exists
        QString tokenPath = info.driveLetter + QString::fromWCharArray(BOOT_TOKEN_FILENAME);
        info.hasBootToken = QFile::exists(tokenPath);

        // Get volume info for manufacturer/product (limited info available)
        wchar_t volName[MAX_PATH] = {};
        wchar_t fsName[MAX_PATH] = {};
        GetVolumeInformationW(rootW.c_str(), volName, MAX_PATH,
                               nullptr, nullptr, nullptr, fsName, MAX_PATH);
        if (wcslen(volName) > 0 && info.volumeLabel.isEmpty())
        {
            info.volumeLabel = QString::fromWCharArray(volName);
        }

        drives.push_back(std::move(info));
    }

    return drives;
}

// ============================================================
// Generate token blob
// ============================================================

Result<std::vector<uint8_t>> BootAuthenticator::generateTokenBlob(
    const QString& usbSerial) const
{
    std::vector<uint8_t> blob(BOOT_TOKEN_FILE_SIZE, 0);
    size_t offset = 0;

    // Magic (8 bytes)
    std::memcpy(blob.data() + offset, BOOT_MAGIC, BOOT_MAGIC_LEN);
    offset += BOOT_MAGIC_LEN;

    // Device serial hash (SHA-256 of the USB serial string)
    QByteArray serialBytes = usbSerial.toUtf8();
    auto serialHashResult = sha256(
        reinterpret_cast<const uint8_t*>(serialBytes.constData()),
        static_cast<size_t>(serialBytes.size()));
    if (serialHashResult.isError())
        return serialHashResult.error();

    std::memcpy(blob.data() + offset, serialHashResult.value().data(), BOOT_SERIAL_HASH_LEN);
    offset += BOOT_SERIAL_HASH_LEN;

    // Random token (32 bytes)
    auto randResult = generateRandom(blob.data() + offset, BOOT_TOKEN_LEN);
    if (randResult.isError())
        return randResult.error();
    offset += BOOT_TOKEN_LEN;

    // HMAC-SHA256 over bytes 0x00..0x47 (magic + serial hash + token), keyed by the token
    const uint8_t* tokenPtr = blob.data() + BOOT_MAGIC_LEN + BOOT_SERIAL_HASH_LEN;
    auto hmacResult = hmacSha256(
        tokenPtr, BOOT_TOKEN_LEN,
        blob.data(), BOOT_MAGIC_LEN + BOOT_SERIAL_HASH_LEN + BOOT_TOKEN_LEN);
    if (hmacResult.isError())
        return hmacResult.error();

    std::memcpy(blob.data() + offset, hmacResult.value().data(), BOOT_HMAC_LEN);

    return blob;
}

// ============================================================
// Create boot key
// ============================================================

Result<void> BootAuthenticator::createBootKey(const QString& driveLetter)
{
    log::info("Creating boot key on drive: " + driveLetter);

    // Get USB serial number
    auto serialResult = getUsbSerialForDrive(driveLetter);
    if (serialResult.isError())
    {
        return ErrorInfo::fromCode(ErrorCode::DiskNotFound,
                                   "Cannot determine USB serial for drive " +
                                   driveLetter.toStdString() +
                                   " — is this a USB drive?");
    }

    const QString& usbSerial = serialResult.value();

    // Generate the token blob
    auto blobResult = generateTokenBlob(usbSerial);
    if (blobResult.isError())
        return blobResult.error();

    const auto& blob = blobResult.value();

    // Write the .spwboot file
    QString tokenPath = driveLetter + QString::fromWCharArray(BOOT_TOKEN_FILENAME);
    QFile tokenFile(tokenPath);

    if (!tokenFile.open(QIODevice::WriteOnly))
    {
        return ErrorInfo::fromCode(ErrorCode::FileCreateFailed,
                                   "Cannot create boot token file: " +
                                   tokenPath.toStdString());
    }

    qint64 written = tokenFile.write(
        reinterpret_cast<const char*>(blob.data()),
        static_cast<qint64>(blob.size()));
    tokenFile.flush();
    tokenFile.close();

    if (written != static_cast<qint64>(BOOT_TOKEN_FILE_SIZE))
    {
        QFile::remove(tokenPath);
        return ErrorInfo::fromCode(ErrorCode::DiskWriteError,
                                   "Failed to write boot token file");
    }

    // Set the file as hidden + system
    std::wstring tokenPathW = tokenPath.toStdWString();
    SetFileAttributesW(tokenPathW.c_str(),
                       FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);

    // Compute the token hash (SHA-256 of the random token) for storage
    const uint8_t* tokenPtr = blob.data() + BOOT_MAGIC_LEN + BOOT_SERIAL_HASH_LEN;
    auto tokenHashResult = sha256(tokenPtr, BOOT_TOKEN_LEN);
    if (tokenHashResult.isError())
        return tokenHashResult.error();

    // Compute serial hash for config
    QByteArray serialBytes = usbSerial.toUtf8();
    auto serialHashResult = sha256(
        reinterpret_cast<const uint8_t*>(serialBytes.constData()),
        static_cast<size_t>(serialBytes.size()));
    if (serialHashResult.isError())
        return serialHashResult.error();

    // Save configuration
    BootKeyConfig config;
    config.enabled           = true;
    config.tokenHashHex      = QString::fromStdString(
        toHex(tokenHashResult.value().data(), tokenHashResult.value().size()));
    config.serialHashHex     = QString::fromStdString(
        toHex(serialHashResult.value().data(), serialHashResult.value().size()));
    config.createdTimestamp   = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    config.lastVerifiedTimestamp.clear();

    saveConfig(config);

    log::info("Boot key created successfully on " + driveLetter);
    return Result<void>::ok();
}

// ============================================================
// Read token file
// ============================================================

Result<std::vector<uint8_t>> BootAuthenticator::readTokenFile(
    const QString& driveLetter) const
{
    QString tokenPath = driveLetter + QString::fromWCharArray(BOOT_TOKEN_FILENAME);
    QFile file(tokenPath);

    if (!file.open(QIODevice::ReadOnly))
    {
        return ErrorInfo::fromCode(ErrorCode::FileNotFound,
                                   "Boot token file not found on " +
                                   driveLetter.toStdString());
    }

    QByteArray raw = file.readAll();
    file.close();

    if (raw.size() != static_cast<int>(BOOT_TOKEN_FILE_SIZE))
    {
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "Boot token file has wrong size: expected " +
                                   std::to_string(BOOT_TOKEN_FILE_SIZE) + ", got " +
                                   std::to_string(raw.size()));
    }

    return std::vector<uint8_t>(
        reinterpret_cast<const uint8_t*>(raw.constData()),
        reinterpret_cast<const uint8_t*>(raw.constData()) + raw.size());
}

// ============================================================
// Validate token blob
// ============================================================

Result<void> BootAuthenticator::validateTokenBlob(
    const uint8_t* data, size_t len) const
{
    if (!data || len != BOOT_TOKEN_FILE_SIZE)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                   "Invalid token blob size");
    }

    // Check magic
    if (std::memcmp(data, BOOT_MAGIC, BOOT_MAGIC_LEN) != 0)
    {
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "Invalid boot token magic");
    }

    // Verify HMAC
    const uint8_t* tokenPtr = data + BOOT_MAGIC_LEN + BOOT_SERIAL_HASH_LEN;
    const uint8_t* storedHmac = data + BOOT_MAGIC_LEN + BOOT_SERIAL_HASH_LEN + BOOT_TOKEN_LEN;

    size_t hmacInputLen = BOOT_MAGIC_LEN + BOOT_SERIAL_HASH_LEN + BOOT_TOKEN_LEN;
    auto hmacResult = hmacSha256(tokenPtr, BOOT_TOKEN_LEN,
                                 data, hmacInputLen);
    if (hmacResult.isError())
        return hmacResult.error();

    if (!constantTimeCompare(storedHmac, hmacResult.value().data(), BOOT_HMAC_LEN))
    {
        return ErrorInfo::fromCode(ErrorCode::DecryptionFailed,
                                   "Boot token HMAC verification failed — "
                                   "token may be corrupted or tampered with");
    }

    return Result<void>::ok();
}

// ============================================================
// Verify boot key (scan all USB drives)
// ============================================================

Result<QString> BootAuthenticator::verifyBootKey() const
{
    BootKeyConfig config = loadConfig();
    if (!config.enabled)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                   "Boot authentication is not enabled");
    }

    if (config.tokenHashHex.isEmpty() || config.serialHashHex.isEmpty())
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                   "Boot key configuration is incomplete");
    }

    // Enumerate USB drives and check each for a valid token
    auto drivesResult = enumerateUsbDrives();
    if (drivesResult.isError())
        return drivesResult.error();

    std::vector<uint8_t> expectedTokenHash = fromHex(config.tokenHashHex.toStdString());
    std::vector<uint8_t> expectedSerialHash = fromHex(config.serialHashHex.toStdString());

    for (const auto& drive : drivesResult.value())
    {
        auto tokenResult = readTokenFile(drive.driveLetter);
        if (tokenResult.isError())
            continue; // No token on this drive

        const auto& blob = tokenResult.value();

        // Validate blob structure and HMAC
        auto validateResult = validateTokenBlob(blob.data(), blob.size());
        if (validateResult.isError())
            continue;

        // Check serial hash matches stored config
        const uint8_t* serialHash = blob.data() + BOOT_MAGIC_LEN;
        if (expectedSerialHash.size() == BOOT_SERIAL_HASH_LEN &&
            !constantTimeCompare(serialHash, expectedSerialHash.data(), BOOT_SERIAL_HASH_LEN))
        {
            continue; // Serial mismatch — not our registered key
        }

        // Check token hash matches stored config
        const uint8_t* token = blob.data() + BOOT_MAGIC_LEN + BOOT_SERIAL_HASH_LEN;
        auto tokenHashResult = sha256(token, BOOT_TOKEN_LEN);
        if (tokenHashResult.isError())
            continue;

        if (expectedTokenHash.size() == 32 &&
            constantTimeCompare(tokenHashResult.value().data(),
                                expectedTokenHash.data(), 32))
        {
            // Match found — update last verified timestamp
            // (const method, so we cast away const for this bookkeeping)
            const_cast<BootAuthenticator*>(this)->saveConfig([&]() {
                BootKeyConfig updated = config;
                updated.lastVerifiedTimestamp =
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
                return updated;
            }());

            log::info("Boot key verified on drive: " + drive.driveLetter);
            return drive.driveLetter;
        }
    }

    return ErrorInfo::fromCode(ErrorCode::Fido2AuthFailed,
                               "No valid boot key found on any connected USB drive");
}

// ============================================================
// Verify a specific drive
// ============================================================

Result<void> BootAuthenticator::verifyDrive(const QString& driveLetter) const
{
    BootKeyConfig config = loadConfig();
    if (!config.enabled)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                   "Boot authentication is not enabled");
    }

    auto tokenResult = readTokenFile(driveLetter);
    if (tokenResult.isError())
        return tokenResult.error();

    const auto& blob = tokenResult.value();

    // Validate structure
    auto validateResult = validateTokenBlob(blob.data(), blob.size());
    if (validateResult.isError())
        return validateResult.error();

    // Check serial hash
    std::vector<uint8_t> expectedSerialHash = fromHex(config.serialHashHex.toStdString());
    const uint8_t* serialHash = blob.data() + BOOT_MAGIC_LEN;
    if (expectedSerialHash.size() == BOOT_SERIAL_HASH_LEN &&
        !constantTimeCompare(serialHash, expectedSerialHash.data(), BOOT_SERIAL_HASH_LEN))
    {
        return ErrorInfo::fromCode(ErrorCode::Fido2AuthFailed,
                                   "USB device serial does not match registered boot key");
    }

    // Check token hash
    std::vector<uint8_t> expectedTokenHash = fromHex(config.tokenHashHex.toStdString());
    const uint8_t* token = blob.data() + BOOT_MAGIC_LEN + BOOT_SERIAL_HASH_LEN;
    auto tokenHashResult = sha256(token, BOOT_TOKEN_LEN);
    if (tokenHashResult.isError())
        return tokenHashResult.error();

    if (expectedTokenHash.size() == 32 &&
        !constantTimeCompare(tokenHashResult.value().data(),
                             expectedTokenHash.data(), 32))
    {
        return ErrorInfo::fromCode(ErrorCode::Fido2AuthFailed,
                                   "Boot token does not match registered configuration");
    }

    log::info("Boot key verified on drive: " + driveLetter);
    return Result<void>::ok();
}

// ============================================================
// Configuration management
// ============================================================

bool BootAuthenticator::isEnabled() const
{
    return loadConfig().enabled;
}

Result<void> BootAuthenticator::setEnabled(bool enabled)
{
    BootKeyConfig config = loadConfig();

    if (!enabled)
    {
        // Disabling — clear token data
        config.enabled = false;
        config.tokenHashHex.clear();
        config.serialHashHex.clear();
        saveConfig(config);
        log::info("Boot authentication disabled");
    }
    else
    {
        if (config.tokenHashHex.isEmpty())
        {
            return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                       "Cannot enable boot auth without creating a boot key first");
        }
        config.enabled = true;
        saveConfig(config);
        log::info("Boot authentication enabled");
    }

    return Result<void>::ok();
}

BootKeyConfig BootAuthenticator::getConfig() const
{
    return loadConfig();
}

Result<void> BootAuthenticator::removeBootKey(bool wipeUsbToken)
{
    BootKeyConfig config = loadConfig();

    if (wipeUsbToken)
    {
        // Try to find and delete the token file from connected USB drives
        auto drivesResult = enumerateUsbDrives();
        if (drivesResult.isOk())
        {
            for (const auto& drive : drivesResult.value())
            {
                if (drive.hasBootToken)
                {
                    QString tokenPath = drive.driveLetter +
                                        QString::fromWCharArray(BOOT_TOKEN_FILENAME);

                    // Overwrite with random data before deleting (secure wipe)
                    QFile tokenFile(tokenPath);
                    if (tokenFile.open(QIODevice::WriteOnly))
                    {
                        std::vector<uint8_t> randomData(BOOT_TOKEN_FILE_SIZE, 0);
                        generateRandom(randomData.data(), randomData.size());
                        tokenFile.write(
                            reinterpret_cast<const char*>(randomData.data()),
                            static_cast<qint64>(randomData.size()));
                        tokenFile.flush();
                        tokenFile.close();

                        SecureZeroMemory(randomData.data(), randomData.size());
                    }

                    // Remove the hidden/system attributes so we can delete
                    std::wstring tokenPathW = tokenPath.toStdWString();
                    SetFileAttributesW(tokenPathW.c_str(), FILE_ATTRIBUTE_NORMAL);
                    QFile::remove(tokenPath);

                    log::info("Wiped boot token from " + drive.driveLetter);
                }
            }
        }
    }

    // Clear configuration
    config.enabled = false;
    config.tokenHashHex.clear();
    config.serialHashHex.clear();
    config.createdTimestamp.clear();
    config.lastVerifiedTimestamp.clear();
    saveConfig(config);

    log::info("Boot key configuration removed");
    return Result<void>::ok();
}

// ============================================================
// QSettings persistence
// ============================================================

void BootAuthenticator::saveConfig(const BootKeyConfig& config)
{
    QSettings settings("SetecAstronomy", "SetecPartitionWizard");
    settings.beginGroup("Security/BootAuth");

    settings.setValue("enabled",            config.enabled);
    settings.setValue("tokenHashHex",       config.tokenHashHex);
    settings.setValue("serialHashHex",      config.serialHashHex);
    settings.setValue("createdTimestamp",    config.createdTimestamp);
    settings.setValue("lastVerifiedTimestamp", config.lastVerifiedTimestamp);

    settings.endGroup();
    settings.sync();
}

BootKeyConfig BootAuthenticator::loadConfig() const
{
    QSettings settings("SetecAstronomy", "SetecPartitionWizard");
    settings.beginGroup("Security/BootAuth");

    BootKeyConfig config;
    config.enabled              = settings.value("enabled", false).toBool();
    config.tokenHashHex         = settings.value("tokenHashHex").toString();
    config.serialHashHex        = settings.value("serialHashHex").toString();
    config.createdTimestamp      = settings.value("createdTimestamp").toString();
    config.lastVerifiedTimestamp = settings.value("lastVerifiedTimestamp").toString();

    settings.endGroup();
    return config;
}

} // namespace spw
