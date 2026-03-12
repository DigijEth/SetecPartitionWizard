#include "MaintenanceTab.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace spw
{

MaintenanceTab::MaintenanceTab(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

MaintenanceTab::~MaintenanceTab() = default;

void MaintenanceTab::setupUi()
{
    auto* layout = new QVBoxLayout(this);

    // Secure Erase section
    auto* eraseGroup = new QGroupBox(tr("Secure Erase"));
    auto* eraseLayout = new QGridLayout(eraseGroup);

    eraseLayout->addWidget(new QLabel(tr("Target Disk:")), 0, 0);
    eraseLayout->addWidget(new QComboBox(), 0, 1);

    eraseLayout->addWidget(new QLabel(tr("Erase Method:")), 1, 0);
    auto* methodWidget = new QWidget();
    auto* methodLayout = new QVBoxLayout(methodWidget);
    methodLayout->setContentsMargins(0, 0, 0, 0);
    auto* zeroPass = new QRadioButton(tr("Zero fill (1 pass) — Fast"));
    zeroPass->setChecked(true);
    auto* dod3Pass = new QRadioButton(tr("DoD 5220.22-M (3 passes) — Standard"));
    auto* dod7Pass = new QRadioButton(tr("DoD 5220.22-M ECE (7 passes) — Enhanced"));
    auto* gutmann = new QRadioButton(tr("Gutmann method (35 passes) — Maximum"));
    auto* customPass = new QRadioButton(tr("Custom:"));
    auto* customSpin = new QSpinBox();
    customSpin->setRange(1, 99);
    customSpin->setValue(3);
    customSpin->setEnabled(false);
    auto* customRow = new QHBoxLayout();
    customRow->addWidget(customPass);
    customRow->addWidget(customSpin);
    customRow->addWidget(new QLabel(tr("passes")));
    customRow->addStretch();

    methodLayout->addWidget(zeroPass);
    methodLayout->addWidget(dod3Pass);
    methodLayout->addWidget(dod7Pass);
    methodLayout->addWidget(gutmann);
    methodLayout->addLayout(customRow);
    eraseLayout->addWidget(methodWidget, 1, 1);

    auto* verifyCheck = new QCheckBox(tr("Verify after erase"));
    verifyCheck->setChecked(true);
    eraseLayout->addWidget(verifyCheck, 2, 1);

    auto* eraseBtn = new QPushButton(tr("Secure Erase"));
    eraseBtn->setObjectName("cancelButton");
    eraseBtn->setToolTip(tr("WARNING: This permanently destroys all data on the selected disk!"));
    eraseLayout->addWidget(eraseBtn, 3, 1, Qt::AlignRight);

    layout->addWidget(eraseGroup);

    // Boot Repair section
    auto* bootGroup = new QGroupBox(tr("Boot Repair"));
    auto* bootLayout = new QVBoxLayout(bootGroup);

    auto* bootInfo = new QLabel(
        tr("Repair boot configuration for Windows and other operating systems."));
    bootLayout->addWidget(bootInfo);

    auto* mbrRepairBtn = new QPushButton(tr("Repair MBR"));
    mbrRepairBtn->setToolTip(tr("Rewrite the Master Boot Record with a standard boot loader"));
    auto* gptRepairBtn = new QPushButton(tr("Repair GPT"));
    gptRepairBtn->setToolTip(tr("Rebuild GPT headers and verify partition entries"));
    auto* bcdRepairBtn = new QPushButton(tr("Repair Windows BCD"));
    bcdRepairBtn->setToolTip(tr("Rebuild the Windows Boot Configuration Data store"));
    auto* bootloaderBtn = new QPushButton(tr("Reinstall Bootloader"));
    bootloaderBtn->setToolTip(tr("Reinstall the bootloader to the selected disk's boot sector"));

    bootLayout->addWidget(mbrRepairBtn);
    bootLayout->addWidget(gptRepairBtn);
    bootLayout->addWidget(bcdRepairBtn);
    bootLayout->addWidget(bootloaderBtn);

    layout->addWidget(bootGroup);

    // Progress
    auto* progressBar = new QProgressBar();
    progressBar->setVisible(false);
    layout->addWidget(progressBar);

    layout->addStretch();
}

} // namespace spw
