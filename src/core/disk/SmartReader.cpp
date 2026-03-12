#include "SmartReader.h"

#include <winioctl.h>
#include <ntddscsi.h>
// For NVMe: StorageAdapterProtocolSpecificProperty and STORAGE_PROTOCOL_SPECIFIC_DATA
// are available in ntddstor.h on Windows 10+.
#include <ntddstor.h>

#include <cstring>
#include <sstream>

// Link against required libraries
#pragma comment(lib, "kernel32.lib")

namespace spw
{

// ---------------------------------------------------------------------------
// ATA command constants for S.M.A.R.T.
// Reference: ATA/ATAPI Command Set (ACS-3), Section 7.51
// ---------------------------------------------------------------------------
static constexpr uint8_t ATA_SMART_CMD             = 0xB0;
static constexpr uint8_t ATA_SMART_READ_DATA       = 0xD0;
static constexpr uint8_t ATA_SMART_READ_THRESHOLDS = 0xD1;
static constexpr uint8_t ATA_SMART_ENABLE          = 0xD8;
static constexpr uint8_t ATA_SMART_LBA_MID         = 0x4F;
static constexpr uint8_t ATA_SMART_LBA_HI          = 0xC2;

// S.M.A.R.T. data sector is always 512 bytes
static constexpr uint32_t SMART_DATA_SIZE = 512;

// Each ATA S.M.A.R.T. attribute entry is 12 bytes, starting at offset 2 in the data sector.
// There can be up to 30 attributes.
static constexpr int SMART_ATTR_ENTRY_SIZE = 12;
static constexpr int SMART_ATTR_START_OFFSET = 2;
static constexpr int SMART_MAX_ATTRS = 30;

// Threshold entries are also 12 bytes each, starting at offset 2 in the threshold sector.
static constexpr int SMART_THRESH_ENTRY_SIZE = 12;
static constexpr int SMART_THRESH_START_OFFSET = 2;

// ---------------------------------------------------------------------------
// Helper: build Win32 error
// ---------------------------------------------------------------------------
static ErrorInfo makeWin32Error(ErrorCode code, const std::string& context)
{
    const DWORD lastErr = ::GetLastError();
    std::ostringstream oss;
    oss << context << " (Win32 error " << lastErr << ")";
    return ErrorInfo::fromWin32(code, lastErr, oss.str());
}

// ---------------------------------------------------------------------------
// ATA PASS-THROUGH structure for 28-bit commands.
// We use ATA_PASS_THROUGH_EX which is available on all modern Windows.
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct AtaSmartReadCmd
{
    ATA_PASS_THROUGH_EX header;
    uint8_t dataBuffer[SMART_DATA_SIZE];
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Send an ATA S.M.A.R.T. command and receive 512 bytes of data back.
// ---------------------------------------------------------------------------
static Result<std::vector<uint8_t>> sendAtaSmartRead(HANDLE diskHandle, uint8_t feature)
{
    AtaSmartReadCmd cmd = {};

    cmd.header.Length = sizeof(ATA_PASS_THROUGH_EX);
    cmd.header.AtaFlags = ATA_FLAGS_DATA_IN | ATA_FLAGS_DRDY_REQUIRED;
    cmd.header.DataTransferLength = SMART_DATA_SIZE;
    cmd.header.TimeOutValue = 10; // seconds
    // DataBufferOffset is the offset from the start of the structure to the data buffer
    cmd.header.DataBufferOffset = offsetof(AtaSmartReadCmd, dataBuffer);

    // Set up the ATA task file registers for S.M.A.R.T. READ DATA
    auto& tf = cmd.header.CurrentTaskFile;
    tf[0] = feature;              // Feature register (0xD0 = read data, 0xD1 = read thresholds)
    tf[1] = 0;                    // Sector Count
    tf[2] = 0;                    // Sector Number (LBA low)
    tf[3] = ATA_SMART_LBA_MID;   // Cylinder Low  (LBA mid)  = 0x4F
    tf[4] = ATA_SMART_LBA_HI;    // Cylinder High (LBA high) = 0xC2
    tf[5] = 0xA0;                 // Device/Head (master)
    tf[6] = ATA_SMART_CMD;        // Command register = 0xB0

    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(
        diskHandle,
        IOCTL_ATA_PASS_THROUGH,
        &cmd, sizeof(cmd),
        &cmd, sizeof(cmd),
        &bytesReturned, nullptr);

    if (!ok)
    {
        return makeWin32Error(ErrorCode::SmartReadFailed,
            "IOCTL_ATA_PASS_THROUGH failed for SMART command");
    }

    // Check the status register in the returned task file
    // Bit 0 (ERR) indicates an error
    if (cmd.header.CurrentTaskFile[6] & 0x01)
    {
        return ErrorInfo::fromCode(ErrorCode::SmartReadFailed,
            "ATA SMART command returned error in status register");
    }

    std::vector<uint8_t> result(SMART_DATA_SIZE);
    std::memcpy(result.data(), cmd.dataBuffer, SMART_DATA_SIZE);
    return result;
}

// ---------------------------------------------------------------------------
// Parse ATA S.M.A.R.T. attributes from the 512-byte data sector.
//
// Data layout (ATA Spec):
//   Offset 0:   Revision number (2 bytes)
//   Offset 2:   Attribute entries, 12 bytes each, up to 30 entries
//   Offset 362: Reserved
//
// Each 12-byte attribute entry:
//   Byte 0:     Attribute ID
//   Byte 1-2:   Status flags
//   Byte 3:     Current value (normalized, 1-253)
//   Byte 4:     Worst value
//   Byte 5-10:  Raw value (6 bytes, little-endian)
//   Byte 11:    Reserved
// ---------------------------------------------------------------------------
static std::vector<SmartAttribute> parseAtaAttributes(const uint8_t* data)
{
    std::vector<SmartAttribute> attrs;

    for (int i = 0; i < SMART_MAX_ATTRS; ++i)
    {
        int offset = SMART_ATTR_START_OFFSET + (i * SMART_ATTR_ENTRY_SIZE);

        uint8_t attrId = data[offset];
        if (attrId == 0) continue; // Empty slot

        SmartAttribute attr;
        attr.id = attrId;
        attr.name = SmartReader::getAttributeName(attrId);
        attr.currentValue = data[offset + 3];
        attr.worstValue = data[offset + 4];

        // Raw value: 6 bytes little-endian starting at offset+5
        attr.rawValue = 0;
        for (int b = 5; b >= 0; --b)
        {
            attr.rawValue = (attr.rawValue << 8) | data[offset + 5 + b];
        }

        attrs.push_back(attr);
    }

    return attrs;
}

// ---------------------------------------------------------------------------
// Parse S.M.A.R.T. thresholds from the 512-byte threshold sector.
//
// Threshold layout:
//   Offset 0:   Revision number (2 bytes)
//   Offset 2:   Threshold entries, 12 bytes each
//
// Each 12-byte threshold entry:
//   Byte 0:     Attribute ID
//   Byte 1:     Threshold value
//   Byte 2-11:  Reserved
// ---------------------------------------------------------------------------
static std::vector<std::pair<uint8_t, uint8_t>> parseAtaThresholds(const uint8_t* data)
{
    std::vector<std::pair<uint8_t, uint8_t>> thresholds;

    for (int i = 0; i < SMART_MAX_ATTRS; ++i)
    {
        int offset = SMART_THRESH_START_OFFSET + (i * SMART_THRESH_ENTRY_SIZE);

        uint8_t attrId = data[offset];
        if (attrId == 0) continue;

        uint8_t threshold = data[offset + 1];
        thresholds.emplace_back(attrId, threshold);
    }

    return thresholds;
}

// ---------------------------------------------------------------------------
// Read ATA S.M.A.R.T. data
// ---------------------------------------------------------------------------
Result<SmartData> SmartReader::readAtaSmart(HANDLE diskHandle, DiskId diskId)
{
    // Read the S.M.A.R.T. data sector (feature 0xD0)
    auto dataResult = sendAtaSmartRead(diskHandle, ATA_SMART_READ_DATA);
    if (dataResult.isError())
    {
        return dataResult.error();
    }

    const auto& dataSector = dataResult.value();

    // Parse attributes
    auto attributes = parseAtaAttributes(dataSector.data());

    // Read thresholds (feature 0xD1)
    auto threshResult = sendAtaSmartRead(diskHandle, ATA_SMART_READ_THRESHOLDS);
    if (threshResult.isOk())
    {
        auto thresholds = parseAtaThresholds(threshResult.value().data());

        // Merge thresholds into attributes
        for (auto& attr : attributes)
        {
            for (const auto& [threshId, threshVal] : thresholds)
            {
                if (threshId == attr.id)
                {
                    attr.threshold = threshVal;
                    attr.status = evaluateAttributeHealth(
                        attr.currentValue, attr.worstValue, attr.threshold);
                    break;
                }
            }

            // If no threshold was found, mark as OK if value looks healthy
            if (attr.status == SmartStatus::Unknown)
            {
                attr.status = (attr.currentValue > 0) ? SmartStatus::OK : SmartStatus::Unknown;
            }
        }
    }
    else
    {
        // Thresholds not available — mark all attributes as Unknown status
        for (auto& attr : attributes)
        {
            attr.status = SmartStatus::Unknown;
        }
    }

    SmartData result;
    result.diskId = diskId;
    result.isNvme = false;
    result.attributes = std::move(attributes);
    result.overallHealth = evaluateOverallHealth(result.attributes);
    return result;
}

// ---------------------------------------------------------------------------
// Read NVMe S.M.A.R.T. / Health Information Log
//
// We use IOCTL_STORAGE_QUERY_PROPERTY with:
//   PropertyId = StorageDeviceProtocolSpecificProperty (50)
//   QueryType  = PropertyStandardQuery
//   ProtocolType = ProtocolTypeNvme
//   DataType   = NVMeDataTypeLogPage (2)
//   ProtocolDataRequestValue = NVME_LOG_PAGE_HEALTH_INFO (0x02)
//
// The NVMe SMART/Health Information log (NVMe spec 1.4, Figure 93) is 512 bytes.
// ---------------------------------------------------------------------------
Result<SmartData> SmartReader::readNvmeSmart(HANDLE diskHandle, DiskId diskId)
{
    // Build the query buffer. The layout is:
    //   STORAGE_PROPERTY_QUERY (header)
    //     -> AdditionalParameters contains STORAGE_PROTOCOL_SPECIFIC_DATA
    //   followed by space for the returned NVMe log page data (512 bytes)
    constexpr DWORD kNvmeHealthLogSize = 512;
    constexpr DWORD kQueryBufSize = FIELD_OFFSET(STORAGE_PROPERTY_QUERY, AdditionalParameters)
                                    + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA)
                                    + kNvmeHealthLogSize;

    std::vector<uint8_t> queryBuf(kQueryBufSize, 0);
    auto* query = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(queryBuf.data());
    query->PropertyId = StorageDeviceProtocolSpecificProperty;
    query->QueryType = PropertyStandardQuery;

    auto* protocolData = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(
        query->AdditionalParameters);
    protocolData->ProtocolType = ProtocolTypeNvme;
    protocolData->DataType = NVMeDataTypeLogPage;
    protocolData->ProtocolDataRequestValue = NVME_LOG_PAGE_HEALTH_INFO; // 0x02
    protocolData->ProtocolDataRequestSubValue = 0;
    protocolData->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    protocolData->ProtocolDataLength = kNvmeHealthLogSize;

    // Output buffer: STORAGE_PROTOCOL_DATA_DESCRIPTOR + log page data
    constexpr DWORD kOutputBufSize = FIELD_OFFSET(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData)
                                     + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA)
                                     + kNvmeHealthLogSize;
    std::vector<uint8_t> outputBuf(kOutputBufSize, 0);

    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(
        diskHandle,
        IOCTL_STORAGE_QUERY_PROPERTY,
        queryBuf.data(), kQueryBufSize,
        outputBuf.data(), kOutputBufSize,
        &bytesReturned, nullptr);

    if (!ok)
    {
        return makeWin32Error(ErrorCode::SmartReadFailed,
            "IOCTL_STORAGE_QUERY_PROPERTY failed for NVMe SMART log");
    }

    // Parse the returned data descriptor
    auto* descriptor = reinterpret_cast<STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(outputBuf.data());
    auto* returnedProtocol = &descriptor->ProtocolSpecificData;

    if (returnedProtocol->ProtocolDataLength < kNvmeHealthLogSize)
    {
        return ErrorInfo::fromCode(ErrorCode::SmartReadFailed,
            "NVMe SMART log page returned insufficient data");
    }

    // The log page data starts at ProtocolSpecificData offset + ProtocolDataOffset
    const uint8_t* logData = outputBuf.data()
        + FIELD_OFFSET(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData)
        + returnedProtocol->ProtocolDataOffset;

    // Parse NVMe SMART/Health Information Log (NVMe spec 1.4, Figure 93):
    //   Byte 0:      Critical Warning
    //   Byte 1-2:    Composite Temperature (Kelvin)
    //   Byte 3:      Available Spare (%)
    //   Byte 4:      Available Spare Threshold (%)
    //   Byte 5:      Percentage Used
    //   Byte 6-31:   Reserved
    //   Byte 32-47:  Data Units Read (128-bit, in units of 1000 x 512 bytes)
    //   Byte 48-63:  Data Units Written
    //   Byte 64-79:  Host Read Commands
    //   Byte 80-95:  Host Write Commands
    //   Byte 96-111: Controller Busy Time (minutes)
    //   Byte 112-127: Power Cycles
    //   Byte 128-143: Power On Hours
    //   Byte 144-159: Unsafe Shutdowns
    //   Byte 160-175: Media and Data Integrity Errors
    //   Byte 176-191: Number of Error Information Log Entries

    NvmeHealthInfo health = {};
    health.criticalWarning = logData[0];
    health.temperature = static_cast<uint16_t>(logData[1]) |
                         (static_cast<uint16_t>(logData[2]) << 8);
    health.availableSpare = logData[3];
    health.availableSpareThreshold = logData[4];
    health.percentageUsed = logData[5];

    // Helper lambda to read low 64 bits of a 128-bit little-endian value
    auto readLow64 = [&logData](int offset) -> uint64_t {
        uint64_t val = 0;
        for (int i = 7; i >= 0; --i)
        {
            val = (val << 8) | logData[offset + i];
        }
        return val;
    };

    health.dataUnitsRead       = readLow64(32);
    health.dataUnitsWritten    = readLow64(48);
    health.hostReadCommands    = readLow64(64);
    health.hostWriteCommands   = readLow64(80);
    health.controllerBusyTime  = readLow64(96);
    health.powerCycles         = readLow64(112);
    health.powerOnHours        = readLow64(128);
    health.unsafeShutdowns     = readLow64(144);
    health.mediaErrors         = readLow64(160);
    health.errorLogEntries     = readLow64(176);

    SmartData result;
    result.diskId = diskId;
    result.isNvme = true;
    result.nvmeHealth = health;
    result.overallHealth = evaluateNvmeHealth(health);
    return result;
}

// ---------------------------------------------------------------------------
// Detect if a disk is NVMe using IOCTL_STORAGE_QUERY_PROPERTY
// ---------------------------------------------------------------------------
Result<bool> SmartReader::isNvmeDrive(HANDLE diskHandle)
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
    if (!ok || header.Size == 0)
    {
        return false;  // Can't determine, assume not NVMe
    }

    std::vector<uint8_t> buffer(header.Size, 0);
    ok = ::DeviceIoControl(diskHandle, IOCTL_STORAGE_QUERY_PROPERTY,
                           &query, sizeof(query),
                           buffer.data(), static_cast<DWORD>(buffer.size()),
                           &bytesReturned, nullptr);
    if (!ok) return false;

    const auto* desc = reinterpret_cast<const STORAGE_ADAPTER_DESCRIPTOR*>(buffer.data());
    return (desc->BusType == BusTypeNvme);
}

// ---------------------------------------------------------------------------
// Auto-detect ATA vs NVMe and read S.M.A.R.T.
// ---------------------------------------------------------------------------
Result<SmartData> SmartReader::readSmartData(HANDLE diskHandle, DiskId diskId)
{
    auto nvmeResult = isNvmeDrive(diskHandle);

    bool nvme = false;
    if (nvmeResult.isOk())
        nvme = nvmeResult.value();

    if (nvme)
    {
        return readNvmeSmart(diskHandle, diskId);
    }
    else
    {
        return readAtaSmart(diskHandle, diskId);
    }
}

// ---------------------------------------------------------------------------
// Read ATA thresholds (public API)
// ---------------------------------------------------------------------------
Result<std::vector<std::pair<uint8_t, uint8_t>>> SmartReader::readAtaSmartThresholds(HANDLE diskHandle)
{
    auto result = sendAtaSmartRead(diskHandle, ATA_SMART_READ_THRESHOLDS);
    if (result.isError()) return result.error();
    return parseAtaThresholds(result.value().data());
}

// ---------------------------------------------------------------------------
// Attribute name lookup table.
// Standard S.M.A.R.T. attribute IDs are defined by the ATA spec and individual
// drive manufacturers. This covers the most common/important ones.
// ---------------------------------------------------------------------------
const char* SmartReader::getAttributeName(uint8_t attributeId)
{
    switch (attributeId)
    {
    case 1:   return "Raw Read Error Rate";
    case 2:   return "Throughput Performance";
    case 3:   return "Spin-Up Time";
    case 4:   return "Start/Stop Count";
    case 5:   return "Reallocated Sectors Count";
    case 6:   return "Read Channel Margin";
    case 7:   return "Seek Error Rate";
    case 8:   return "Seek Time Performance";
    case 9:   return "Power-On Hours";
    case 10:  return "Spin Retry Count";
    case 11:  return "Recalibration Retries";
    case 12:  return "Power Cycle Count";
    case 13:  return "Soft Read Error Rate";
    case 170: return "Available Reserved Space";
    case 171: return "SSD Program Fail Count";
    case 172: return "SSD Erase Fail Count";
    case 173: return "SSD Wear Leveling Count";
    case 174: return "Unexpected Power Loss Count";
    case 175: return "Power Loss Protection Failure";
    case 176: return "Erase Fail Count (chip)";
    case 177: return "Wear Range Delta";
    case 178: return "Used Reserved Block Count (chip)";
    case 179: return "Used Reserved Block Count (total)";
    case 180: return "Unused Reserved Block Count (total)";
    case 181: return "Program Fail Count (total)";
    case 182: return "Erase Fail Count (total)";
    case 183: return "Runtime Bad Block";
    case 184: return "End-to-End Error";
    case 187: return "Reported Uncorrectable Errors";
    case 188: return "Command Timeout";
    case 189: return "High Fly Writes";
    case 190: return "Airflow Temperature";
    case 191: return "G-Sense Error Rate";
    case 192: return "Power-Off Retract Count";
    case 193: return "Load/Unload Cycle Count";
    case 194: return "Temperature";
    case 195: return "Hardware ECC Recovered";
    case 196: return "Reallocation Event Count";
    case 197: return "Current Pending Sector Count";
    case 198: return "Offline Uncorrectable Sector Count";
    case 199: return "Ultra DMA CRC Error Count";
    case 200: return "Multi-Zone Error Rate";
    case 201: return "Soft Read Error Rate";
    case 202: return "Data Address Mark Errors";
    case 203: return "Run Out Cancel";
    case 204: return "Soft ECC Correction";
    case 205: return "Thermal Asperity Rate";
    case 206: return "Flying Height";
    case 207: return "Spin High Current";
    case 208: return "Spin Buzz";
    case 209: return "Offline Seek Performance";
    case 220: return "Disk Shift";
    case 221: return "G-Sense Error Rate";
    case 222: return "Loaded Hours";
    case 223: return "Load/Unload Retry Count";
    case 224: return "Load Friction";
    case 225: return "Load/Unload Cycle Count";
    case 226: return "Load-In Time";
    case 227: return "Torque Amplification Count";
    case 228: return "Power-Off Retract Cycle";
    case 230: return "GMR Head Amplitude";
    case 231: return "Life Left (SSD)";
    case 232: return "Endurance Remaining";
    case 233: return "Media Wearout Indicator";
    case 234: return "Average Erase Count";
    case 235: return "Good Block Count / System Free Block Count";
    case 240: return "Head Flying Hours";
    case 241: return "Total LBAs Written";
    case 242: return "Total LBAs Read";
    case 243: return "Total LBAs Written Expanded";
    case 244: return "Total LBAs Read Expanded";
    case 249: return "NAND Writes (1 GiB)";
    case 250: return "Read Error Retry Rate";
    case 251: return "Minimum Spares Remaining";
    case 252: return "Newly Added Bad Flash Block";
    case 254: return "Free Fall Protection";
    default:  return "Unknown Attribute";
    }
}

// ---------------------------------------------------------------------------
// Evaluate health of a single attribute
// ---------------------------------------------------------------------------
SmartStatus SmartReader::evaluateAttributeHealth(uint8_t currentValue, uint8_t worstValue,
                                                  uint8_t threshold)
{
    if (threshold == 0)
    {
        // Threshold of 0 means "always passing" per ATA spec
        return SmartStatus::OK;
    }

    // Critical: current value is at or below threshold
    if (currentValue <= threshold)
    {
        return SmartStatus::Critical;
    }

    // Warning: worst value has been at or below threshold, or current is close
    if (worstValue <= threshold)
    {
        return SmartStatus::Warning;
    }

    // Warning: within 10% of threshold (approaching failure)
    if (threshold > 0 && currentValue < static_cast<uint16_t>(threshold) + 10)
    {
        return SmartStatus::Warning;
    }

    return SmartStatus::OK;
}

// ---------------------------------------------------------------------------
// Overall health from all ATA attributes.
// Any Critical attribute makes the overall status Critical.
// Any Warning without Critical makes it Warning.
// ---------------------------------------------------------------------------
SmartStatus SmartReader::evaluateOverallHealth(const std::vector<SmartAttribute>& attributes)
{
    bool hasWarning = false;
    bool hasUnknown = false;

    for (const auto& attr : attributes)
    {
        switch (attr.status)
        {
        case SmartStatus::Critical:
            return SmartStatus::Critical;
        case SmartStatus::Warning:
            hasWarning = true;
            break;
        case SmartStatus::Unknown:
            hasUnknown = true;
            break;
        default:
            break;
        }
    }

    if (hasWarning) return SmartStatus::Warning;
    if (attributes.empty() || hasUnknown) return SmartStatus::Unknown;
    return SmartStatus::OK;
}

// ---------------------------------------------------------------------------
// NVMe overall health evaluation
// ---------------------------------------------------------------------------
SmartStatus SmartReader::evaluateNvmeHealth(const NvmeHealthInfo& health)
{
    // Critical Warning byte: any bit set indicates a problem
    // Bit 0: Available spare below threshold
    // Bit 1: Temperature above or below threshold
    // Bit 2: NVM subsystem reliability degraded
    // Bit 3: Media placed in read-only mode
    // Bit 4: Volatile memory backup device has failed
    if (health.criticalWarning != 0)
    {
        // Bit 2 (reliability) or bit 3 (read-only) are critical
        if (health.criticalWarning & 0x0C)
            return SmartStatus::Critical;
        return SmartStatus::Warning;
    }

    // Available spare below threshold
    if (health.availableSpare > 0 && health.availableSpareThreshold > 0 &&
        health.availableSpare <= health.availableSpareThreshold)
    {
        return SmartStatus::Critical;
    }

    // Percentage used > 100% indicates the drive has exceeded its rated endurance
    if (health.percentageUsed > 100)
    {
        return SmartStatus::Warning;
    }

    // Media errors indicate data integrity issues
    if (health.mediaErrors > 0)
    {
        return SmartStatus::Warning;
    }

    return SmartStatus::OK;
}

} // namespace spw
