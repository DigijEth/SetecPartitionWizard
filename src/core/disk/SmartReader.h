#pragma once

// SmartReader — Read and parse S.M.A.R.T. data from ATA and NVMe drives.
// ATA drives: IOCTL_ATA_PASS_THROUGH with SMART READ DATA (command 0xB0, feature 0xD0).
// NVMe drives: IOCTL_STORAGE_QUERY_PROPERTY with StorageAdapterProtocolSpecificProperty.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winioctl.h>

// NVMe log page ID for health info (may not be defined in all SDK versions)
#ifndef NVME_LOG_PAGE_HEALTH_INFO
#define NVME_LOG_PAGE_HEALTH_INFO 0x02
#endif

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace spw
{

// Health status for individual attributes or overall drive
enum class SmartStatus
{
    OK,
    Warning,
    Critical,
    Unknown,
};

// A single S.M.A.R.T. attribute
struct SmartAttribute
{
    uint8_t id = 0;
    std::string name;
    uint8_t currentValue = 0;
    uint8_t worstValue = 0;
    uint8_t threshold = 0;
    uint64_t rawValue = 0;
    SmartStatus status = SmartStatus::Unknown;
};

// NVMe health info (from SMART/Health Information Log, NVMe spec 1.4 Figure 93)
struct NvmeHealthInfo
{
    uint8_t criticalWarning = 0;
    uint16_t temperature = 0;               // Kelvin
    uint8_t availableSpare = 0;             // percentage
    uint8_t availableSpareThreshold = 0;    // percentage
    uint8_t percentageUsed = 0;
    // These are 128-bit in the spec; we store low 64 bits which is sufficient for most drives
    uint64_t dataUnitsRead = 0;
    uint64_t dataUnitsWritten = 0;
    uint64_t hostReadCommands = 0;
    uint64_t hostWriteCommands = 0;
    uint64_t controllerBusyTime = 0;        // minutes
    uint64_t powerCycles = 0;
    uint64_t powerOnHours = 0;
    uint64_t unsafeShutdowns = 0;
    uint64_t mediaErrors = 0;
    uint64_t errorLogEntries = 0;
};

// Overall S.M.A.R.T. result for a drive
struct SmartData
{
    DiskId diskId = -1;
    bool isNvme = false;
    SmartStatus overallHealth = SmartStatus::Unknown;

    // ATA attributes (empty for NVMe)
    std::vector<SmartAttribute> attributes;

    // NVMe health info (zeroed for ATA)
    NvmeHealthInfo nvmeHealth = {};
};

namespace SmartReader
{

// Read S.M.A.R.T. data from a disk. Automatically detects ATA vs NVMe.
// Requires an open handle with at least GENERIC_READ | GENERIC_EXECUTE.
Result<SmartData> readSmartData(HANDLE diskHandle, DiskId diskId);

// Read ATA S.M.A.R.T. attributes via IOCTL_ATA_PASS_THROUGH.
Result<SmartData> readAtaSmart(HANDLE diskHandle, DiskId diskId);

// Read NVMe health info via IOCTL_STORAGE_QUERY_PROPERTY.
Result<SmartData> readNvmeSmart(HANDLE diskHandle, DiskId diskId);

// Read ATA S.M.A.R.T. thresholds (command 0xB0, feature 0xD1).
Result<std::vector<std::pair<uint8_t, uint8_t>>> readAtaSmartThresholds(HANDLE diskHandle);

// Determine if a disk supports NVMe protocol using IOCTL_STORAGE_QUERY_PROPERTY
// with StorageAdapterProtocolSpecificProperty.
Result<bool> isNvmeDrive(HANDLE diskHandle);

// Get the human-readable name for a standard S.M.A.R.T. attribute ID.
const char* getAttributeName(uint8_t attributeId);

// Calculate the health status for an individual attribute given its value and threshold.
SmartStatus evaluateAttributeHealth(uint8_t currentValue, uint8_t worstValue, uint8_t threshold);

// Calculate overall drive health from all attributes.
SmartStatus evaluateOverallHealth(const std::vector<SmartAttribute>& attributes);

// Calculate overall NVMe health from health info.
SmartStatus evaluateNvmeHealth(const NvmeHealthInfo& health);

} // namespace SmartReader
} // namespace spw
