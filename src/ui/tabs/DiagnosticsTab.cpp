#include "DiagnosticsTab.h"

#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

namespace spw
{

DiagnosticsTab::DiagnosticsTab(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

DiagnosticsTab::~DiagnosticsTab() = default;

void DiagnosticsTab::setupUi()
{
    auto* layout = new QVBoxLayout(this);

    // Disk selector
    auto* selectorLayout = new QHBoxLayout();
    selectorLayout->addWidget(new QLabel(tr("Select Disk:")));
    auto* diskCombo = new QComboBox();
    selectorLayout->addWidget(diskCombo, 1);
    auto* refreshBtn = new QPushButton(tr("Refresh"));
    selectorLayout->addWidget(refreshBtn);
    layout->addLayout(selectorLayout);

    auto* splitter = new QSplitter(Qt::Horizontal);

    // S.M.A.R.T. panel
    auto* smartGroup = new QGroupBox(tr("S.M.A.R.T. Health"));
    auto* smartLayout = new QVBoxLayout(smartGroup);

    auto* healthLabel = new QLabel(tr("Overall Health: —"));
    healthLabel->setStyleSheet("font-size: 16px; font-weight: bold; padding: 8px;");
    smartLayout->addWidget(healthLabel);

    auto* smartTable = new QTableWidget(0, 5);
    smartTable->setHorizontalHeaderLabels(
        {tr("ID"), tr("Attribute"), tr("Value"), tr("Worst"), tr("Threshold")});
    smartTable->setAlternatingRowColors(true);
    smartLayout->addWidget(smartTable);

    splitter->addWidget(smartGroup);

    // Benchmark & Surface Scan panel
    auto* rightPanel = new QWidget();
    auto* rightLayout = new QVBoxLayout(rightPanel);

    auto* benchGroup = new QGroupBox(tr("Benchmark"));
    auto* benchLayout = new QVBoxLayout(benchGroup);
    auto* benchResults = new QLabel(
        tr("Sequential Read:  — MB/s\n"
           "Sequential Write: — MB/s\n"
           "Random 4K Read:   — IOPS\n"
           "Random 4K Write:  — IOPS"));
    benchResults->setStyleSheet("font-family: monospace; padding: 8px;");
    benchLayout->addWidget(benchResults);
    auto* benchBtn = new QPushButton(tr("Run Benchmark"));
    benchBtn->setObjectName("applyButton");
    benchLayout->addWidget(benchBtn);
    rightLayout->addWidget(benchGroup);

    auto* scanGroup = new QGroupBox(tr("Surface Scan"));
    auto* scanLayout = new QVBoxLayout(scanGroup);
    auto* scanInfo = new QLabel(tr("Sectors: — total, — bad, — pending"));
    scanLayout->addWidget(scanInfo);
    auto* scanProgress = new QProgressBar();
    scanLayout->addWidget(scanProgress);
    auto* scanBtn = new QPushButton(tr("Start Surface Scan"));
    scanBtn->setObjectName("applyButton");
    scanLayout->addWidget(scanBtn);
    rightLayout->addWidget(scanGroup);

    rightLayout->addStretch();
    splitter->addWidget(rightPanel);

    layout->addWidget(splitter);
}

} // namespace spw
