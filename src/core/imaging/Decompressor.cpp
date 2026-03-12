#include "Decompressor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <QtZlib/zlib.h>

// xz-embedded header — provided at third_party/xz-embedded/xz.h
#include "xz.h"

#include <cstring>
#include <vector>

namespace spw
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int kChunkSize = 4 * 1024 * 1024; // 4 MB processing chunks
static constexpr int kZipLocalFileHeaderSig = 0x04034b50;

// ---------------------------------------------------------------------------
// decompressXz — xz-embedded streaming decompression
// ---------------------------------------------------------------------------

Result<void> Decompressor::decompressXz(const QString& inputPath,
                                        const QString& outputPath,
                                        std::function<void(qint64, qint64)> progress)
{
    QFile inFile(inputPath);
    if (!inFile.open(QIODevice::ReadOnly)) {
        return ErrorInfo::fromCode(ErrorCode::FileNotFound,
            "Cannot open input file: " + inputPath.toStdString());
    }

    QFile outFile(outputPath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return ErrorInfo::fromCode(ErrorCode::FileCreateFailed,
            "Cannot create output file: " + outputPath.toStdString());
    }

    const qint64 totalSize = inFile.size();

    // Initialize CRC tables required by xz-embedded
    xz_crc32_init();

    // Allocate decoder — XZ_DYNALLOC lets xz-embedded malloc as needed,
    // dictionary limit set to 64 MB (1 << 26).
    struct xz_dec* decoder = xz_dec_init(XZ_DYNALLOC, 1 << 26);
    if (!decoder) {
        return ErrorInfo::fromCode(ErrorCode::DecompressionFailed,
            "Failed to initialize xz decoder");
    }

    std::vector<uint8_t> inBuf(kChunkSize);
    std::vector<uint8_t> outBuf(kChunkSize);

    struct xz_buf buf;
    std::memset(&buf, 0, sizeof(buf));
    buf.in = inBuf.data();
    buf.out = outBuf.data();
    buf.out_size = outBuf.size();

    qint64 totalRead = 0;
    enum xz_ret ret = XZ_OK;

    while (ret == XZ_OK) {
        // Refill input buffer if exhausted
        if (buf.in_pos == buf.in_size) {
            qint64 bytesRead = inFile.read(reinterpret_cast<char*>(inBuf.data()),
                                           static_cast<qint64>(inBuf.size()));
            if (bytesRead < 0) {
                xz_dec_end(decoder);
                return ErrorInfo::fromCode(ErrorCode::ImageReadError,
                    "Read error on input file: " + inputPath.toStdString());
            }
            buf.in_size = static_cast<size_t>(bytesRead);
            buf.in_pos = 0;
            totalRead += bytesRead;
        }

        buf.out_pos = 0;
        ret = xz_dec_run(decoder, &buf);

        // Write whatever output was produced
        if (buf.out_pos > 0) {
            qint64 written = outFile.write(reinterpret_cast<const char*>(outBuf.data()),
                                           static_cast<qint64>(buf.out_pos));
            if (written != static_cast<qint64>(buf.out_pos)) {
                xz_dec_end(decoder);
                return ErrorInfo::fromCode(ErrorCode::ImageWriteError,
                    "Write error on output file: " + outputPath.toStdString());
            }
        }

        if (progress && totalSize > 0) {
            progress(totalRead, totalSize);
        }
    }

    xz_dec_end(decoder);

    if (ret != XZ_STREAM_END) {
        outFile.remove();
        return ErrorInfo::fromCode(ErrorCode::DecompressionFailed,
            "XZ decompression failed (error code " + std::to_string(static_cast<int>(ret)) + ")");
    }

    outFile.flush();
    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// decompressGz — zlib inflate with gzip wrapper
// ---------------------------------------------------------------------------

Result<void> Decompressor::decompressGz(const QString& inputPath,
                                        const QString& outputPath,
                                        std::function<void(qint64, qint64)> progress)
{
    QFile inFile(inputPath);
    if (!inFile.open(QIODevice::ReadOnly)) {
        return ErrorInfo::fromCode(ErrorCode::FileNotFound,
            "Cannot open input file: " + inputPath.toStdString());
    }

    QFile outFile(outputPath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return ErrorInfo::fromCode(ErrorCode::FileCreateFailed,
            "Cannot create output file: " + outputPath.toStdString());
    }

    const qint64 totalSize = inFile.size();

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));

    // 15 + 16 = enable gzip decoding via zlib
    int zret = inflateInit2(&strm, 15 + 16);
    if (zret != Z_OK) {
        return ErrorInfo::fromCode(ErrorCode::DecompressionFailed,
            "inflateInit2 failed: " + std::string(strm.msg ? strm.msg : "unknown error"));
    }

    std::vector<Bytef> inBuf(kChunkSize);
    std::vector<Bytef> outBuf(kChunkSize);

    qint64 totalRead = 0;

    while (true) {
        qint64 bytesRead = inFile.read(reinterpret_cast<char*>(inBuf.data()),
                                       static_cast<qint64>(inBuf.size()));
        if (bytesRead < 0) {
            inflateEnd(&strm);
            return ErrorInfo::fromCode(ErrorCode::ImageReadError,
                "Read error on input file: " + inputPath.toStdString());
        }
        if (bytesRead == 0) {
            break; // EOF
        }

        totalRead += bytesRead;
        strm.avail_in = static_cast<uInt>(bytesRead);
        strm.next_in = inBuf.data();

        // Inflate all available input
        do {
            strm.avail_out = static_cast<uInt>(outBuf.size());
            strm.next_out = outBuf.data();

            zret = inflate(&strm, Z_NO_FLUSH);
            if (zret == Z_STREAM_ERROR || zret == Z_DATA_ERROR ||
                zret == Z_MEM_ERROR || zret == Z_NEED_DICT) {
                std::string errMsg = strm.msg ? strm.msg : "inflate error";
                inflateEnd(&strm);
                outFile.remove();
                return ErrorInfo::fromCode(ErrorCode::DecompressionFailed,
                    "Gzip decompression failed: " + errMsg);
            }

            uInt have = static_cast<uInt>(outBuf.size()) - strm.avail_out;
            if (have > 0) {
                qint64 written = outFile.write(reinterpret_cast<const char*>(outBuf.data()),
                                               static_cast<qint64>(have));
                if (written != static_cast<qint64>(have)) {
                    inflateEnd(&strm);
                    return ErrorInfo::fromCode(ErrorCode::ImageWriteError,
                        "Write error on output file: " + outputPath.toStdString());
                }
            }
        } while (strm.avail_out == 0);

        if (progress && totalSize > 0) {
            progress(totalRead, totalSize);
        }

        if (zret == Z_STREAM_END) {
            break;
        }
    }

    inflateEnd(&strm);
    outFile.flush();
    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// decompressZip — basic zip extraction using zlib raw inflate
//
// Parses local file headers directly (PK\x03\x04) and inflates each stored
// or deflated entry. This avoids a minizip dependency while handling the
// vast majority of zip files encountered in disk-imaging workflows.
// ---------------------------------------------------------------------------

namespace
{

struct ZipLocalHeader
{
    uint16_t versionNeeded;
    uint16_t flags;
    uint16_t method;
    uint16_t modTime;
    uint16_t modDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t nameLen;
    uint16_t extraLen;
};

template <typename T>
T readLE(const uint8_t* p)
{
    T val = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        val |= static_cast<T>(p[i]) << (8 * i);
    return val;
}

bool parseLocalHeader(const uint8_t* data, ZipLocalHeader& hdr)
{
    // Skip past 4-byte signature already verified by caller
    hdr.versionNeeded  = readLE<uint16_t>(data + 4);
    hdr.flags          = readLE<uint16_t>(data + 6);
    hdr.method         = readLE<uint16_t>(data + 8);
    hdr.modTime        = readLE<uint16_t>(data + 10);
    hdr.modDate        = readLE<uint16_t>(data + 12);
    hdr.crc32          = readLE<uint32_t>(data + 14);
    hdr.compressedSize = readLE<uint32_t>(data + 18);
    hdr.uncompressedSize = readLE<uint32_t>(data + 22);
    hdr.nameLen        = readLE<uint16_t>(data + 26);
    hdr.extraLen       = readLE<uint16_t>(data + 28);
    return true;
}

} // anonymous namespace

Result<void> Decompressor::decompressZip(const QString& inputPath,
                                         const QString& outputDir,
                                         std::function<void(qint64, qint64)> progress)
{
    QFile inFile(inputPath);
    if (!inFile.open(QIODevice::ReadOnly)) {
        return ErrorInfo::fromCode(ErrorCode::FileNotFound,
            "Cannot open zip file: " + inputPath.toStdString());
    }

    const qint64 totalSize = inFile.size();
    QDir outDirObj(outputDir);
    if (!outDirObj.mkpath(".")) {
        return ErrorInfo::fromCode(ErrorCode::FileCreateFailed,
            "Cannot create output directory: " + outputDir.toStdString());
    }

    qint64 bytesProcessed = 0;

    while (bytesProcessed < totalSize) {
        // Read local file header (30 bytes minimum)
        uint8_t headerBuf[30];
        inFile.seek(bytesProcessed);
        qint64 hdrRead = inFile.read(reinterpret_cast<char*>(headerBuf), 30);
        if (hdrRead < 30) {
            break; // No more entries
        }

        uint32_t sig = readLE<uint32_t>(headerBuf);
        if (sig != static_cast<uint32_t>(kZipLocalFileHeaderSig)) {
            // Reached central directory or end-of-central-dir — we are done
            break;
        }

        ZipLocalHeader hdr;
        parseLocalHeader(headerBuf, hdr);

        // Read filename
        QByteArray nameBuf(hdr.nameLen, '\0');
        inFile.read(nameBuf.data(), hdr.nameLen);
        QString entryName = QString::fromUtf8(nameBuf);

        // Skip extra field
        inFile.skip(hdr.extraLen);

        qint64 dataOffset = bytesProcessed + 30 + hdr.nameLen + hdr.extraLen;

        // Determine compressed size — handle data descriptor (bit 3 of flags)
        uint32_t compSize = hdr.compressedSize;
        uint32_t uncompSize = hdr.uncompressedSize;

        // If the entry name ends with '/', it is a directory
        if (entryName.endsWith('/') || entryName.endsWith('\\')) {
            outDirObj.mkpath(entryName);
            bytesProcessed = dataOffset + compSize;
            continue;
        }

        // Build output path, ensuring parent directories exist
        QString outPath = outDirObj.filePath(entryName);
        QFileInfo outInfo(outPath);
        outInfo.dir().mkpath(".");

        QFile outFile(outPath);
        if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return ErrorInfo::fromCode(ErrorCode::FileCreateFailed,
                "Cannot create file: " + outPath.toStdString());
        }

        inFile.seek(dataOffset);

        if (hdr.method == 0) {
            // Stored (no compression) — just copy bytes
            qint64 remaining = compSize;
            std::vector<char> buf(kChunkSize);
            while (remaining > 0) {
                qint64 toRead = std::min(remaining, static_cast<qint64>(buf.size()));
                qint64 got = inFile.read(buf.data(), toRead);
                if (got <= 0) {
                    return ErrorInfo::fromCode(ErrorCode::ImageReadError,
                        "Read error extracting stored entry: " + entryName.toStdString());
                }
                if (outFile.write(buf.data(), got) != got) {
                    return ErrorInfo::fromCode(ErrorCode::ImageWriteError,
                        "Write error extracting: " + entryName.toStdString());
                }
                remaining -= got;
            }
        } else if (hdr.method == 8) {
            // Deflated — use zlib raw inflate (windowBits = -15 for raw deflate)
            z_stream strm;
            std::memset(&strm, 0, sizeof(strm));
            int zret = inflateInit2(&strm, -15); // raw deflate
            if (zret != Z_OK) {
                return ErrorInfo::fromCode(ErrorCode::DecompressionFailed,
                    "inflateInit2 failed for zip entry: " + entryName.toStdString());
            }

            std::vector<Bytef> inBuf(kChunkSize);
            std::vector<Bytef> outBuf(kChunkSize);
            qint64 compRemaining = compSize;

            while (compRemaining > 0) {
                qint64 toRead = std::min(compRemaining, static_cast<qint64>(inBuf.size()));
                qint64 got = inFile.read(reinterpret_cast<char*>(inBuf.data()), toRead);
                if (got <= 0) {
                    inflateEnd(&strm);
                    return ErrorInfo::fromCode(ErrorCode::ImageReadError,
                        "Read error inflating zip entry: " + entryName.toStdString());
                }
                compRemaining -= got;

                strm.avail_in = static_cast<uInt>(got);
                strm.next_in = inBuf.data();

                do {
                    strm.avail_out = static_cast<uInt>(outBuf.size());
                    strm.next_out = outBuf.data();
                    zret = inflate(&strm, Z_NO_FLUSH);

                    if (zret == Z_DATA_ERROR || zret == Z_MEM_ERROR) {
                        std::string errMsg = strm.msg ? strm.msg : "inflate error";
                        inflateEnd(&strm);
                        return ErrorInfo::fromCode(ErrorCode::DecompressionFailed,
                            "Deflate error in zip entry '" + entryName.toStdString() + "': " + errMsg);
                    }

                    uInt have = static_cast<uInt>(outBuf.size()) - strm.avail_out;
                    if (have > 0) {
                        if (outFile.write(reinterpret_cast<const char*>(outBuf.data()),
                                          static_cast<qint64>(have)) != static_cast<qint64>(have)) {
                            inflateEnd(&strm);
                            return ErrorInfo::fromCode(ErrorCode::ImageWriteError,
                                "Write error inflating zip entry: " + entryName.toStdString());
                        }
                    }
                } while (strm.avail_out == 0);

                if (zret == Z_STREAM_END) {
                    break;
                }
            }

            inflateEnd(&strm);
        } else {
            // Unsupported compression method
            outFile.close();
            outFile.remove();
            return ErrorInfo::fromCode(ErrorCode::DecompressionFailed,
                "Unsupported zip compression method " + std::to_string(hdr.method)
                + " for entry: " + entryName.toStdString());
        }

        outFile.flush();
        outFile.close();

        // Handle data descriptor if bit 3 is set
        qint64 nextOffset = dataOffset + compSize;
        if (hdr.flags & 0x08) {
            // Data descriptor follows: optionally 4-byte sig + crc32 + compSize + uncompSize
            inFile.seek(nextOffset);
            uint8_t ddBuf[16];
            qint64 ddRead = inFile.read(reinterpret_cast<char*>(ddBuf), 16);
            if (ddRead >= 12) {
                uint32_t ddSig = readLE<uint32_t>(ddBuf);
                if (ddSig == 0x08074b50) {
                    // Signature present — descriptor is 16 bytes
                    nextOffset += 16;
                } else {
                    // No signature — descriptor is 12 bytes
                    nextOffset += 12;
                }
            }
        }

        bytesProcessed = nextOffset;

        if (progress && totalSize > 0) {
            progress(bytesProcessed, totalSize);
        }
    }

    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// decompressAuto — detect format by extension and dispatch
// ---------------------------------------------------------------------------

Result<QString> Decompressor::decompressAuto(const QString& inputPath,
                                             const QString& outputDir,
                                             std::function<void(qint64, qint64)> progress)
{
    QFileInfo info(inputPath);
    QString ext = info.suffix().toLower();

    QDir dir(outputDir);
    if (!dir.mkpath(".")) {
        return ErrorInfo::fromCode(ErrorCode::FileCreateFailed,
            "Cannot create output directory: " + outputDir.toStdString());
    }

    if (ext == "xz") {
        QString outName = decompressedName(info.fileName());
        QString outPath = dir.filePath(outName);
        auto result = decompressXz(inputPath, outPath, progress);
        if (result.isError())
            return result.error();
        return Result<QString>(outPath);
    }

    if (ext == "gz") {
        QString outName = decompressedName(info.fileName());
        QString outPath = dir.filePath(outName);
        auto result = decompressGz(inputPath, outPath, progress);
        if (result.isError())
            return result.error();
        return Result<QString>(outPath);
    }

    if (ext == "zip") {
        auto result = decompressZip(inputPath, outputDir, progress);
        if (result.isError())
            return result.error();
        return Result<QString>(outputDir);
    }

    return ErrorInfo::fromCode(ErrorCode::DecompressionFailed,
        "Unsupported compression format: ." + ext.toStdString());
}

// ---------------------------------------------------------------------------
// isCompressed — check extension
// ---------------------------------------------------------------------------

bool Decompressor::isCompressed(const QString& path)
{
    QString ext = QFileInfo(path).suffix().toLower();
    return (ext == "xz" || ext == "gz" || ext == "zip" || ext == "7z");
}

// ---------------------------------------------------------------------------
// decompressedName — strip compression extension
// ---------------------------------------------------------------------------

QString Decompressor::decompressedName(const QString& path)
{
    QFileInfo info(path);
    QString name = info.fileName();
    QString ext = info.suffix().toLower();

    if (ext == "xz" || ext == "gz" || ext == "zip" || ext == "7z") {
        // Remove the last extension: "file.img.xz" -> "file.img"
        int lastDot = name.lastIndexOf('.');
        if (lastDot > 0) {
            return name.left(lastDot);
        }
    }

    return name;
}

} // namespace spw
