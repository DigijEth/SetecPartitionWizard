#pragma once

#include "core/common/Types.h"
#include "core/disk/DiskEnumerator.h"

#include <QWidget>
#include <QColor>
#include <vector>

namespace spw
{

class DiskMapWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DiskMapWidget(QWidget* parent = nullptr);

    // Set the disk to display
    void setDisk(const DiskInfo& disk,
                 const std::vector<PartitionInfo>& partitions,
                 const std::vector<VolumeInfo>& volumes);

    void clear();

    QSize minimumSizeHint() const override { return QSize(400, 80); }
    QSize sizeHint() const override { return QSize(600, 100); }

signals:
    void partitionClicked(int partitionIndex);
    void partitionDoubleClicked(int partitionIndex);
    void contextMenuRequested(int partitionIndex, const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    struct Block
    {
        int partitionIndex = -1; // -1 = unallocated
        uint64_t startBytes = 0;
        uint64_t sizeBytes = 0;
        FilesystemType fsType = FilesystemType::Unknown;
        QString label;
        wchar_t driveLetter = L'\0';
        QColor color;
    };

    void rebuildBlocks();
    int blockAtPos(const QPoint& pos) const;
    static QColor colorForFilesystem(FilesystemType fs);
    static QString filesystemShortName(FilesystemType fs);

    DiskInfo m_disk;
    std::vector<PartitionInfo> m_partitions;
    std::vector<VolumeInfo> m_volumes;
    std::vector<Block> m_blocks;
    int m_hoveredBlock = -1;
    int m_selectedBlock = -1;

    // Cached block rectangles from last paint
    std::vector<QRect> m_blockRects;
};

} // namespace spw
