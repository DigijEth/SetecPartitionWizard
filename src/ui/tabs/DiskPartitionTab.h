#pragma once

#include <QWidget>

class QSplitter;
class QTreeView;
class QTableView;

namespace spw
{

class DiskPartitionTab : public QWidget
{
    Q_OBJECT

public:
    explicit DiskPartitionTab(QWidget* parent = nullptr);
    ~DiskPartitionTab() override;

private:
    void setupUi();

    QSplitter* m_mainSplitter = nullptr;
    QSplitter* m_rightSplitter = nullptr;

    // Left panel: disk tree
    QTreeView* m_diskTree = nullptr;

    // Center: partition map (placeholder for DiskMapWidget)
    QWidget* m_diskMapPlaceholder = nullptr;

    // Bottom: partition detail table
    QTableView* m_partitionTable = nullptr;

    // Right: operation list
    QWidget* m_operationList = nullptr;
};

} // namespace spw
