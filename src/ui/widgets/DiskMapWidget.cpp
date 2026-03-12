#include "DiskMapWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>

namespace spw
{

DiskMapWidget::DiskMapWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(80);
    setFixedHeight(100);
}

void DiskMapWidget::setDisk(const DiskInfo& disk,
                            const std::vector<PartitionInfo>& partitions,
                            const std::vector<VolumeInfo>& volumes)
{
    m_disk = disk;
    m_partitions = partitions;
    m_volumes = volumes;
    m_hoveredBlock = -1;
    m_selectedBlock = -1;
    rebuildBlocks();
    update();
}

void DiskMapWidget::clear()
{
    m_disk = {};
    m_partitions.clear();
    m_volumes.clear();
    m_blocks.clear();
    m_blockRects.clear();
    m_hoveredBlock = -1;
    m_selectedBlock = -1;
    update();
}

void DiskMapWidget::rebuildBlocks()
{
    m_blocks.clear();
    if (m_disk.sizeBytes == 0)
        return;

    // Sort partitions by offset
    auto sorted = m_partitions;
    std::sort(sorted.begin(), sorted.end(),
              [](const PartitionInfo& a, const PartitionInfo& b) {
                  return a.offsetBytes < b.offsetBytes;
              });

    uint64_t currentOffset = 0;

    for (size_t i = 0; i < sorted.size(); ++i)
    {
        const auto& p = sorted[i];

        // Unallocated gap before this partition
        if (p.offsetBytes > currentOffset)
        {
            Block gap;
            gap.startBytes = currentOffset;
            gap.sizeBytes = p.offsetBytes - currentOffset;
            gap.fsType = FilesystemType::Unallocated;
            gap.label = QStringLiteral("Unallocated");
            gap.color = QColor(80, 80, 80);
            m_blocks.push_back(gap);
        }

        Block blk;
        blk.partitionIndex = p.index;
        blk.startBytes = p.offsetBytes;
        blk.sizeBytes = p.sizeBytes;
        blk.fsType = p.filesystemType;
        blk.driveLetter = p.driveLetter;
        blk.color = colorForFilesystem(p.filesystemType);

        // Try to find volume label
        if (!p.label.empty())
        {
            blk.label = QString::fromStdWString(p.label);
        }
        else if (p.driveLetter != L'\0')
        {
            blk.label = QString("%1:").arg(QChar(p.driveLetter));
        }
        else
        {
            blk.label = filesystemShortName(p.filesystemType);
        }

        m_blocks.push_back(blk);
        currentOffset = p.offsetBytes + p.sizeBytes;
    }

    // Trailing unallocated space
    if (currentOffset < m_disk.sizeBytes)
    {
        Block gap;
        gap.startBytes = currentOffset;
        gap.sizeBytes = m_disk.sizeBytes - currentOffset;
        gap.fsType = FilesystemType::Unallocated;
        gap.label = QStringLiteral("Unallocated");
        gap.color = QColor(80, 80, 80);
        m_blocks.push_back(gap);
    }
}

void DiskMapWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const int margin = 4;
    const QRect drawArea = rect().adjusted(margin, margin, -margin, -margin);

    if (m_blocks.empty() || m_disk.sizeBytes == 0)
    {
        painter.setPen(QColor(100, 100, 100));
        painter.drawText(drawArea, Qt::AlignCenter, tr("No disk selected"));
        return;
    }

    m_blockRects.resize(m_blocks.size());

    // Calculate block widths proportional to size, with minimum width
    const int totalWidth = drawArea.width();
    const int minBlockWidth = 40;
    const double totalBytes = static_cast<double>(m_disk.sizeBytes);

    // First pass: calculate raw widths
    std::vector<int> widths(m_blocks.size());
    int usedWidth = 0;
    for (size_t i = 0; i < m_blocks.size(); ++i)
    {
        double frac = static_cast<double>(m_blocks[i].sizeBytes) / totalBytes;
        widths[i] = std::max(minBlockWidth, static_cast<int>(frac * totalWidth));
        usedWidth += widths[i];
    }

    // Scale to fit
    if (usedWidth > totalWidth && !m_blocks.empty())
    {
        double scale = static_cast<double>(totalWidth) / usedWidth;
        usedWidth = 0;
        for (size_t i = 0; i < m_blocks.size(); ++i)
        {
            widths[i] = std::max(2, static_cast<int>(widths[i] * scale));
            usedWidth += widths[i];
        }
        // Adjust last block to fill
        if (usedWidth != totalWidth)
            widths.back() += (totalWidth - usedWidth);
    }

    int x = drawArea.x();
    for (size_t i = 0; i < m_blocks.size(); ++i)
    {
        const auto& blk = m_blocks[i];
        QRect blockRect(x, drawArea.y(), widths[i], drawArea.height());
        m_blockRects[i] = blockRect;

        // Fill
        QColor fillColor = blk.color;
        if (static_cast<int>(i) == m_hoveredBlock)
            fillColor = fillColor.lighter(120);
        if (static_cast<int>(i) == m_selectedBlock)
            fillColor = fillColor.lighter(140);

        painter.fillRect(blockRect.adjusted(1, 0, -1, 0), fillColor);

        // Border
        painter.setPen(QColor(40, 40, 40));
        painter.drawRect(blockRect.adjusted(1, 0, -1, 0));

        // Label text
        if (widths[i] > 30)
        {
            painter.setPen(Qt::white);
            QFont font = painter.font();
            font.setPointSize(8);
            painter.setFont(font);

            // Size string
            auto sizeStr = [](uint64_t bytes) -> QString {
                if (bytes >= 1099511627776ULL)
                    return QString("%1 TB").arg(bytes / 1099511627776.0, 0, 'f', 1);
                if (bytes >= 1073741824ULL)
                    return QString("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 1);
                if (bytes >= 1048576ULL)
                    return QString("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
                return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
            };

            QRect textRect = blockRect.adjusted(4, 2, -4, -2);
            QString topText = blk.label;
            QString botText = sizeStr(blk.sizeBytes);

            painter.drawText(textRect, Qt::AlignTop | Qt::AlignHCenter, topText);
            painter.drawText(textRect, Qt::AlignBottom | Qt::AlignHCenter, botText);
        }

        x += widths[i];
    }
}

void DiskMapWidget::mousePressEvent(QMouseEvent* event)
{
    int idx = blockAtPos(event->pos());

    if (event->button() == Qt::LeftButton)
    {
        m_selectedBlock = idx;
        if (idx >= 0 && idx < static_cast<int>(m_blocks.size()))
        {
            emit partitionClicked(m_blocks[idx].partitionIndex);
        }
        update();
    }
    else if (event->button() == Qt::RightButton)
    {
        m_selectedBlock = idx;
        if (idx >= 0 && idx < static_cast<int>(m_blocks.size()))
        {
            emit contextMenuRequested(m_blocks[idx].partitionIndex, event->globalPosition().toPoint());
        }
        update();
    }
}

void DiskMapWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    int idx = blockAtPos(event->pos());
    if (idx >= 0 && idx < static_cast<int>(m_blocks.size()))
    {
        emit partitionDoubleClicked(m_blocks[idx].partitionIndex);
    }
}

void DiskMapWidget::mouseMoveEvent(QMouseEvent* event)
{
    int idx = blockAtPos(event->pos());
    if (idx != m_hoveredBlock)
    {
        m_hoveredBlock = idx;
        update();
    }

    // Tooltip
    if (idx >= 0 && idx < static_cast<int>(m_blocks.size()))
    {
        const auto& blk = m_blocks[idx];
        auto fmtSize = [](uint64_t bytes) -> QString {
            if (bytes >= 1099511627776ULL)
                return QString("%1 TB").arg(bytes / 1099511627776.0, 0, 'f', 2);
            if (bytes >= 1073741824ULL)
                return QString("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 2);
            return QString("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
        };
        QString tip = QString("%1\nSize: %2\nFS: %3")
                          .arg(blk.label)
                          .arg(fmtSize(blk.sizeBytes))
                          .arg(filesystemShortName(blk.fsType));
        QToolTip::showText(event->globalPosition().toPoint(), tip, this);
    }
    else
    {
        QToolTip::hideText();
    }
}

int DiskMapWidget::blockAtPos(const QPoint& pos) const
{
    for (size_t i = 0; i < m_blockRects.size(); ++i)
    {
        if (m_blockRects[i].contains(pos))
            return static_cast<int>(i);
    }
    return -1;
}

QColor DiskMapWidget::colorForFilesystem(FilesystemType fs)
{
    switch (fs)
    {
    case FilesystemType::NTFS:       return QColor(52, 101, 164);
    case FilesystemType::FAT32:      return QColor(78, 154, 6);
    case FilesystemType::FAT16:      return QColor(78, 154, 6);
    case FilesystemType::FAT12:      return QColor(78, 154, 6);
    case FilesystemType::ExFAT:      return QColor(115, 210, 22);
    case FilesystemType::ReFS:       return QColor(32, 74, 135);
    case FilesystemType::Ext2:       return QColor(204, 0, 0);
    case FilesystemType::Ext3:       return QColor(204, 0, 0);
    case FilesystemType::Ext4:       return QColor(164, 0, 0);
    case FilesystemType::Btrfs:      return QColor(245, 121, 0);
    case FilesystemType::XFS:        return QColor(196, 160, 0);
    case FilesystemType::HFSPlus:    return QColor(117, 80, 123);
    case FilesystemType::APFS:       return QColor(173, 127, 168);
    case FilesystemType::SWAP_LINUX: return QColor(143, 89, 2);
    case FilesystemType::ISO9660:    return QColor(85, 87, 83);
    case FilesystemType::UDF:        return QColor(85, 87, 83);
    case FilesystemType::Unallocated: return QColor(80, 80, 80);
    default:                          return QColor(136, 138, 133);
    }
}

QString DiskMapWidget::filesystemShortName(FilesystemType fs)
{
    switch (fs)
    {
    case FilesystemType::NTFS:         return QStringLiteral("NTFS");
    case FilesystemType::FAT32:        return QStringLiteral("FAT32");
    case FilesystemType::FAT16:        return QStringLiteral("FAT16");
    case FilesystemType::FAT12:        return QStringLiteral("FAT12");
    case FilesystemType::ExFAT:        return QStringLiteral("exFAT");
    case FilesystemType::ReFS:         return QStringLiteral("ReFS");
    case FilesystemType::Ext2:         return QStringLiteral("ext2");
    case FilesystemType::Ext3:         return QStringLiteral("ext3");
    case FilesystemType::Ext4:         return QStringLiteral("ext4");
    case FilesystemType::Btrfs:        return QStringLiteral("Btrfs");
    case FilesystemType::XFS:          return QStringLiteral("XFS");
    case FilesystemType::ZFS:          return QStringLiteral("ZFS");
    case FilesystemType::HFSPlus:      return QStringLiteral("HFS+");
    case FilesystemType::APFS:         return QStringLiteral("APFS");
    case FilesystemType::SWAP_LINUX:   return QStringLiteral("Swap");
    case FilesystemType::ISO9660:      return QStringLiteral("ISO9660");
    case FilesystemType::UDF:          return QStringLiteral("UDF");
    case FilesystemType::Unallocated:  return QStringLiteral("Free");
    case FilesystemType::Unknown:      return QStringLiteral("Unknown");
    case FilesystemType::Raw:          return QStringLiteral("RAW");
    default:                           return QStringLiteral("Other");
    }
}

} // namespace spw
