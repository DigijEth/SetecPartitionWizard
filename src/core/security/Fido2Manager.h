#pragma once

// Fido2Manager — Enumerate, inspect, and manage FIDO2/WebAuthn security keys.
// Uses Windows HID enumeration (SetupAPI) for device discovery with FIDO usage
// page 0xF1D0, and the Windows WebAuthn API (webauthn.dll) for credential
// operations.
// DISCLAIMER: This code is for authorized security utility software only.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "../common/Error.h"
#include "../common/Result.h"

#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace spw
{

// FIDO2 HID usage page (defined by FIDO Alliance)
static constexpr uint16_t FIDO_USAGE_PAGE = 0xF1D0;
static constexpr uint16_t FIDO_USAGE_ID   = 0x01;

// CTAP2 command bytes
static constexpr uint8_t CTAP2_CMD_MAKE_CREDENTIAL    = 0x01;
static constexpr uint8_t CTAP2_CMD_GET_ASSERTION       = 0x02;
static constexpr uint8_t CTAP2_CMD_GET_INFO            = 0x04;
static constexpr uint8_t CTAP2_CMD_CLIENT_PIN          = 0x06;
static constexpr uint8_t CTAP2_CMD_RESET               = 0x07;
static constexpr uint8_t CTAP2_CMD_GET_NEXT_ASSERTION   = 0x08;

// CTAP2 clientPin subcommands
static constexpr uint8_t PIN_SUBCMD_GET_RETRIES     = 0x01;
static constexpr uint8_t PIN_SUBCMD_GET_KEY_AGREEMENT = 0x02;
static constexpr uint8_t PIN_SUBCMD_SET_PIN          = 0x03;
static constexpr uint8_t PIN_SUBCMD_CHANGE_PIN       = 0x04;
static constexpr uint8_t PIN_SUBCMD_GET_PIN_TOKEN    = 0x05;

// CTAP HID frame constants
static constexpr uint32_t CTAPHID_INIT        = 0x06;
static constexpr uint32_t CTAPHID_MSG         = 0x03;
static constexpr uint32_t CTAPHID_CBOR        = 0x10;
static constexpr uint32_t CTAPHID_PING        = 0x01;
static constexpr uint32_t CTAPHID_ERROR       = 0x3F;
static constexpr uint32_t CTAPHID_BROADCAST_CID = 0xFFFFFFFF;

// Maximum HID report sizes for FIDO
static constexpr size_t CTAPHID_REPORT_SIZE   = 64;
static constexpr size_t CTAPHID_INIT_NONCE_LEN = 8;

// Information about a connected FIDO2 device
struct Fido2DeviceInfo
{
    QString  devicePath;          // HID device path for opening
    QString  manufacturer;        // Manufacturer string
    QString  product;             // Product name
    QString  serialNumber;        // Serial number (may be empty)
    uint16_t vendorId  = 0;
    uint16_t productId = 0;

    // Populated by getDeviceDetails()
    std::vector<std::string> protocols;    // e.g. "FIDO_2_0", "U2F_V2"
    std::vector<std::string> extensions;   // Supported extensions
    std::string firmwareVersion;           // aaguid or firmware string
    bool     supportsPinProtocol = false;
    bool     hasPin              = false;
    uint32_t pinRetryCount       = 0;
};

// WebAuthn credential result
struct WebAuthnCredentialResult
{
    std::vector<uint8_t> credentialId;
    std::vector<uint8_t> attestationObject;
    std::vector<uint8_t> clientDataJson;
};

// WebAuthn assertion result
struct WebAuthnAssertionResult
{
    std::vector<uint8_t> credentialId;
    std::vector<uint8_t> authenticatorData;
    std::vector<uint8_t> signature;
    std::vector<uint8_t> userHandle;
};

class Fido2Manager
{
public:
    Fido2Manager();
    ~Fido2Manager();

    // Non-copyable
    Fido2Manager(const Fido2Manager&) = delete;
    Fido2Manager& operator=(const Fido2Manager&) = delete;

    // ---- Device enumeration ----

    // Enumerate all connected FIDO2 HID devices.
    Result<std::vector<Fido2DeviceInfo>> enumerateDevices() const;

    // Get detailed CTAP2 info for a specific device (authenticatorGetInfo).
    Result<Fido2DeviceInfo> getDeviceDetails(const QString& devicePath) const;

    // ---- PIN management (CTAP2 clientPin) ----

    // Get the number of PIN retries remaining.
    Result<uint32_t> getPinRetryCount(const QString& devicePath) const;

    // Set the PIN on a device that has no PIN yet.
    Result<void> setPin(const QString& devicePath, const QString& newPin) const;

    // Change the PIN on a device that already has one.
    Result<void> changePin(const QString& devicePath,
                           const QString& currentPin,
                           const QString& newPin) const;

    // ---- Device management ----

    // Factory reset (authenticatorReset).  Must be invoked within a short
    // window after the authenticator powers up.
    Result<void> factoryReset(const QString& devicePath) const;

    // ---- WebAuthn API (via webauthn.dll) ----

    // Check if the WebAuthn API is available on this system.
    Result<uint32_t> getApiVersion() const;

    // Check if a user-verifying platform authenticator is available.
    Result<bool> isPlatformAuthenticatorAvailable() const;

    // Create a credential (WebAuthNAuthenticatorMakeCredential wrapper).
    Result<WebAuthnCredentialResult> makeCredential(
        HWND parentWindow,
        const QString& rpId,
        const QString& rpName,
        const std::vector<uint8_t>& userId,
        const QString& userName,
        const std::vector<uint8_t>& challenge) const;

    // Get an assertion (WebAuthNAuthenticatorGetAssertion wrapper).
    Result<WebAuthnAssertionResult> getAssertion(
        HWND parentWindow,
        const QString& rpId,
        const std::vector<uint8_t>& challenge,
        const std::vector<uint8_t>& allowCredentialId = {}) const;

private:
    // CTAP HID transport helpers
    struct CtapHidChannel
    {
        HANDLE   handle = INVALID_HANDLE_VALUE;
        uint32_t cid    = 0; // Channel ID
    };

    Result<CtapHidChannel> openCtapChannel(const QString& devicePath) const;
    void closeCtapChannel(CtapHidChannel& channel) const;

    Result<std::vector<uint8_t>> ctapHidInit(HANDLE hidHandle) const;
    Result<std::vector<uint8_t>> ctapHidCborCommand(
        const CtapHidChannel& channel,
        uint8_t command,
        const std::vector<uint8_t>& cborPayload = {}) const;

    // Send/receive raw HID reports
    Result<void> sendHidReport(HANDLE handle, const uint8_t* data, size_t len) const;
    Result<std::vector<uint8_t>> recvHidReport(HANDLE handle, size_t maxLen, uint32_t timeoutMs = 5000) const;

    // Build CTAPHID frames
    static std::vector<std::vector<uint8_t>> buildInitPackets(
        uint32_t cid, uint8_t cmd, const uint8_t* data, size_t dataLen);

    // Parse CBOR response from authenticatorGetInfo
    Result<Fido2DeviceInfo> parseGetInfoResponse(const std::vector<uint8_t>& cborData,
                                                  const Fido2DeviceInfo& baseInfo) const;

    // WebAuthn DLL function pointers (loaded dynamically)
    struct WebAuthnApi
    {
        HMODULE dll = nullptr;
        bool    loaded = false;

        // Function pointers (typedefs match webauthn.h signatures)
        using PFN_GetApiVersionNumber = DWORD (WINAPI*)();
        using PFN_IsUserVerifyingPlatformAuthenticatorAvailable = HRESULT (WINAPI*)(BOOL*);

        PFN_GetApiVersionNumber pfnGetApiVersionNumber = nullptr;
        PFN_IsUserVerifyingPlatformAuthenticatorAvailable pfnIsAvailable = nullptr;

        // The full MakeCredential and GetAssertion pointers are stored as void*
        // because the struct layouts vary by API version; we cast at call time.
        void* pfnMakeCredential = nullptr;
        void* pfnGetAssertion   = nullptr;
        void* pfnFreeCredentialAttestation = nullptr;
        void* pfnFreeAssertion  = nullptr;
    };

    Result<void> ensureWebAuthnLoaded() const;

    mutable WebAuthnApi m_webAuthn;
};

} // namespace spw
