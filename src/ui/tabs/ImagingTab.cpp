#include "ImagingTab.h"

#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace spw
{

ImagingTab::ImagingTab(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

ImagingTab::~ImagingTab() = default;

void ImagingTab::setupUi()
{
    auto* layout = new QVBoxLayout(this);

    // Clone Disk section
    auto* cloneGroup = new QGroupBox(tr("Clone Disk"));
    auto* cloneLayout = new QGridLayout(cloneGroup);
    cloneLayout->addWidget(new QLabel(tr("Source:")), 0, 0);
    cloneLayout->addWidget(new QComboBox(), 0, 1);
    cloneLayout->addWidget(new QLabel(tr("Target:")), 1, 0);
    cloneLayout->addWidget(new QComboBox(), 1, 1);
    auto* cloneBtn = new QPushButton(tr("Clone"));
    cloneBtn->setObjectName("applyButton");
    cloneLayout->addWidget(cloneBtn, 2, 1, Qt::AlignRight);
    layout->addWidget(cloneGroup);

    // Create Image section
    auto* imageGroup = new QGroupBox(tr("Create Disk/Media Image"));
    auto* imageLayout = new QGridLayout(imageGroup);
    imageLayout->addWidget(new QLabel(tr("Source:")), 0, 0);
    auto* sourceCombo = new QComboBox();
    sourceCombo->setToolTip(tr("Select disk, USB drive, SD card, or other media"));
    imageLayout->addWidget(sourceCombo, 0, 1);
    imageLayout->addWidget(new QLabel(tr("Output File:")), 1, 0);
    auto* outputLine = new QLineEdit();
    imageLayout->addWidget(outputLine, 1, 1);
    auto* browseBtn = new QPushButton(tr("Browse..."));
    imageLayout->addWidget(browseBtn, 1, 2);
    imageLayout->addWidget(new QLabel(tr("Compression:")), 2, 0);
    auto* compCombo = new QComboBox();
    compCombo->addItems({tr("None"), tr("Fast (zstd-1)"), tr("Default (zstd-3)"), tr("Best (zstd-9)")});
    imageLayout->addWidget(compCombo, 2, 1);
    auto* createImgBtn = new QPushButton(tr("Create Image"));
    createImgBtn->setObjectName("applyButton");
    imageLayout->addWidget(createImgBtn, 3, 1, Qt::AlignRight);
    layout->addWidget(imageGroup);

    // Restore Image section
    auto* restoreGroup = new QGroupBox(tr("Restore Image"));
    auto* restoreLayout = new QGridLayout(restoreGroup);
    restoreLayout->addWidget(new QLabel(tr("Image File:")), 0, 0);
    restoreLayout->addWidget(new QLineEdit(), 0, 1);
    restoreLayout->addWidget(new QPushButton(tr("Browse...")), 0, 2);
    restoreLayout->addWidget(new QLabel(tr("Target:")), 1, 0);
    restoreLayout->addWidget(new QComboBox(), 1, 1);
    auto* restoreBtn = new QPushButton(tr("Restore"));
    restoreBtn->setObjectName("applyButton");
    restoreLayout->addWidget(restoreBtn, 2, 1, Qt::AlignRight);
    layout->addWidget(restoreGroup);

    // Flash ISO/IMG section
    auto* flashGroup = new QGroupBox(tr("Flash ISO/IMG to USB"));
    auto* flashLayout = new QGridLayout(flashGroup);
    flashLayout->addWidget(new QLabel(tr("Image:")), 0, 0);
    flashLayout->addWidget(new QLineEdit(), 0, 1);
    flashLayout->addWidget(new QPushButton(tr("Browse...")), 0, 2);
    flashLayout->addWidget(new QLabel(tr("Target USB:")), 1, 0);
    flashLayout->addWidget(new QComboBox(), 1, 1);
    auto* flashBtn = new QPushButton(tr("Flash"));
    flashBtn->setObjectName("applyButton");
    flashLayout->addWidget(flashBtn, 2, 1, Qt::AlignRight);
    layout->addWidget(flashGroup);

    // Progress
    auto* progressBar = new QProgressBar();
    progressBar->setVisible(false);
    layout->addWidget(progressBar);

    layout->addStretch();
}

} // namespace spw
