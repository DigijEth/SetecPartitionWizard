// Benchmark.cpp -- Disk performance benchmark using direct I/O.
//
// DISCLAIMER: This code is for authorized disk utility software only.
//             Write benchmarks create temporary files on the target volume.

#include "Benchmark.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <sstream>
#include <thread>

namespace spw
{

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Benchmark::Benchmark(const std::string& volumePath)
    : m_volumePath(volumePath)
{
}

// ---------------------------------------------------------------------------
// getTimestamp -- QueryPerformanceCounter-based high-resolution timer
// ---------------------------------------------------------------------------

double Benchmark::getTimestamp()
{
    static LARGE_INTEGER frequency = {};
    if (frequency.QuadPart == 0)
        QueryPerformanceFrequency(&frequency);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

// ---------------------------------------------------------------------------
// getVolumeSize -- query the free space on the volume
// ---------------------------------------------------------------------------

Result<uint64_t> Benchmark::getVolumeSize() const
{
    // Convert path to wide string
    std::wstring wpath(m_volumePath.begin(), m_volumePath.end());

    ULARGE_INTEGER freeBytesAvailable, totalBytes, totalFreeBytes;
    BOOL ok = GetDiskFreeSpaceExW(wpath.c_str(), &freeBytesAvailable,
                                   &totalBytes, &totalFreeBytes);
    if (!ok)
        return ErrorInfo::fromWin32(ErrorCode::BenchmarkFailed, GetLastError(),
                                    "GetDiskFreeSpaceExW failed");

    return static_cast<uint64_t>(totalBytes.QuadPart);
}

// ---------------------------------------------------------------------------
// createTempFile -- create a preallocated temp file for write testing
// ---------------------------------------------------------------------------

Result<std::wstring> Benchmark::createTempFile(uint64_t sizeBytes)
{
    std::wstring wpath(m_volumePath.begin(), m_volumePath.end());

    // Generate temp file path
    wchar_t tempPath[MAX_PATH + 1] = {};
    wchar_t tempFile[MAX_PATH + 1] = {};

    // Use the volume root as temp directory
    wcsncpy_s(tempPath, wpath.c_str(), MAX_PATH);

    if (GetTempFileNameW(tempPath, L"spw", 0, tempFile) == 0)
        return ErrorInfo::fromWin32(ErrorCode::FileCreateFailed, GetLastError(),
                                    "Cannot create temp file");

    m_tempFilePath = tempFile;

    // Open with FILE_FLAG_NO_BUFFERING for direct I/O
    HANDLE hFile = CreateFileW(
        m_tempFilePath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
        return ErrorInfo::fromWin32(ErrorCode::FileCreateFailed, GetLastError(),
                                    "Cannot open temp file for writing");

    // Preallocate by setting the file pointer and end of file.
    // We need to write actual data since FILE_FLAG_NO_BUFFERING requires
    // sector-aligned writes.
    LARGE_INTEGER fileSize;
    fileSize.QuadPart = static_cast<LONGLONG>(sizeBytes);
    SetFilePointerEx(hFile, fileSize, nullptr, FILE_BEGIN);
    SetEndOfFile(hFile);
    SetFilePointerEx(hFile, {}, nullptr, FILE_BEGIN);

    // Write zeros in 1 MiB chunks to actually allocate the space
    const uint32_t chunkSize = 1024 * 1024;
    std::vector<uint8_t> zeros(chunkSize, 0);
    uint64_t written = 0;

    while (written < sizeBytes)
    {
        DWORD toWrite = static_cast<DWORD>(std::min(
            static_cast<uint64_t>(chunkSize), sizeBytes - written));

        // Align to sector size
        toWrite = (toWrite / 512) * 512;
        if (toWrite == 0)
            break;

        DWORD bytesWritten = 0;
        WriteFile(hFile, zeros.data(), toWrite, &bytesWritten, nullptr);
        if (bytesWritten == 0)
            break;
        written += bytesWritten;
    }

    CloseHandle(hFile);
    return m_tempFilePath;
}

// ---------------------------------------------------------------------------
// deleteTempFile
// ---------------------------------------------------------------------------

void Benchmark::deleteTempFile()
{
    if (!m_tempFilePath.empty())
    {
        DeleteFileW(m_tempFilePath.c_str());
        m_tempFilePath.clear();
    }
}

// ---------------------------------------------------------------------------
// sequentialRead -- read large contiguous blocks, measure throughput
// ---------------------------------------------------------------------------

Result<double> Benchmark::sequentialRead(int durationSec, uint32_t blockSize,
                                          BenchmarkProgress progressCb,
                                          std::atomic<bool>* cancelFlag)
{
    // We'll read from the raw volume.  Build the volume device path.
    // E.g., for "C:\", the device path is "\\.\C:"
    if (m_volumePath.empty())
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Volume path is empty");

    wchar_t driveLetter = static_cast<wchar_t>(m_volumePath[0]);
    std::wstring devicePath = L"\\\\.\\";
    devicePath += driveLetter;
    devicePath += L':';

    HANDLE hDevice = CreateFileW(
        devicePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (hDevice == INVALID_HANDLE_VALUE)
        return ErrorInfo::fromWin32(ErrorCode::BenchmarkFailed, GetLastError(),
                                    "Cannot open volume for sequential read benchmark");

    // Align block size to 512-byte boundary
    blockSize = (blockSize / 512) * 512;
    if (blockSize == 0)
        blockSize = BENCH_BLOCK_SEQ;

    // Allocate an aligned read buffer
    void* buffer = VirtualAlloc(nullptr, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer)
    {
        CloseHandle(hDevice);
        return ErrorInfo::fromCode(ErrorCode::OutOfMemory, "Cannot allocate aligned buffer");
    }

    uint64_t totalBytesRead = 0;
    double startTime = getTimestamp();
    double elapsed = 0.0;

    while (elapsed < durationSec)
    {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
        {
            VirtualFree(buffer, 0, MEM_RELEASE);
            CloseHandle(hDevice);
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled);
        }

        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hDevice, buffer, blockSize, &bytesRead, nullptr);
        if (!ok || bytesRead == 0)
        {
            // Reached end of volume or error; seek back to start
            LARGE_INTEGER zero = {};
            SetFilePointerEx(hDevice, zero, nullptr, FILE_BEGIN);
        }
        else
        {
            totalBytesRead += bytesRead;
        }

        elapsed = getTimestamp() - startTime;

        if (progressCb)
        {
            int pct = static_cast<int>((elapsed / durationSec) * 100.0);
            pct = std::min(pct, 100);
            BenchmarkResults partial;
            partial.seqReadMBps = (totalBytesRead / (1024.0 * 1024.0)) / std::max(elapsed, 0.001);
            progressCb(BenchmarkPhase::SequentialRead, pct, partial);
        }
    }

    VirtualFree(buffer, 0, MEM_RELEASE);
    CloseHandle(hDevice);

    if (elapsed <= 0.0)
        return 0.0;

    double mbps = (static_cast<double>(totalBytesRead) / (1024.0 * 1024.0)) / elapsed;
    return mbps;
}

// ---------------------------------------------------------------------------
// sequentialWrite -- write large contiguous blocks to a temp file
// ---------------------------------------------------------------------------

Result<double> Benchmark::sequentialWrite(int durationSec, uint32_t blockSize,
                                           BenchmarkProgress progressCb,
                                           std::atomic<bool>* cancelFlag)
{
    blockSize = (blockSize / 512) * 512;
    if (blockSize == 0)
        blockSize = BENCH_BLOCK_SEQ;

    // Create temp file
    auto tempResult = createTempFile(static_cast<uint64_t>(blockSize) * 2048);
    if (tempResult.isError())
        return tempResult.error();

    HANDLE hFile = CreateFileW(
        m_tempFilePath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        deleteTempFile();
        return ErrorInfo::fromWin32(ErrorCode::BenchmarkFailed, GetLastError(),
                                    "Cannot open temp file for write benchmark");
    }

    // Allocate aligned write buffer with random data
    void* buffer = VirtualAlloc(nullptr, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer)
    {
        CloseHandle(hFile);
        deleteTempFile();
        return ErrorInfo::fromCode(ErrorCode::OutOfMemory, "Cannot allocate aligned buffer");
    }

    // Fill with random data to avoid compression effects
    std::mt19937 rng(42);
    uint32_t* buf32 = static_cast<uint32_t*>(buffer);
    for (uint32_t i = 0; i < blockSize / 4; ++i)
        buf32[i] = rng();

    uint64_t totalBytesWritten = 0;
    double startTime = getTimestamp();
    double elapsed = 0.0;

    while (elapsed < durationSec)
    {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
        {
            VirtualFree(buffer, 0, MEM_RELEASE);
            CloseHandle(hFile);
            deleteTempFile();
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled);
        }

        DWORD bytesWritten = 0;
        BOOL ok = WriteFile(hFile, buffer, blockSize, &bytesWritten, nullptr);
        if (!ok || bytesWritten == 0)
        {
            // Wrap around to start of file
            LARGE_INTEGER zero = {};
            SetFilePointerEx(hFile, zero, nullptr, FILE_BEGIN);
        }
        else
        {
            totalBytesWritten += bytesWritten;
        }

        elapsed = getTimestamp() - startTime;

        if (progressCb)
        {
            int pct = static_cast<int>((elapsed / durationSec) * 100.0);
            pct = std::min(pct, 100);
            BenchmarkResults partial;
            partial.seqWriteMBps = (totalBytesWritten / (1024.0 * 1024.0)) / std::max(elapsed, 0.001);
            progressCb(BenchmarkPhase::SequentialWrite, pct, partial);
        }
    }

    VirtualFree(buffer, 0, MEM_RELEASE);
    CloseHandle(hFile);
    deleteTempFile();

    if (elapsed <= 0.0)
        return 0.0;

    double mbps = (static_cast<double>(totalBytesWritten) / (1024.0 * 1024.0)) / elapsed;
    return mbps;
}

// ---------------------------------------------------------------------------
// randomRead4K -- random 4K reads, measure IOPS
// ---------------------------------------------------------------------------

Result<double> Benchmark::randomRead4K(int durationSec, int queueDepth,
                                        BenchmarkProgress progressCb,
                                        std::atomic<bool>* cancelFlag)
{
    if (m_volumePath.empty())
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Volume path is empty");

    auto volSizeResult = getVolumeSize();
    if (volSizeResult.isError())
        return volSizeResult.error();

    const uint64_t volumeSize = volSizeResult.value();
    const uint32_t blockSize = BENCH_BLOCK_RND;
    const uint64_t maxOffset = (volumeSize / blockSize) * blockSize;
    if (maxOffset < blockSize)
        return ErrorInfo::fromCode(ErrorCode::BenchmarkFailed, "Volume too small for random read test");

    wchar_t driveLetter = static_cast<wchar_t>(m_volumePath[0]);
    std::wstring devicePath = L"\\\\.\\";
    devicePath += driveLetter;
    devicePath += L':';

    BenchmarkPhase phase = (queueDepth > 1)
        ? BenchmarkPhase::RandomRead4K_QD32
        : BenchmarkPhase::RandomRead4K_QD1;

    if (queueDepth <= 1)
    {
        // QD1: simple synchronous random reads
        HANDLE hDevice = CreateFileW(
            devicePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS,
            nullptr);

        if (hDevice == INVALID_HANDLE_VALUE)
            return ErrorInfo::fromWin32(ErrorCode::BenchmarkFailed, GetLastError(),
                                        "Cannot open volume for random read");

        void* buffer = VirtualAlloc(nullptr, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buffer)
        {
            CloseHandle(hDevice);
            return ErrorInfo::fromCode(ErrorCode::OutOfMemory, "Cannot allocate buffer");
        }

        std::mt19937_64 rng(std::random_device{}());
        uint64_t totalOps = 0;
        double totalLatency = 0.0;
        double startTime = getTimestamp();
        double elapsed = 0.0;

        while (elapsed < durationSec)
        {
            if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
            {
                VirtualFree(buffer, 0, MEM_RELEASE);
                CloseHandle(hDevice);
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled);
            }

            // Random aligned offset
            uint64_t offset = (rng() % (maxOffset / blockSize)) * blockSize;
            LARGE_INTEGER li;
            li.QuadPart = static_cast<LONGLONG>(offset);
            SetFilePointerEx(hDevice, li, nullptr, FILE_BEGIN);

            double opStart = getTimestamp();
            DWORD bytesRead = 0;
            ReadFile(hDevice, buffer, blockSize, &bytesRead, nullptr);
            double opEnd = getTimestamp();

            if (bytesRead > 0)
            {
                ++totalOps;
                totalLatency += (opEnd - opStart);
            }

            elapsed = getTimestamp() - startTime;

            if (progressCb && (totalOps % 1000 == 0))
            {
                int pct = static_cast<int>((elapsed / durationSec) * 100.0);
                BenchmarkResults partial;
                partial.rnd4kReadIOPS = totalOps / std::max(elapsed, 0.001);
                partial.avgReadLatencyUs = (totalOps > 0)
                    ? (totalLatency / totalOps) * 1e6
                    : 0.0;
                progressCb(phase, std::min(pct, 100), partial);
            }
        }

        VirtualFree(buffer, 0, MEM_RELEASE);
        CloseHandle(hDevice);

        return (elapsed > 0.0) ? (static_cast<double>(totalOps) / elapsed) : 0.0;
    }
    else
    {
        // QD32: use overlapped I/O for concurrent requests
        HANDLE hDevice = CreateFileW(
            devicePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS,
            nullptr);

        if (hDevice == INVALID_HANDLE_VALUE)
            return ErrorInfo::fromWin32(ErrorCode::BenchmarkFailed, GetLastError(),
                                        "Cannot open volume for QD32 random read");

        struct IoSlot
        {
            OVERLAPPED overlapped = {};
            void*      buffer = nullptr;
            bool       pending = false;
        };

        std::vector<IoSlot> slots(queueDepth);
        for (auto& slot : slots)
        {
            slot.buffer = VirtualAlloc(nullptr, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            slot.overlapped.hEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        }

        std::mt19937_64 rng(std::random_device{}());
        uint64_t totalOps = 0;
        double startTime = getTimestamp();
        double elapsed = 0.0;

        // Submit initial batch
        for (auto& slot : slots)
        {
            uint64_t offset = (rng() % (maxOffset / blockSize)) * blockSize;
            slot.overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
            slot.overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
            ResetEvent(slot.overlapped.hEvent);
            ReadFile(hDevice, slot.buffer, blockSize, nullptr, &slot.overlapped);
            slot.pending = true;
        }

        while (elapsed < durationSec)
        {
            if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
                break;

            for (auto& slot : slots)
            {
                if (!slot.pending)
                    continue;

                DWORD bytesRead = 0;
                BOOL result = GetOverlappedResult(hDevice, &slot.overlapped, &bytesRead, FALSE);
                if (result || GetLastError() != ERROR_IO_INCOMPLETE)
                {
                    ++totalOps;
                    slot.pending = false;

                    // Resubmit
                    uint64_t offset = (rng() % (maxOffset / blockSize)) * blockSize;
                    slot.overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
                    slot.overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
                    slot.overlapped.Internal = 0;
                    slot.overlapped.InternalHigh = 0;
                    ResetEvent(slot.overlapped.hEvent);
                    ReadFile(hDevice, slot.buffer, blockSize, nullptr, &slot.overlapped);
                    slot.pending = true;
                }
            }

            elapsed = getTimestamp() - startTime;
        }

        // Cancel outstanding I/O and clean up
        CancelIo(hDevice);
        for (auto& slot : slots)
        {
            if (slot.pending)
            {
                DWORD bytesRead = 0;
                GetOverlappedResult(hDevice, &slot.overlapped, &bytesRead, TRUE);
            }
            if (slot.overlapped.hEvent)
                CloseHandle(slot.overlapped.hEvent);
            if (slot.buffer)
                VirtualFree(slot.buffer, 0, MEM_RELEASE);
        }
        CloseHandle(hDevice);

        return (elapsed > 0.0) ? (static_cast<double>(totalOps) / elapsed) : 0.0;
    }
}

// ---------------------------------------------------------------------------
// randomWrite4K -- random 4K writes, measure IOPS
// ---------------------------------------------------------------------------

Result<double> Benchmark::randomWrite4K(int durationSec, int queueDepth,
                                          BenchmarkProgress progressCb,
                                          std::atomic<bool>* cancelFlag)
{
    const uint32_t blockSize = BENCH_BLOCK_RND;

    // Create a temp file for random writes
    uint64_t tempSize = 256ULL * 1024 * 1024; // 256 MiB
    auto tempResult = createTempFile(tempSize);
    if (tempResult.isError())
        return tempResult.error();

    const uint64_t maxOffset = (tempSize / blockSize) * blockSize;

    BenchmarkPhase phase = (queueDepth > 1)
        ? BenchmarkPhase::RandomWrite4K_QD32
        : BenchmarkPhase::RandomWrite4K_QD1;

    if (queueDepth <= 1)
    {
        HANDLE hFile = CreateFileW(
            m_tempFilePath.c_str(),
            GENERIC_WRITE | GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_RANDOM_ACCESS,
            nullptr);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            deleteTempFile();
            return ErrorInfo::fromWin32(ErrorCode::BenchmarkFailed, GetLastError(),
                                        "Cannot open temp file for random write");
        }

        void* buffer = VirtualAlloc(nullptr, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buffer)
        {
            CloseHandle(hFile);
            deleteTempFile();
            return ErrorInfo::fromCode(ErrorCode::OutOfMemory, "Cannot allocate buffer");
        }

        // Fill buffer with random data
        std::mt19937 fillRng(42);
        uint32_t* buf32 = static_cast<uint32_t*>(buffer);
        for (uint32_t i = 0; i < blockSize / 4; ++i)
            buf32[i] = fillRng();

        std::mt19937_64 rng(std::random_device{}());
        uint64_t totalOps = 0;
        double totalLatency = 0.0;
        double startTime = getTimestamp();
        double elapsed = 0.0;

        while (elapsed < durationSec)
        {
            if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
            {
                VirtualFree(buffer, 0, MEM_RELEASE);
                CloseHandle(hFile);
                deleteTempFile();
                return ErrorInfo::fromCode(ErrorCode::OperationCanceled);
            }

            uint64_t offset = (rng() % (maxOffset / blockSize)) * blockSize;
            LARGE_INTEGER li;
            li.QuadPart = static_cast<LONGLONG>(offset);
            SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN);

            double opStart = getTimestamp();
            DWORD bytesWritten = 0;
            WriteFile(hFile, buffer, blockSize, &bytesWritten, nullptr);
            double opEnd = getTimestamp();

            if (bytesWritten > 0)
            {
                ++totalOps;
                totalLatency += (opEnd - opStart);
            }

            elapsed = getTimestamp() - startTime;

            if (progressCb && (totalOps % 1000 == 0))
            {
                int pct = static_cast<int>((elapsed / durationSec) * 100.0);
                BenchmarkResults partial;
                partial.rnd4kWriteIOPS = totalOps / std::max(elapsed, 0.001);
                partial.avgWriteLatencyUs = (totalOps > 0)
                    ? (totalLatency / totalOps) * 1e6
                    : 0.0;
                progressCb(phase, std::min(pct, 100), partial);
            }
        }

        VirtualFree(buffer, 0, MEM_RELEASE);
        CloseHandle(hFile);
        deleteTempFile();

        return (elapsed > 0.0) ? (static_cast<double>(totalOps) / elapsed) : 0.0;
    }
    else
    {
        // QD32 overlapped writes
        HANDLE hFile = CreateFileW(
            m_tempFilePath.c_str(),
            GENERIC_WRITE | GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_RANDOM_ACCESS,
            nullptr);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            deleteTempFile();
            return ErrorInfo::fromWin32(ErrorCode::BenchmarkFailed, GetLastError(),
                                        "Cannot open temp file for QD32 random write");
        }

        struct IoSlot
        {
            OVERLAPPED overlapped = {};
            void*      buffer = nullptr;
            bool       pending = false;
        };

        std::vector<IoSlot> slots(queueDepth);
        std::mt19937 fillRng(42);
        for (auto& slot : slots)
        {
            slot.buffer = VirtualAlloc(nullptr, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            uint32_t* buf32 = static_cast<uint32_t*>(slot.buffer);
            for (uint32_t i = 0; i < blockSize / 4; ++i)
                buf32[i] = fillRng();
            slot.overlapped.hEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        }

        std::mt19937_64 rng(std::random_device{}());
        uint64_t totalOps = 0;
        double startTime = getTimestamp();
        double elapsed = 0.0;

        // Submit initial batch
        for (auto& slot : slots)
        {
            uint64_t offset = (rng() % (maxOffset / blockSize)) * blockSize;
            slot.overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
            slot.overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
            ResetEvent(slot.overlapped.hEvent);
            WriteFile(hFile, slot.buffer, blockSize, nullptr, &slot.overlapped);
            slot.pending = true;
        }

        while (elapsed < durationSec)
        {
            if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
                break;

            for (auto& slot : slots)
            {
                if (!slot.pending)
                    continue;

                DWORD bytesWritten = 0;
                BOOL result = GetOverlappedResult(hFile, &slot.overlapped, &bytesWritten, FALSE);
                if (result || GetLastError() != ERROR_IO_INCOMPLETE)
                {
                    ++totalOps;
                    slot.pending = false;

                    uint64_t offset = (rng() % (maxOffset / blockSize)) * blockSize;
                    slot.overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
                    slot.overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
                    slot.overlapped.Internal = 0;
                    slot.overlapped.InternalHigh = 0;
                    ResetEvent(slot.overlapped.hEvent);
                    WriteFile(hFile, slot.buffer, blockSize, nullptr, &slot.overlapped);
                    slot.pending = true;
                }
            }

            elapsed = getTimestamp() - startTime;
        }

        CancelIo(hFile);
        for (auto& slot : slots)
        {
            if (slot.pending)
            {
                DWORD bw = 0;
                GetOverlappedResult(hFile, &slot.overlapped, &bw, TRUE);
            }
            if (slot.overlapped.hEvent)
                CloseHandle(slot.overlapped.hEvent);
            if (slot.buffer)
                VirtualFree(slot.buffer, 0, MEM_RELEASE);
        }
        CloseHandle(hFile);
        deleteTempFile();

        return (elapsed > 0.0) ? (static_cast<double>(totalOps) / elapsed) : 0.0;
    }
}

// ---------------------------------------------------------------------------
// run -- complete benchmark suite
// ---------------------------------------------------------------------------

Result<BenchmarkResults> Benchmark::run(
    const BenchmarkConfig& config,
    BenchmarkProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    BenchmarkResults results;

    // Sequential read
    auto seqReadResult = sequentialRead(config.durationSeconds, config.seqBlockSize,
                                         progressCb, cancelFlag);
    if (seqReadResult.isOk())
        results.seqReadMBps = seqReadResult.value();
    else if (seqReadResult.error().code == ErrorCode::OperationCanceled)
        return seqReadResult.error();

    // Sequential write
    if (!config.skipWriteTests)
    {
        auto seqWriteResult = sequentialWrite(config.durationSeconds, config.seqBlockSize,
                                               progressCb, cancelFlag);
        if (seqWriteResult.isOk())
            results.seqWriteMBps = seqWriteResult.value();
        else if (seqWriteResult.error().code == ErrorCode::OperationCanceled)
            return seqWriteResult.error();
    }

    // Random read 4K QD1
    auto rndReadResult = randomRead4K(config.durationSeconds, 1, progressCb, cancelFlag);
    if (rndReadResult.isOk())
        results.rnd4kReadIOPS = rndReadResult.value();
    else if (rndReadResult.error().code == ErrorCode::OperationCanceled)
        return rndReadResult.error();

    // Random read 4K QD32
    auto rndReadQD32 = randomRead4K(config.durationSeconds, 32, progressCb, cancelFlag);
    if (rndReadQD32.isOk())
        results.rnd4kReadIOPS_QD32 = rndReadQD32.value();
    else if (rndReadQD32.error().code == ErrorCode::OperationCanceled)
        return rndReadQD32.error();

    // Random write 4K QD1
    if (!config.skipWriteTests)
    {
        auto rndWriteResult = randomWrite4K(config.durationSeconds, 1, progressCb, cancelFlag);
        if (rndWriteResult.isOk())
            results.rnd4kWriteIOPS = rndWriteResult.value();
        else if (rndWriteResult.error().code == ErrorCode::OperationCanceled)
            return rndWriteResult.error();

        // Random write 4K QD32
        auto rndWriteQD32 = randomWrite4K(config.durationSeconds, 32, progressCb, cancelFlag);
        if (rndWriteQD32.isOk())
            results.rnd4kWriteIOPS_QD32 = rndWriteQD32.value();
        else if (rndWriteQD32.error().code == ErrorCode::OperationCanceled)
            return rndWriteQD32.error();
    }

    // Calculate average latencies from QD1 results
    if (results.rnd4kReadIOPS > 0)
        results.avgReadLatencyUs = (1.0 / results.rnd4kReadIOPS) * 1e6;
    if (results.rnd4kWriteIOPS > 0)
        results.avgWriteLatencyUs = (1.0 / results.rnd4kWriteIOPS) * 1e6;

    if (progressCb)
    {
        progressCb(BenchmarkPhase::Complete, 100, results);
    }

    return results;
}

} // namespace spw
