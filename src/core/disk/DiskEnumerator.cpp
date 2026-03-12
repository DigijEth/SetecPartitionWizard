#include "DiskEnumerator.h"
#include "RawDiskHandle.h"

// Windows headers for SetupAPI and WMI
#include <initguid.h>      // Must come before devguid.h/ntddstor.h for GUID definitions
#include <setupapi.h>
#include <devguid.h>
#include <winioctl.h>
#include <comdef.h>
#include <Wbemidl.h>

#include <algorithm>
#include <sstream>
#include <memory>

// Link against required libraries
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace spw
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static ErrorInfo makeWin32Error(ErrorCode code, const std::string& context)
{
    const DWORD lastErr = ::GetLastError();
    std::ostringstream oss;
    oss << context << " (Win32 error " << lastErr << ")";
    return ErrorInfo::fromWin32(code, lastErr, oss.str());
}

static ErrorInfo makeHResultError(ErrorCode code, HRESULT hr, const std::string& context)
{
    std::ostringstream oss;
    oss << context << " (HRESULT 0x" << std::hex << hr << ")";
    return ErrorInfo::fromHResult(code, hr, oss.str());
}

// Trim trailing whitespace (common in WMI strings and STORAGE_DEVICE_DESCRIPTOR)
static std::wstring trimRight(const std::wstring& str)
{
    auto end = str.find_last_not_of(L" \t\r\n");
    if (end == std::wstring::npos) return L"";
    return str.substr(0, end + 1);
}

// Convert a narrow ANSI string at an offset in a byte buffer to a wide string
static std::wstring narrowToWide(const char* narrowStr)
{
    if (!narrowStr || narrowStr[0] == '\0') return L"";

    int needed = ::MultiByteToWideChar(CP_ACP, 0, narrowStr, -1, nullptr, 0);
    if (needed <= 0) return L"";

    std::wstring result(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, narrowStr, -1, &result[0], needed);
    // Remove the null terminator that MultiByteToWideChar includes
    if (!result.empty() && result.back() == L'\0')
        result.pop_back();

    return trimRight(result);
}

// ---------------------------------------------------------------------------
// RAII wrapper for COM initialization
// ---------------------------------------------------------------------------
class ComInitGuard
{
public:
    ComInitGuard()
    {
        m_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    ~ComInitGuard()
    {
        if (SUCCEEDED(m_hr))
            ::CoUninitialize();
    }
    bool succeeded() const { return SUCCEEDED(m_hr); }
    HRESULT result() const { return m_hr; }

    ComInitGuard(const ComInitGuard&) = delete;
    ComInitGuard& operator=(const ComInitGuard&) = delete;
private:
    HRESULT m_hr;
};

// ---------------------------------------------------------------------------
// RAII wrapper for HDEVINFO
// ---------------------------------------------------------------------------
class DevInfoGuard
{
public:
    explicit DevInfoGuard(HDEVINFO h) : m_handle(h) {}
    ~DevInfoGuard()
    {
        if (m_handle != INVALID_HANDLE_VALUE)
            ::SetupDiDestroyDeviceInfoList(m_handle);
    }
    HDEVINFO get() const { return m_handle; }
    bool isValid() const { return m_handle != INVALID_HANDLE_VALUE; }

    DevInfoGuard(const DevInfoGuard&) = delete;
    DevInfoGuard& operator=(const DevInfoGuard&) = delete;
private:
    HDEVINFO m_handle;
};

// ---------------------------------------------------------------------------
// Helper: get STORAGE_DEVICE_DESCRIPTOR for a physical drive
// ---------------------------------------------------------------------------
static bool getStorageDescriptor(HANDLE diskHandle,
                                 std::wstring& outModel,
                                 std::wstring& outSerial,
                                 std::wstring& outFirmware,
                                 bool& outRemovable)
{
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    // First call to get the needed size
    STORAGE_DESCRIPTOR_HEADER header = {};
    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(diskHandle, IOCTL_STORAGE_QUERY_PROPERTY,
                                &query, sizeof(query),
                                &header, sizeof(header),
                                &bytesReturned, nullptr);
    if (!ok || header.Size == 0) return false;

    std::vector<uint8_t> buffer(header.Size, 0);
    ok = ::DeviceIoControl(diskHandle, IOCTL_STORAGE_QUERY_PROPERTY,
                           &query, sizeof(query),
                           buffer.data(), static_cast<DWORD>(buffer.size()),
                           &bytesReturned, nullptr);
    if (!ok) return false;

    const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());

    if (desc->VendorIdOffset != 0)
    {
        const char* vendor = reinterpret_cast<const char*>(buffer.data()) + desc->VendorIdOffset;
        std::wstring vendorW = narrowToWide(vendor);
        if (!vendorW.empty())
            outModel = vendorW + L" ";
    }

    if (desc->ProductIdOffset != 0)
    {
        const char* product = reinterpret_cast<const char*>(buffer.data()) + desc->ProductIdOffset;
        outModel += narrowToWide(product);
    }
    outModel = trimRight(outModel);

    if (desc->SerialNumberOffset != 0)
    {
        const char* serial = reinterpret_cast<const char*>(buffer.data()) + desc->SerialNumberOffset;
        outSerial = narrowToWide(serial);
    }

    if (desc->ProductRevisionOffset != 0)
    {
        const char* rev = reinterpret_cast<const char*>(buffer.data()) + desc->ProductRevisionOffset;
        outFirmware = narrowToWide(rev);
    }

    outRemovable = (desc->RemovableMedia != FALSE);

    return true;
}

// ---------------------------------------------------------------------------
// Helper: Detect interface type from STORAGE_ADAPTER_DESCRIPTOR bus type
// ---------------------------------------------------------------------------
static DiskInterfaceType busTypeToInterface(STORAGE_BUS_TYPE busType)
{
    switch (busType)
    {
    case BusTypeAta:       return DiskInterfaceType::IDE;
    case BusTypeSata:      return DiskInterfaceType::SATA;
    case BusTypeUsb:       return DiskInterfaceType::USB;
    case BusTypeScsi:      return DiskInterfaceType::SCSI;
    case BusTypeSas:       return DiskInterfaceType::SAS;
    case BusTypeNvme:      return DiskInterfaceType::NVMe;
    case BusTypeSd:        return DiskInterfaceType::MMC;
    case BusTypeMmc:       return DiskInterfaceType::MMC;
    case BusType1394:      return DiskInterfaceType::Firewire;
    case BusTypeVirtual:   return DiskInterfaceType::Virtual;
    case BusTypeFileBackedVirtual: return DiskInterfaceType::Virtual;
    default:               return DiskInterfaceType::Unknown;
    }
}

// ---------------------------------------------------------------------------
// Helper: Get bus type via STORAGE_ADAPTER_DESCRIPTOR
// ---------------------------------------------------------------------------
static DiskInterfaceType getInterfaceType(HANDLE diskHandle)
{
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageAdapterProperty;
    query.QueryType = PropertyStandardQuery;

    STORAGE_DESCRIPTOR_HEADER header = {};
    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(diskHandle, IOCTL_STORAGE_QUERY_PROPERTY,
                                &query, sizeof(query),
                                &header, sizeof(header),
                                &bytesReturned, nullptr);
    if (!ok || header.Size == 0) return DiskInterfaceType::Unknown;

    std::vector<uint8_t> buffer(header.Size, 0);
    ok = ::DeviceIoControl(diskHandle, IOCTL_STORAGE_QUERY_PROPERTY,
                           &query, sizeof(query),
                           buffer.data(), static_cast<DWORD>(buffer.size()),
                           &bytesReturned, nullptr);
    if (!ok) return DiskInterfaceType::Unknown;

    const auto* desc = reinterpret_cast<const STORAGE_ADAPTER_DESCRIPTOR*>(buffer.data());
    return busTypeToInterface(static_cast<STORAGE_BUS_TYPE>(desc->BusType));
}

// ---------------------------------------------------------------------------
// Helper: Detect if disk is SSD using IOCTL_ATA_PASS_THROUGH (IDENTIFY DEVICE)
// or by checking the seek penalty via IOCTL_STORAGE_QUERY_PROPERTY.
// ---------------------------------------------------------------------------
static MediaType detectMediaType(HANDLE diskHandle, DiskInterfaceType ifType, bool isRemovable)
{
    if (isRemovable)
    {
        if (ifType == DiskInterfaceType::USB) return MediaType::USBFlash;
        if (ifType == DiskInterfaceType::MMC) return MediaType::SDCard;
    }

    if (ifType == DiskInterfaceType::NVMe) return MediaType::NVMe;
    if (ifType == DiskInterfaceType::Virtual) return MediaType::Virtual;

    // Use IOCTL_STORAGE_QUERY_PROPERTY with StorageDeviceSeekPenaltyProperty
    // to determine if the device has no seek penalty (SSD) or has one (HDD).
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;

    DEVICE_SEEK_PENALTY_DESCRIPTOR seekDesc = {};
    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(diskHandle, IOCTL_STORAGE_QUERY_PROPERTY,
                                &query, sizeof(query),
                                &seekDesc, sizeof(seekDesc),
                                &bytesReturned, nullptr);

    if (ok && bytesReturned >= sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR))
    {
        return seekDesc.IncursSeekPenalty ? MediaType::HDD : MediaType::SSD;
    }

    // Fallback: unknown
    return MediaType::Unknown;
}

// ---------------------------------------------------------------------------
// Enumerate physical disks
// ---------------------------------------------------------------------------
Result<std::vector<DiskInfo>> DiskEnumerator::enumerateDisks()
{
    std::vector<DiskInfo> disks;

    // Strategy: Try SetupDiGetClassDevs with GUID_DEVINTERFACE_DISK first to get
    // device paths, then fall back to iterating PhysicalDrive0..31 for any disks
    // not found via SetupAPI.

    // Phase 1: SetupAPI enumeration
    DevInfoGuard devInfo(::SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_DISK,
        nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));

    std::vector<int> foundIndices;

    if (devInfo.isValid())
    {
        SP_DEVICE_INTERFACE_DATA ifData = {};
        ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        for (DWORD idx = 0;
             ::SetupDiEnumDeviceInterfaces(devInfo.get(), nullptr,
                                           &GUID_DEVINTERFACE_DISK, idx, &ifData);
             ++idx)
        {
            // Get required buffer size for the detail struct
            DWORD detailSize = 0;
            ::SetupDiGetDeviceInterfaceDetailW(devInfo.get(), &ifData,
                                               nullptr, 0, &detailSize, nullptr);
            if (detailSize == 0) continue;

            std::vector<uint8_t> detailBuf(detailSize, 0);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            SP_DEVINFO_DATA devInfoData = {};
            devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

            if (!::SetupDiGetDeviceInterfaceDetailW(devInfo.get(), &ifData,
                                                     detail, detailSize, nullptr, &devInfoData))
            {
                continue;
            }

            std::wstring devicePath = detail->DevicePath;

            // Open the device to query properties
            HANDLE hDisk = ::CreateFileW(
                devicePath.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr);

            if (hDisk == INVALID_HANDLE_VALUE) continue;

            // Get disk number to determine DiskId
            STORAGE_DEVICE_NUMBER deviceNumber = {};
            DWORD bytesReturned = 0;
            BOOL ok = ::DeviceIoControl(hDisk, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                        nullptr, 0,
                                        &deviceNumber, sizeof(deviceNumber),
                                        &bytesReturned, nullptr);

            if (!ok || deviceNumber.DeviceType != FILE_DEVICE_DISK)
            {
                ::CloseHandle(hDisk);
                continue;
            }

            DiskInfo info;
            info.id = static_cast<DiskId>(deviceNumber.DeviceNumber);
            info.devicePath = devicePath;

            // Get geometry
            uint8_t geomBuf[256] = {};
            ok = ::DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                                   nullptr, 0, geomBuf, sizeof(geomBuf),
                                   &bytesReturned, nullptr);
            if (ok)
            {
                const auto* geomEx = reinterpret_cast<const DISK_GEOMETRY_EX*>(geomBuf);
                info.sizeBytes = static_cast<uint64_t>(geomEx->DiskSize.QuadPart);
                info.sectorSize = geomEx->Geometry.BytesPerSector;
            }

            // Get model, serial, firmware, removable flag
            getStorageDescriptor(hDisk, info.model, info.serialNumber,
                                 info.firmwareRevision, info.isRemovable);

            // Get interface type
            info.interfaceType = getInterfaceType(hDisk);

            // Detect media type (SSD vs HDD)
            info.mediaType = detectMediaType(hDisk, info.interfaceType, info.isRemovable);

            // Get partition table type
            constexpr size_t kLayoutBufSize = sizeof(DRIVE_LAYOUT_INFORMATION_EX)
                                              + 128 * sizeof(PARTITION_INFORMATION_EX);
            std::vector<uint8_t> layoutBuf(kLayoutBufSize, 0);
            ok = ::DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                                   nullptr, 0,
                                   layoutBuf.data(), static_cast<DWORD>(layoutBuf.size()),
                                   &bytesReturned, nullptr);
            if (ok)
            {
                const auto* layout =
                    reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(layoutBuf.data());
                switch (layout->PartitionStyle)
                {
                case PARTITION_STYLE_MBR: info.partitionTableType = PartitionTableType::MBR; break;
                case PARTITION_STYLE_GPT: info.partitionTableType = PartitionTableType::GPT; break;
                default: info.partitionTableType = PartitionTableType::Unknown; break;
                }
            }

            ::CloseHandle(hDisk);

            foundIndices.push_back(info.id);
            disks.push_back(std::move(info));
        }
    }

    // Phase 2: Fallback — try PhysicalDrive0..31 for any we missed
    for (int driveIdx = 0; driveIdx < 32; ++driveIdx)
    {
        // Skip indices already found by SetupAPI
        if (std::find(foundIndices.begin(), foundIndices.end(), driveIdx) != foundIndices.end())
            continue;

        std::wostringstream pathStream;
        pathStream << L"\\\\.\\PhysicalDrive" << driveIdx;
        std::wstring drivePath = pathStream.str();

        HANDLE hDisk = ::CreateFileW(
            drivePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);

        if (hDisk == INVALID_HANDLE_VALUE) continue;

        DiskInfo info;
        info.id = driveIdx;
        info.devicePath = drivePath;

        uint8_t geomBuf[256] = {};
        DWORD bytesReturned = 0;
        BOOL ok = ::DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                                    nullptr, 0, geomBuf, sizeof(geomBuf),
                                    &bytesReturned, nullptr);
        if (ok)
        {
            const auto* geomEx = reinterpret_cast<const DISK_GEOMETRY_EX*>(geomBuf);
            info.sizeBytes = static_cast<uint64_t>(geomEx->DiskSize.QuadPart);
            info.sectorSize = geomEx->Geometry.BytesPerSector;
        }

        getStorageDescriptor(hDisk, info.model, info.serialNumber,
                             info.firmwareRevision, info.isRemovable);
        info.interfaceType = getInterfaceType(hDisk);
        info.mediaType = detectMediaType(hDisk, info.interfaceType, info.isRemovable);

        // Partition table type
        constexpr size_t kLayoutBufSize = sizeof(DRIVE_LAYOUT_INFORMATION_EX)
                                          + 128 * sizeof(PARTITION_INFORMATION_EX);
        std::vector<uint8_t> layoutBuf(kLayoutBufSize, 0);
        ok = ::DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                               nullptr, 0,
                               layoutBuf.data(), static_cast<DWORD>(layoutBuf.size()),
                               &bytesReturned, nullptr);
        if (ok)
        {
            const auto* layout =
                reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(layoutBuf.data());
            switch (layout->PartitionStyle)
            {
            case PARTITION_STYLE_MBR: info.partitionTableType = PartitionTableType::MBR; break;
            case PARTITION_STYLE_GPT: info.partitionTableType = PartitionTableType::GPT; break;
            default: info.partitionTableType = PartitionTableType::Unknown; break;
            }
        }

        ::CloseHandle(hDisk);
        disks.push_back(std::move(info));
    }

    // Sort by disk index for consistent ordering
    std::sort(disks.begin(), disks.end(),
              [](const DiskInfo& a, const DiskInfo& b) { return a.id < b.id; });

    return disks;
}

// ---------------------------------------------------------------------------
// Enumerate all volumes using FindFirstVolumeW / FindNextVolumeW
// ---------------------------------------------------------------------------
Result<std::vector<VolumeInfo>> DiskEnumerator::enumerateVolumes()
{
    std::vector<VolumeInfo> volumes;

    wchar_t volumeNameBuf[MAX_PATH] = {};
    HANDLE findHandle = ::FindFirstVolumeW(volumeNameBuf, MAX_PATH);
    if (findHandle == INVALID_HANDLE_VALUE)
    {
        return makeWin32Error(ErrorCode::DiskReadError, "FindFirstVolumeW failed");
    }

    do
    {
        VolumeInfo vol;
        vol.guidPath = volumeNameBuf;

        // Get mount points (drive letters and folder mounts)
        DWORD pathNamesLen = 0;
        // First call to get needed buffer size
        ::GetVolumePathNamesForVolumeNameW(volumeNameBuf, nullptr, 0, &pathNamesLen);

        if (pathNamesLen > 0)
        {
            std::vector<wchar_t> pathNames(pathNamesLen, L'\0');
            if (::GetVolumePathNamesForVolumeNameW(volumeNameBuf, pathNames.data(),
                                                    pathNamesLen, &pathNamesLen))
            {
                // The result is a multi-string: each path terminated by L'\0',
                // with an extra L'\0' at the end.
                const wchar_t* current = pathNames.data();
                while (*current != L'\0')
                {
                    vol.mountPoints.push_back(current);
                    current += wcslen(current) + 1;
                }
            }
        }

        // Get filesystem info using the volume GUID path (needs trailing backslash)
        std::wstring rootPath = volumeNameBuf; // Already has trailing backslash from FindFirstVolumeW
        wchar_t fsLabel[MAX_PATH + 1] = {};
        wchar_t fsName[MAX_PATH + 1] = {};
        DWORD serialNumber = 0;
        DWORD maxComponentLen = 0;
        DWORD fsFlags = 0;

        if (::GetVolumeInformationW(rootPath.c_str(),
                                     fsLabel, MAX_PATH,
                                     &serialNumber,
                                     &maxComponentLen,
                                     &fsFlags,
                                     fsName, MAX_PATH))
        {
            vol.filesystemLabel = fsLabel;
            vol.filesystemName = fsName;
        }

        // Get total and free space
        ULARGE_INTEGER freeBytesAvail = {};
        ULARGE_INTEGER totalBytes = {};
        ULARGE_INTEGER totalFreeBytes = {};

        if (::GetDiskFreeSpaceExW(rootPath.c_str(),
                                   &freeBytesAvail, &totalBytes, &totalFreeBytes))
        {
            vol.totalBytes = totalBytes.QuadPart;
            vol.freeBytes = totalFreeBytes.QuadPart;
        }

        volumes.push_back(std::move(vol));

    } while (::FindNextVolumeW(findHandle, volumeNameBuf, MAX_PATH));

    ::FindVolumeClose(findHandle);

    return volumes;
}

// ---------------------------------------------------------------------------
// WMI helper: extract a string property from a WMI object
// ---------------------------------------------------------------------------
static std::wstring getWmiString(IWbemClassObject* obj, const wchar_t* propName)
{
    VARIANT vtProp;
    ::VariantInit(&vtProp);

    HRESULT hr = obj->Get(propName, 0, &vtProp, nullptr, nullptr);
    if (FAILED(hr) || vtProp.vt == VT_NULL)
    {
        ::VariantClear(&vtProp);
        return L"";
    }

    std::wstring result;
    if (vtProp.vt == VT_BSTR && vtProp.bstrVal)
    {
        result = vtProp.bstrVal;
    }
    ::VariantClear(&vtProp);
    return result;
}

static uint64_t getWmiUint64(IWbemClassObject* obj, const wchar_t* propName)
{
    VARIANT vtProp;
    ::VariantInit(&vtProp);

    HRESULT hr = obj->Get(propName, 0, &vtProp, nullptr, nullptr);
    if (FAILED(hr) || vtProp.vt == VT_NULL)
    {
        ::VariantClear(&vtProp);
        return 0;
    }

    uint64_t result = 0;
    if (vtProp.vt == VT_BSTR && vtProp.bstrVal)
    {
        // WMI returns large integers as strings
        result = _wcstoui64(vtProp.bstrVal, nullptr, 10);
    }
    else if (vtProp.vt == VT_I4 || vtProp.vt == VT_UI4)
    {
        result = static_cast<uint64_t>(vtProp.ulVal);
    }
    ::VariantClear(&vtProp);
    return result;
}

static uint32_t getWmiUint32(IWbemClassObject* obj, const wchar_t* propName)
{
    return static_cast<uint32_t>(getWmiUint64(obj, propName));
}

static bool getWmiBool(IWbemClassObject* obj, const wchar_t* propName)
{
    VARIANT vtProp;
    ::VariantInit(&vtProp);

    HRESULT hr = obj->Get(propName, 0, &vtProp, nullptr, nullptr);
    if (FAILED(hr) || vtProp.vt == VT_NULL)
    {
        ::VariantClear(&vtProp);
        return false;
    }

    bool result = false;
    if (vtProp.vt == VT_BOOL)
    {
        result = (vtProp.boolVal != VARIANT_FALSE);
    }
    ::VariantClear(&vtProp);
    return result;
}

// ---------------------------------------------------------------------------
// Use WMI to enumerate partitions with full disk->partition->volume mapping.
// This is the most reliable way to get the partition-to-drive-letter mapping.
// ---------------------------------------------------------------------------
Result<std::vector<PartitionInfo>> DiskEnumerator::enumeratePartitionsWmi()
{
    ComInitGuard comGuard;
    if (!comGuard.succeeded() && comGuard.result() != RPC_E_CHANGED_MODE)
    {
        return makeHResultError(ErrorCode::WmiQueryFailed, comGuard.result(),
            "COM initialization failed");
    }

    // Set COM security. If already set, S_FALSE or RPC_E_TOO_LATE is acceptable.
    HRESULT hr = ::CoInitializeSecurity(
        nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE, nullptr);

    if (FAILED(hr) && hr != RPC_E_TOO_LATE)
    {
        // Non-fatal: proceed anyway, some queries may still work
    }

    // Connect to WMI
    IWbemLocator* pLocator = nullptr;
    hr = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                            IID_IWbemLocator, reinterpret_cast<void**>(&pLocator));
    if (FAILED(hr))
    {
        return makeHResultError(ErrorCode::WmiQueryFailed, hr,
            "Failed to create WMI locator");
    }

    IWbemServices* pServices = nullptr;
    hr = pLocator->ConnectServer(
        _bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &pServices);
    if (FAILED(hr))
    {
        pLocator->Release();
        return makeHResultError(ErrorCode::WmiQueryFailed, hr,
            "Failed to connect to WMI ROOT\\CIMV2");
    }

    // Set proxy security on the WMI connection
    hr = ::CoSetProxyBlanket(pServices,
        RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE);

    std::vector<PartitionInfo> partitions;

    // Step 1: Query Win32_DiskPartition for partition details
    IEnumWbemClassObject* pPartEnum = nullptr;
    hr = pServices->ExecQuery(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT * FROM Win32_DiskPartition"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &pPartEnum);

    if (SUCCEEDED(hr))
    {
        IWbemClassObject* pObj = nullptr;
        ULONG numReturned = 0;

        while (pPartEnum->Next(WBEM_INFINITE, 1, &pObj, &numReturned) == S_OK)
        {
            PartitionInfo pi;

            pi.diskId = static_cast<DiskId>(getWmiUint32(pObj, L"DiskIndex"));
            pi.index = static_cast<PartitionId>(getWmiUint32(pObj, L"Index"));
            pi.offsetBytes = getWmiUint64(pObj, L"StartingOffset");
            pi.sizeBytes = getWmiUint64(pObj, L"Size");
            pi.isBootable = getWmiBool(pObj, L"Bootable");
            pi.isActive = getWmiBool(pObj, L"BootPartition");

            std::wstring partType = getWmiString(pObj, L"Type");
            // WMI Type field has format like "GPT: Basic Data" or "Installable File System"
            if (partType.find(L"GPT") != std::wstring::npos)
            {
                // GPT partition — type string varies; we will get the actual GUID from
                // IOCTL_DISK_GET_DRIVE_LAYOUT_EX, but the WMI type gives us hints
            }

            std::wstring deviceId = getWmiString(pObj, L"DeviceID");
            // DeviceID is like "Disk #0, Partition #1"

            partitions.push_back(std::move(pi));
            pObj->Release();
        }

        pPartEnum->Release();
    }

    // Step 2: Query Win32_LogicalDiskToPartition for drive letter mapping.
    // This WMI associator maps "Win32_DiskPartition.DeviceID" to "Win32_LogicalDisk.DeviceID".
    IEnumWbemClassObject* pAssocEnum = nullptr;
    hr = pServices->ExecQuery(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT * FROM Win32_LogicalDiskToPartition"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &pAssocEnum);

    if (SUCCEEDED(hr))
    {
        IWbemClassObject* pObj = nullptr;
        ULONG numReturned = 0;

        while (pAssocEnum->Next(WBEM_INFINITE, 1, &pObj, &numReturned) == S_OK)
        {
            // Antecedent is the partition, Dependent is the logical disk
            std::wstring antecedent = getWmiString(pObj, L"Antecedent");
            std::wstring dependent = getWmiString(pObj, L"Dependent");

            // Parse disk index and partition index from the Antecedent string.
            // Format: \\HOSTNAME\root\cimv2:Win32_DiskPartition.DeviceID="Disk #0, Partition #1"
            int diskIdx = -1, partIdx = -1;
            auto diskPos = antecedent.find(L"Disk #");
            auto partPos = antecedent.find(L"Partition #");
            if (diskPos != std::wstring::npos)
                diskIdx = _wtoi(antecedent.c_str() + diskPos + 6);
            if (partPos != std::wstring::npos)
                partIdx = _wtoi(antecedent.c_str() + partPos + 11);

            // Parse drive letter from Dependent.
            // Format: \\HOSTNAME\root\cimv2:Win32_LogicalDisk.DeviceID="C:"
            wchar_t driveLetter = L'\0';
            auto quotePos = dependent.rfind(L'"');
            if (quotePos != std::wstring::npos && quotePos >= 2)
            {
                // The character before the last quote should be ':'
                if (dependent[quotePos - 1] == L':')
                    driveLetter = dependent[quotePos - 2];
            }

            // Match to our partition list
            if (diskIdx >= 0 && partIdx >= 0 && driveLetter != L'\0')
            {
                for (auto& part : partitions)
                {
                    if (part.diskId == diskIdx && part.index == partIdx)
                    {
                        part.driveLetter = driveLetter;

                        // Also look up the volume GUID path for this drive letter
                        wchar_t rootPath[] = L"X:\\";
                        rootPath[0] = driveLetter;
                        wchar_t guidBuf[MAX_PATH] = {};
                        if (::GetVolumeNameForVolumeMountPointW(rootPath, guidBuf, MAX_PATH))
                        {
                            part.volumeGuidPath = guidBuf;
                        }

                        // Get filesystem label and type
                        wchar_t labelBuf[MAX_PATH + 1] = {};
                        wchar_t fsBuf[MAX_PATH + 1] = {};
                        if (::GetVolumeInformationW(rootPath, labelBuf, MAX_PATH,
                                                     nullptr, nullptr, nullptr,
                                                     fsBuf, MAX_PATH))
                        {
                            part.label = labelBuf;
                            part.filesystemType = classifyFilesystem(fsBuf);
                        }

                        break;
                    }
                }
            }

            pObj->Release();
        }

        pAssocEnum->Release();
    }

    pServices->Release();
    pLocator->Release();

    return partitions;
}

// ---------------------------------------------------------------------------
// Full system snapshot
// ---------------------------------------------------------------------------
Result<SystemDiskSnapshot> DiskEnumerator::getSystemSnapshot()
{
    SystemDiskSnapshot snapshot;

    auto disksResult = enumerateDisks();
    if (disksResult.isError()) return disksResult.error();
    snapshot.disks = std::move(disksResult.value());

    auto volumesResult = enumerateVolumes();
    if (volumesResult.isError()) return volumesResult.error();
    snapshot.volumes = std::move(volumesResult.value());

    auto partitionsResult = enumeratePartitionsWmi();
    if (partitionsResult.isError()) return partitionsResult.error();
    snapshot.partitions = std::move(partitionsResult.value());

    return snapshot;
}

// ---------------------------------------------------------------------------
// Get info for a single disk
// ---------------------------------------------------------------------------
Result<DiskInfo> DiskEnumerator::getDiskInfo(DiskId diskIndex)
{
    auto allDisks = enumerateDisks();
    if (allDisks.isError()) return allDisks.error();

    for (auto& disk : allDisks.value())
    {
        if (disk.id == diskIndex)
            return std::move(disk);
    }

    return ErrorInfo::fromCode(ErrorCode::DiskNotFound, "Physical disk not found");
}

// ---------------------------------------------------------------------------
// Classify interface type from WMI string
// ---------------------------------------------------------------------------
DiskInterfaceType DiskEnumerator::classifyInterfaceType(const std::wstring& wmiInterfaceType)
{
    if (wmiInterfaceType == L"IDE" || wmiInterfaceType == L"ATA")
        return DiskInterfaceType::IDE;
    if (wmiInterfaceType == L"SCSI")
        return DiskInterfaceType::SCSI;
    if (wmiInterfaceType == L"USB")
        return DiskInterfaceType::USB;
    if (wmiInterfaceType == L"1394")
        return DiskInterfaceType::Firewire;
    if (wmiInterfaceType == L"SAS")
        return DiskInterfaceType::SAS;
    return DiskInterfaceType::Unknown;
}

// ---------------------------------------------------------------------------
// Classify media type from WMI and interface hints
// ---------------------------------------------------------------------------
MediaType DiskEnumerator::classifyMediaType(const std::wstring& wmiMediaType,
                                             DiskInterfaceType ifType)
{
    if (ifType == DiskInterfaceType::NVMe) return MediaType::NVMe;
    if (ifType == DiskInterfaceType::USB) return MediaType::USBFlash;
    if (ifType == DiskInterfaceType::MMC) return MediaType::SDCard;

    if (wmiMediaType.find(L"Fixed") != std::wstring::npos) return MediaType::HDD;
    if (wmiMediaType.find(L"Removable") != std::wstring::npos) return MediaType::USBFlash;
    if (wmiMediaType.find(L"External") != std::wstring::npos) return MediaType::HDD;

    return MediaType::Unknown;
}

// ---------------------------------------------------------------------------
// Classify filesystem name string to enum
// ---------------------------------------------------------------------------
FilesystemType DiskEnumerator::classifyFilesystem(const std::wstring& fsName)
{
    if (fsName == L"NTFS")      return FilesystemType::NTFS;
    if (fsName == L"FAT32")     return FilesystemType::FAT32;
    if (fsName == L"FAT16")     return FilesystemType::FAT16;
    if (fsName == L"FAT12")     return FilesystemType::FAT12;
    if (fsName == L"FAT")       return FilesystemType::FAT16; // Windows reports FAT for FAT12/16
    if (fsName == L"exFAT")     return FilesystemType::ExFAT;
    if (fsName == L"ReFS")      return FilesystemType::ReFS;
    if (fsName == L"UDF")       return FilesystemType::UDF;
    if (fsName == L"CDFS")      return FilesystemType::ISO9660;
    if (fsName == L"ext2")      return FilesystemType::Ext2;
    if (fsName == L"ext3")      return FilesystemType::Ext3;
    if (fsName == L"ext4")      return FilesystemType::Ext4;
    if (fsName == L"Btrfs")     return FilesystemType::Btrfs;
    if (fsName == L"HPFS")      return FilesystemType::HPFS;
    return FilesystemType::Unknown;
}

} // namespace spw
