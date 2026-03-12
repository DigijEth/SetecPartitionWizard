#pragma once

// Benchmark -- Measure sequential/random read and write performance of a disk.
//
// Uses Win32 file I/O with FILE_FLAG_NO_BUFFERING for direct disk access, and
// QueryPerformanceCounter for sub-microsecond timing precision.
//
// Tests:
//   - Sequential read  (1 MiB blocks)
//   - Sequential write (1 MiB blocks, temp file on target volume)
//   - Random read  4K  (QD1 and QD32)
//   - Random write 4K  (QD1 and QD32)
//
// DISCLAIMER: This code is for authorized disk utility software only.
//             Write tests create temporary files; random write tests involve
//             sustained 4K random writes which add write amplification on SSDs.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "../common/Constants.h"
#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spw
{

// Results of a complete benchmark run
struct BenchmarkResults
{
    double seqReadMBps     = 0.0;  // Sequential read throughput (MiB/s)
    double seqWriteMBps    = 0.0;  // Sequential write throughput (MiB/s)
    double rnd4kReadIOPS   = 0.0;  // Random 4K read IOPS (QD1)
    double rnd4kWriteIOPS  = 0.0;  // Random 4K write IOPS (QD1)
    double rnd4kReadIOPS_QD32  = 0.0; // Random 4K read IOPS (QD32)
    double rnd4kWriteIOPS_QD32 = 0.0; // Random 4K write IOPS (QD32)
    double avgReadLatencyUs  = 0.0;// Average read latency (microseconds)
    double avgWriteLatencyUs = 0.0;// Average write latency (microseconds)
};

// Which test is currently running
enum class BenchmarkPhase
{
    SequentialRead,
    SequentialWrite,
    RandomRead4K_QD1,
    RandomWrite4K_QD1,
    RandomRead4K_QD32,
    RandomWrite4K_QD32,
    Complete,
};

// Progress callback.
// Parameters: (currentPhase, phasePercentage 0-100, partialResults)
using BenchmarkProgress = std::function<void(BenchmarkPhase phase,
                                              int percentage,
                                              const BenchmarkResults& partial)>;

// Configuration for the benchmark
struct BenchmarkConfig
{
    int durationSeconds = BENCH_DEFAULT_DURATION_SEC; // Per test
    uint32_t seqBlockSize = BENCH_BLOCK_SEQ;          // Sequential block size
    uint32_t rndBlockSize = BENCH_BLOCK_RND;          // Random block size
    uint64_t testFileSizeBytes = 1024ULL * 1024 * 1024; // 1 GiB temp file for writes
    bool skipWriteTests = false;                      // Skip write tests (safe mode)
};

class Benchmark
{
public:
    // volumePath: root of the volume to benchmark, e.g. "C:\\"
    explicit Benchmark(const std::string& volumePath);

    // Run the complete benchmark suite
    Result<BenchmarkResults> run(
        const BenchmarkConfig& config = {},
        BenchmarkProgress progressCb = nullptr,
        std::atomic<bool>* cancelFlag = nullptr);

    // Run individual tests
    Result<double> sequentialRead(int durationSec, uint32_t blockSize,
                                  BenchmarkProgress progressCb = nullptr,
                                  std::atomic<bool>* cancelFlag = nullptr);
    Result<double> sequentialWrite(int durationSec, uint32_t blockSize,
                                   BenchmarkProgress progressCb = nullptr,
                                   std::atomic<bool>* cancelFlag = nullptr);
    Result<double> randomRead4K(int durationSec, int queueDepth,
                                 BenchmarkProgress progressCb = nullptr,
                                 std::atomic<bool>* cancelFlag = nullptr);
    Result<double> randomWrite4K(int durationSec, int queueDepth,
                                  BenchmarkProgress progressCb = nullptr,
                                  std::atomic<bool>* cancelFlag = nullptr);

private:
    // Create a temp file filled with random data for write testing
    Result<std::wstring> createTempFile(uint64_t sizeBytes);

    // Delete the temp file
    void deleteTempFile();

    // Get high-precision timestamp in seconds
    static double getTimestamp();

    // Get volume size for clamping random offsets
    Result<uint64_t> getVolumeSize() const;

    std::string  m_volumePath;
    std::wstring m_tempFilePath;
};

} // namespace spw
