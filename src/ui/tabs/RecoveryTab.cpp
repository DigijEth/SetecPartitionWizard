#include "RecoveryTab.h"

#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

namespace spw
{

RecoveryTab::RecoveryTab(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

RecoveryTab::~RecoveryTab() = default;

void RecoveryTab::setupUi()
{
    auto* layout = new QHBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal);

    // Left: recovery options
    auto* optionsPanel = new QWidget();
    auto* optLayout = new QVBoxLayout(optionsPanel);

    auto* typeGroup = new QGroupBox(tr("Recovery Type"));
    auto* typeLayout = new QVBoxLayout(typeGroup);
    auto* partRecoveryBtn = new QPushButton(tr("Partition Recovery"));
    partRecoveryBtn->setToolTip(tr("Scan for lost or deleted partitions"));
    auto* fileRecoveryBtn = new QPushButton(tr("File Recovery"));
    fileRecoveryBtn->setToolTip(tr("Recover files from damaged or formatted drives"));
    auto* mbrRepairBtn = new QPushButton(tr("MBR/GPT Repair"));
    mbrRepairBtn->setToolTip(tr("Rebuild partition table from filesystem superblocks"));
    typeLayout->addWidget(partRecoveryBtn);
    typeLayout->addWidget(fileRecoveryBtn);
    typeLayout->addWidget(mbrRepairBtn);
    optLayout->addWidget(typeGroup);

    auto* targetGroup = new QGroupBox(tr("Target Disk"));
    auto* targetLayout = new QVBoxLayout(targetGroup);
    auto* diskCombo = new QComboBox();
    diskCombo->addItem(tr("Select a disk..."));
    targetLayout->addWidget(diskCombo);
    optLayout->addWidget(targetGroup);

    auto* scanBtn = new QPushButton(tr("Start Scan"));
    scanBtn->setObjectName("applyButton");
    optLayout->addWidget(scanBtn);

    auto* progressBar = new QProgressBar();
    progressBar->setVisible(false);
    optLayout->addWidget(progressBar);

    optLayout->addStretch();
    splitter->addWidget(optionsPanel);

    // Right: results
    auto* resultsPanel = new QWidget();
    auto* resLayout = new QVBoxLayout(resultsPanel);

    auto* resLabel = new QLabel(tr("Recovery Results"));
    resLabel->setStyleSheet("font-weight: bold; padding: 4px;");
    resLayout->addWidget(resLabel);

    auto* resultsTable = new QTableWidget(0, 5);
    resultsTable->setHorizontalHeaderLabels(
        {tr("Type"), tr("Name/Label"), tr("Size"), tr("Filesystem"), tr("Confidence")});
    resultsTable->setAlternatingRowColors(true);
    resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    resLayout->addWidget(resultsTable);

    auto* recoverBtn = new QPushButton(tr("Recover Selected"));
    recoverBtn->setObjectName("applyButton");
    resLayout->addWidget(recoverBtn);

    splitter->addWidget(resultsPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    layout->addWidget(splitter);
}

} // namespace spw
