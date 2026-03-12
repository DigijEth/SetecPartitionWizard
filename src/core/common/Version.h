#pragma once

#ifndef SPW_VERSION_MAJOR
#define SPW_VERSION_MAJOR 1
#endif

#ifndef SPW_VERSION_MINOR
#define SPW_VERSION_MINOR 0
#endif

#ifndef SPW_VERSION_PATCH
#define SPW_VERSION_PATCH 0
#endif

#ifndef SPW_VERSION_STRING
#define SPW_VERSION_STRING "1.0.0"
#endif

namespace spw
{

constexpr int VersionMajor = SPW_VERSION_MAJOR;
constexpr int VersionMinor = SPW_VERSION_MINOR;
constexpr int VersionPatch = SPW_VERSION_PATCH;
constexpr const char* VersionString = SPW_VERSION_STRING;
constexpr const char* AppName = "Setec Partition Wizard";

} // namespace spw
