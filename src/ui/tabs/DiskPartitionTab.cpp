#include "DiskPartitionTab.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QTreeView>
#include <QVBoxLayout>

namespace spw
{

DiskPartitionTab::DiskPartitionTab(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

DiskPartitionTab::~DiskPartitionTab() = default;

void DiskPartitionTab::setupUi()
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    m_mainSplitter = new QSplitter(Qt::Horizontal, this);

    // Left panel: Disk tree view
    auto* leftPanel = new QWidget();
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    auto* diskLabel = new QLabel(tr("Physical Disks"));
    diskLabel->setStyleSheet("font-weight: bold; padding: 4px;");
    leftLayout->addWidget(diskLabel);

    m_diskTree = new QTreeView();
    m_diskTree->setHeaderHidden(false);
    m_diskTree->setAlternatingRowColors(true);
    m_diskTree->setMinimumWidth(250);
    leftLayout->addWidget(m_diskTree);

    m_mainSplitter->addWidget(leftPanel);

    // Center + Bottom: partition map and table
    m_rightSplitter = new QSplitter(Qt::Vertical);

    // Disk map placeholder (will be replaced by DiskMapWidget)
    m_diskMapPlaceholder = new QWidget();
    m_diskMapPlaceholder->setMinimumHeight(120);
    auto* mapLayout = new QVBoxLayout(m_diskMapPlaceholder);
    auto* mapLabel = new QLabel(tr("Partition Map"));
    mapLabel->setAlignment(Qt::AlignCenter);
    mapLabel->setStyleSheet("color: #6c7086; font-size: 14px;");
    mapLayout->addWidget(mapLabel);
    m_rightSplitter->addWidget(m_diskMapPlaceholder);

    // Partition detail table
    m_partitionTable = new QTableView();
    m_partitionTable->setAlternatingRowColors(true);
    m_partitionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_partitionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_partitionTable->horizontalHeader()->setStretchLastSection(true);
    m_rightSplitter->addWidget(m_partitionTable);

    m_rightSplitter->setStretchFactor(0, 2);
    m_rightSplitter->setStretchFactor(1, 3);

    m_mainSplitter->addWidget(m_rightSplitter);

    // Right panel: pending operations list
    m_operationList = new QWidget();
    auto* opLayout = new QVBoxLayout(m_operationList);
    opLayout->setContentsMargins(0, 0, 0, 0);

    auto* opLabel = new QLabel(tr("Pending Operations"));
    opLabel->setStyleSheet("font-weight: bold; padding: 4px;");
    opLayout->addWidget(opLabel);

    auto* opListWidget = new QListWidget();
    opListWidget->setMinimumWidth(220);
    opLayout->addWidget(opListWidget);

    auto* buttonLayout = new QHBoxLayout();
    auto* applyBtn = new QPushButton(tr("Apply"));
    applyBtn->setObjectName("applyButton");
    auto* undoBtn = new QPushButton(tr("Undo"));
    auto* clearBtn = new QPushButton(tr("Clear"));
    buttonLayout->addWidget(applyBtn);
    buttonLayout->addWidget(undoBtn);
    buttonLayout->addWidget(clearBtn);
    opLayout->addLayout(buttonLayout);

    m_mainSplitter->addWidget(m_operationList);

    // Set splitter proportions
    m_mainSplitter->setStretchFactor(0, 1); // Disk tree
    m_mainSplitter->setStretchFactor(1, 3); // Center content
    m_mainSplitter->setStretchFactor(2, 1); // Operation list

    layout->addWidget(m_mainSplitter);
}

} // namespace spw
