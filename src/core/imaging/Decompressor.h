#pragma once

#include "../common/Result.h"
#include "../common/Error.h"

#include <QString>
#include <functional>

namespace spw
{

/// Streaming decompression utility for .xz, .gz, and .zip files.
class Decompressor
{
public:
    Decompressor() = delete;

    /// Decompress an .xz file using xz-embedded C API.
    static Result<void> decompressXz(const QString& inputPath,
                                     const QString& outputPath,
                                     std::function<void(qint64, qint64)> progress = nullptr);

    /// Decompress a .gz file using zlib inflate.
    static Result<void> decompressGz(const QString& inputPath,
                                     const QString& outputPath,
                                     std::function<void(qint64, qint64)> progress = nullptr);

    /// Extract a .zip archive using zlib (basic single-stream extraction).
    static Result<void> decompressZip(const QString& inputPath,
                                      const QString& outputDir,
                                      std::function<void(qint64, qint64)> progress = nullptr);

    /// Auto-detect format by extension and decompress. Returns output path on success.
    static Result<QString> decompressAuto(const QString& inputPath,
                                          const QString& outputDir,
                                          std::function<void(qint64, qint64)> progress = nullptr);

    /// Check if a file has a recognized compression extension (.xz, .gz, .zip, .7z).
    static bool isCompressed(const QString& path);

    /// Strip compression extension (e.g. "file.img.xz" -> "file.img").
    static QString decompressedName(const QString& path);
};

} // namespace spw
