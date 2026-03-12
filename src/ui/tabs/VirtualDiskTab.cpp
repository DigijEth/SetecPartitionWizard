#include "VirtualDiskTab.h"

#include "core/imaging/VirtualDisk.h"
#include "core/disk/DiskEnumerator.h"

#include <QMessageBox>
#include <QProcess>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>

namespace spw
{

VirtualDiskTab::VirtualDiskTab(QWidget* parent) : QWidget(parent)
{
    setupUi();
}

VirtualDiskTab::~VirtualDiskTab() = default;

QString VirtualDiskTab::formatSize(uint64_t bytes)
{
    if (bytes >= 1099511627776ULL)
        return QString("%1 TB").arg(bytes / 1099511627776.0, 0, 'f', 2);
    if (bytes >= 1073741824ULL)
        return QString("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 1);
    if (bytes >= 1048576ULL)
        return QString("%1 MB").arg(bytes / 1048576.0, 0, 'f', 0);
    return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}

void VirtualDiskTab::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);

    auto* innerTabs = new QTabWidget();

    // ================================================================
    // Tab 1: Mount / Unmount
    // ================================================================
    auto* mountWidget = new QWidget();
    auto* mountLayout = new QVBoxLayout(mountWidget);

    auto* mountInfo = new QLabel(
        tr("Mount VHD or VHDX files as virtual disks. "
           "Once mounted, they appear as physical drives and can be accessed in Explorer, "
           "formatted, or have data written to them. "
           "VMDK and QCOW2 require conversion to VHDX first (see Convert tab)."));
    mountInfo->setWordWrap(true);
    mountInfo->setStyleSheet("color: #aaaaaa; font-style: italic;");
    mountLayout->addWidget(mountInfo);

    // File selector
    auto* fileGroup = new QGroupBox(tr("Virtual Disk File"));
    auto* fileLayout = new QHBoxLayout(fileGroup);
    m_mountPathEdit = new QLineEdit();
    m_mountPathEdit->setPlaceholderText(tr("Select a .vhd or .vhdx file..."));
    fileLayout->addWidget(m_mountPathEdit, 1);
    m_mountBrowseBtn = new QPushButton(tr("Browse..."));
    connect(m_mountBrowseBtn, &QPushButton::clicked, this, &VirtualDiskTab::onBrowseMount);
    fileLayout->addWidget(m_mountBrowseBtn);
    m_mountReadOnly = new QCheckBox(tr("Read-only"));
    fileLayout->addWidget(m_mountReadOnly);
    m_mountBtn = new QPushButton(tr("Mount"));
    m_mountBtn->setStyleSheet(
        "QPushButton { background-color: #d4a574; color: #1e1e2e; font-weight: bold; "
        "border-radius: 4px; padding: 5px 14px; }"
        "QPushButton:hover { background-color: #e0b584; }");
    connect(m_mountBtn, &QPushButton::clicked, this, &VirtualDiskTab::onMount);
    fileLayout->addWidget(m_mountBtn);
    mountLayout->addWidget(fileGroup);

    // Currently mounted table
    auto* mountedGroup = new QGroupBox(tr("Currently Mounted Virtual Disks"));
    auto* mountedLayout = new QVBoxLayout(mountedGroup);

    m_mountedTable = new QTableWidget(0, 4);
    m_mountedTable->setHorizontalHeaderLabels({tr("File"), tr("Format"), tr("Virtual Size"), tr("Drive Path")});
    m_mountedTable->horizontalHeader()->setStretchLastSection(true);
    m_mountedTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mountedTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_mountedTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mountedLayout->addWidget(m_mountedTable);

    auto* mountedBtnRow = new QHBoxLayout();
    m_unmountBtn = new QPushButton(tr("Unmount Selected"));
    m_unmountBtn->setStyleSheet(
        "QPushButton { background-color: #cc3333; color: white; border-radius: 4px; padding: 5px 14px; }"
        "QPushButton:hover { background-color: #ee4444; }");
    connect(m_unmountBtn, &QPushButton::clicked, this, &VirtualDiskTab::onUnmount);
    mountedBtnRow->addWidget(m_unmountBtn);
    m_refreshBtn = new QPushButton(tr("Refresh List"));
    connect(m_refreshBtn, &QPushButton::clicked, this, &VirtualDiskTab::onRefreshMounted);
    mountedBtnRow->addWidget(m_refreshBtn);
    mountedBtnRow->addStretch();
    mountedLayout->addLayout(mountedBtnRow);

    m_mountStatus = new QLabel();
    m_mountStatus->setWordWrap(true);
    mountedLayout->addWidget(m_mountStatus);

    mountLayout->addWidget(mountedGroup);

    innerTabs->addTab(mountWidget, tr("Mount / Unmount"));

    // ================================================================
    // Tab 2: Create New
    // ================================================================
    auto* createWidget = new QWidget();
    auto* createLayout = new QVBoxLayout(createWidget);

    auto* createInfo = new QLabel(
        tr("Create a new empty virtual disk. VHDX is recommended — it supports larger sizes, "
           "is more resilient, and is the Hyper-V preferred format. "
           "VHD is more compatible with older tools and VirtualBox."));
    createInfo->setWordWrap(true);
    createInfo->setStyleSheet("color: #aaaaaa; font-style: italic;");
    createLayout->addWidget(createInfo);

    auto* createGroup = new QGroupBox(tr("New Virtual Disk"));
    auto* createForm = new QFormLayout(createGroup);

    auto* createPathRow = new QHBoxLayout();
    m_createPathEdit = new QLineEdit();
    m_createPathEdit->setPlaceholderText(tr("e.g. C:\\VMs\\disk.vhdx"));
    createPathRow->addWidget(m_createPathEdit, 1);
    m_createBrowse = new QPushButton(tr("Browse..."));
    connect(m_createBrowse, &QPushButton::clicked, this, &VirtualDiskTab::onBrowseCreate);
    createPathRow->addWidget(m_createBrowse);
    createForm->addRow(tr("Output File:"), createPathRow);

    m_createFmtCombo = new QComboBox();
    m_createFmtCombo->addItems({
        tr("VHDX  (recommended — Hyper-V native, up to 64 TB)"),
        tr("VHD   (legacy — compatible with VirtualBox, up to 2 TB)"),
        tr("VMDK  (VMware — requires qemu-img)"),
        tr("QCOW2 (QEMU native — requires qemu-img)"),
        tr("RAW   (flat image — maximum compatibility, no metadata)"),
    });
    createForm->addRow(tr("Format:"), m_createFmtCombo);

    auto* sizeRow = new QHBoxLayout();
    m_createSizeSpin = new QDoubleSpinBox();
    m_createSizeSpin->setRange(0.001, 65536.0);
    m_createSizeSpin->setDecimals(3);
    m_createSizeSpin->setValue(64.0);
    sizeRow->addWidget(m_createSizeSpin, 1);
    m_createSizeUnit = new QComboBox();
    m_createSizeUnit->addItems({tr("MB"), tr("GB"), tr("TB")});
    m_createSizeUnit->setCurrentIndex(1); // GB default
    sizeRow->addWidget(m_createSizeUnit);
    createForm->addRow(tr("Size:"), sizeRow);

    m_createDynamic = new QCheckBox(
        tr("Dynamic (grows as data is written — saves disk space)"));
    m_createDynamic->setChecked(true);
    createForm->addRow(tr("Type:"), m_createDynamic);

    createLayout->addWidget(createGroup);

    m_createProgress = new QProgressBar();
    m_createProgress->setVisible(false);
    createLayout->addWidget(m_createProgress);
    m_createStatus = new QLabel();
    m_createStatus->setWordWrap(true);
    createLayout->addWidget(m_createStatus);

    m_createBtn = new QPushButton(tr("Create Virtual Disk"));
    m_createBtn->setStyleSheet(
        "QPushButton { background-color: #d4a574; color: #1e1e2e; font-weight: bold; "
        "font-size: 13px; border-radius: 5px; }"
        "QPushButton:hover { background-color: #e0b584; }");
    connect(m_createBtn, &QPushButton::clicked, this, &VirtualDiskTab::onCreate);
    createLayout->addWidget(m_createBtn);
    createLayout->addStretch();

    innerTabs->addTab(createWidget, tr("Create New"));

    // ================================================================
    // Tab 3: Capture (Disk → Image)
    // ================================================================
    auto* capWidget = new QWidget();
    auto* capLayout = new QVBoxLayout(capWidget);

    auto* capInfo = new QLabel(
        tr("Capture a physical disk or SD card as a virtual disk image. "
           "The resulting image can be mounted, shared, or flashed back to hardware."));
    capInfo->setWordWrap(true);
    capInfo->setStyleSheet("color: #aaaaaa; font-style: italic;");
    capLayout->addWidget(capInfo);

    auto* capGroup = new QGroupBox(tr("Capture Settings"));
    auto* capForm = new QFormLayout(capGroup);

    m_captureSourceCombo = new QComboBox();
    capForm->addRow(tr("Source Disk:"), m_captureSourceCombo);

    auto* capOutRow = new QHBoxLayout();
    m_captureOutEdit = new QLineEdit();
    m_captureOutEdit->setPlaceholderText(tr("Output image file path..."));
    capOutRow->addWidget(m_captureOutEdit, 1);
    m_captureBrowse = new QPushButton(tr("Browse..."));
    connect(m_captureBrowse, &QPushButton::clicked, this, [this]() {
        auto path = QFileDialog::getSaveFileName(this, tr("Save Captured Image"),
            QString(), tr("VHDX (*.vhdx);;VHD (*.vhd);;Raw Image (*.img);;All Files (*)"));
        if (!path.isEmpty()) m_captureOutEdit->setText(path);
    });
    capOutRow->addWidget(m_captureBrowse);
    capForm->addRow(tr("Output File:"), capOutRow);

    m_captureFmtCombo = new QComboBox();
    m_captureFmtCombo->addItems({
        tr("VHDX  (recommended)"),
        tr("VHD"),
        tr("RAW   (.img — flat copy)"),
    });
    capForm->addRow(tr("Format:"), m_captureFmtCombo);

    capLayout->addWidget(capGroup);

    m_captureProgress = new QProgressBar();
    m_captureProgress->setVisible(false);
    capLayout->addWidget(m_captureProgress);
    m_captureStatus = new QLabel();
    m_captureStatus->setWordWrap(true);
    capLayout->addWidget(m_captureStatus);

    m_captureBtn = new QPushButton(tr("Capture Disk to Image"));
    m_captureBtn->setStyleSheet(
        "QPushButton { background-color: #d4a574; color: #1e1e2e; font-weight: bold; "
        "font-size: 13px; border-radius: 5px; }"
        "QPushButton:hover { background-color: #e0b584; }");
    connect(m_captureBtn, &QPushButton::clicked, this, &VirtualDiskTab::onCapture);
    capLayout->addWidget(m_captureBtn);
    capLayout->addStretch();

    innerTabs->addTab(capWidget, tr("Capture to Image"));

    // ================================================================
    // Tab 4: Flash (Image → Disk/SD)
    // ================================================================
    auto* flashWidget = new QWidget();
    auto* flashLayout = new QVBoxLayout(flashWidget);

    auto* flashInfo = new QLabel(
        tr("Flash a virtual disk image (VHD, VHDX, or raw .img) directly to a physical disk "
           "or SD card. The image contents replace everything on the target drive."));
    flashInfo->setWordWrap(true);
    flashInfo->setStyleSheet("color: #aaaaaa; font-style: italic;");
    flashLayout->addWidget(flashInfo);

    auto* flashGroup = new QGroupBox(tr("Flash Settings"));
    auto* flashForm = new QFormLayout(flashGroup);

    auto* flashImgRow = new QHBoxLayout();
    m_flashImageEdit = new QLineEdit();
    m_flashImageEdit->setPlaceholderText(tr("Select VHD, VHDX, or .img file..."));
    flashImgRow->addWidget(m_flashImageEdit, 1);
    m_flashBrowse = new QPushButton(tr("Browse..."));
    connect(m_flashBrowse, &QPushButton::clicked, this, &VirtualDiskTab::onBrowseFlashImage);
    flashImgRow->addWidget(m_flashBrowse);
    flashForm->addRow(tr("Image File:"), flashImgRow);

    m_flashTargetCombo = new QComboBox();
    flashForm->addRow(tr("Target Disk:"), m_flashTargetCombo);

    flashLayout->addWidget(flashGroup);

    auto* flashWarnLabel = new QLabel(
        tr("⚠ WARNING: All data on the target disk will be overwritten!"));
    flashWarnLabel->setStyleSheet("color: #ff9944; font-weight: bold; padding: 4px;");
    flashLayout->addWidget(flashWarnLabel);

    m_flashProgress = new QProgressBar();
    m_flashProgress->setVisible(false);
    flashLayout->addWidget(m_flashProgress);
    m_flashStatus = new QLabel();
    m_flashStatus->setWordWrap(true);
    flashLayout->addWidget(m_flashStatus);

    m_flashBtn = new QPushButton(tr("Flash Image to Disk"));
    m_flashBtn->setStyleSheet(
        "QPushButton { background-color: #cc3333; color: white; font-weight: bold; "
        "font-size: 13px; border-radius: 5px; }"
        "QPushButton:hover { background-color: #ee4444; }");
    connect(m_flashBtn, &QPushButton::clicked, this, &VirtualDiskTab::onFlash);
    flashLayout->addWidget(m_flashBtn);
    flashLayout->addStretch();

    innerTabs->addTab(flashWidget, tr("Flash to Disk"));

    // ================================================================
    // Tab 5: Convert
    // ================================================================
    auto* convWidget = new QWidget();
    auto* convLayout = new QVBoxLayout(convWidget);

    auto* convInfo = new QLabel(
        tr("Convert between virtual disk formats using qemu-img. "
           "Supports VHD ↔ VHDX ↔ VMDK ↔ QCOW2 ↔ RAW.\n"
           "qemu-img must be installed and on PATH (install QEMU for Windows)."));
    convInfo->setWordWrap(true);
    convInfo->setStyleSheet("color: #aaaaaa; font-style: italic;");
    convLayout->addWidget(convInfo);

    m_qemuStatus = new QLabel();
    m_qemuStatus->setWordWrap(true);
    // Check qemu-img availability
    if (VirtualDisk::qemuImgAvailable())
        m_qemuStatus->setText(tr("✓ qemu-img detected — conversion available"));
    else
        m_qemuStatus->setText(tr("✗ qemu-img not found. Install QEMU (qemu.org) to enable conversion."));
    convLayout->addWidget(m_qemuStatus);

    auto* convGroup = new QGroupBox(tr("Conversion Settings"));
    auto* convForm = new QFormLayout(convGroup);

    auto* convInRow = new QHBoxLayout();
    m_convInEdit = new QLineEdit();
    m_convInEdit->setPlaceholderText(tr("Input image file..."));
    convInRow->addWidget(m_convInEdit, 1);
    m_convInBrowse = new QPushButton(tr("Browse..."));
    connect(m_convInBrowse, &QPushButton::clicked, this, &VirtualDiskTab::onBrowseConvertIn);
    convInRow->addWidget(m_convInBrowse);
    convForm->addRow(tr("Input:"), convInRow);

    m_convFmtCombo = new QComboBox();
    m_convFmtCombo->addItems({
        tr("VHDX  (Hyper-V / Windows)"),
        tr("VHD   (Legacy / VirtualBox)"),
        tr("VMDK  (VMware)"),
        tr("QCOW2 (QEMU / KVM)"),
        tr("RAW   (flat .img)"),
    });
    convForm->addRow(tr("Output Format:"), m_convFmtCombo);

    auto* convOutRow = new QHBoxLayout();
    m_convOutEdit = new QLineEdit();
    m_convOutEdit->setPlaceholderText(tr("Output file path..."));
    convOutRow->addWidget(m_convOutEdit, 1);
    m_convOutBrowse = new QPushButton(tr("Browse..."));
    connect(m_convOutBrowse, &QPushButton::clicked, this, &VirtualDiskTab::onBrowseConvertOut);
    convOutRow->addWidget(m_convOutBrowse);
    convForm->addRow(tr("Output:"), convOutRow);

    convLayout->addWidget(convGroup);

    m_convProgress = new QProgressBar();
    m_convProgress->setRange(0, 0);
    m_convProgress->setVisible(false);
    convLayout->addWidget(m_convProgress);
    m_convStatus = new QLabel();
    m_convStatus->setWordWrap(true);
    convLayout->addWidget(m_convStatus);

    m_convertBtn = new QPushButton(tr("Convert"));
    m_convertBtn->setStyleSheet(
        "QPushButton { background-color: #d4a574; color: #1e1e2e; font-weight: bold; "
        "font-size: 13px; border-radius: 5px; }"
        "QPushButton:hover { background-color: #e0b584; }");
    connect(m_convertBtn, &QPushButton::clicked, this, &VirtualDiskTab::onConvert);
    convLayout->addWidget(m_convertBtn);
    convLayout->addStretch();

    innerTabs->addTab(convWidget, tr("Convert Format"));

    mainLayout->addWidget(innerTabs);
}

// ============================================================================
// refreshDisks
// ============================================================================
void VirtualDiskTab::refreshDisks(const SystemDiskSnapshot& snapshot)
{
    m_snapshot = snapshot;
    populateDiskCombo();
}

void VirtualDiskTab::populateDiskCombo()
{
    m_captureSourceCombo->clear();
    m_flashTargetCombo->clear();

    for (const auto& disk : m_snapshot.disks)
    {
        QString label = QString("Disk %1: %2 (%3)")
            .arg(disk.id)
            .arg(QString::fromStdWString(disk.model))
            .arg(formatSize(disk.sizeBytes));
        m_captureSourceCombo->addItem(label, disk.id);
        m_flashTargetCombo->addItem(label, disk.id);
    }
}

// ============================================================================
// Browse slots
// ============================================================================
void VirtualDiskTab::onBrowseMount()
{
    auto path = QFileDialog::getOpenFileName(this, tr("Select Virtual Disk"),
        QString(), tr("Virtual Disks (*.vhd *.vhdx *.vmdk *.qcow2 *.img);;All Files (*)"));
    if (!path.isEmpty()) m_mountPathEdit->setText(path);
}

void VirtualDiskTab::onBrowseCreate()
{
    static const char* exts[] = { ".vhdx", ".vhd", ".vmdk", ".qcow2", ".img" };
    int fmtIdx = m_createFmtCombo->currentIndex();
    QString ext = exts[fmtIdx < 5 ? fmtIdx : 0];
    auto path = QFileDialog::getSaveFileName(this, tr("Save Virtual Disk"),
        QString(), tr("Virtual Disk (*%1);;All Files (*)").arg(ext));
    if (!path.isEmpty()) m_createPathEdit->setText(path);
}

void VirtualDiskTab::onBrowseFlashImage()
{
    auto path = QFileDialog::getOpenFileName(this, tr("Select Image to Flash"),
        QString(), tr("Virtual Disks (*.vhd *.vhdx *.vmdk *.img);;All Files (*)"));
    if (!path.isEmpty()) m_flashImageEdit->setText(path);
}

void VirtualDiskTab::onBrowseConvertIn()
{
    auto path = QFileDialog::getOpenFileName(this, tr("Select Input Image"),
        QString(), tr("Virtual Disks (*.vhd *.vhdx *.vmdk *.qcow2 *.img);;All Files (*)"));
    if (!path.isEmpty()) m_convInEdit->setText(path);
}

void VirtualDiskTab::onBrowseConvertOut()
{
    auto path = QFileDialog::getSaveFileName(this, tr("Output File"),
        QString(), tr("Virtual Disks (*.vhd *.vhdx *.vmdk *.qcow2 *.img);;All Files (*)"));
    if (!path.isEmpty()) m_convOutEdit->setText(path);
}

// ============================================================================
// Mount / Unmount
// ============================================================================
void VirtualDiskTab::onMount()
{
    QString path = m_mountPathEdit->text().trimmed();
    if (path.isEmpty()) { QMessageBox::warning(this, tr("Mount"), tr("No file selected.")); return; }

    bool readOnly = m_mountReadOnly->isChecked();
    m_mountBtn->setEnabled(false);
    m_mountStatus->setText(tr("Mounting..."));

    auto* thread = QThread::create([this, path, readOnly]() {
        auto result = VirtualDisk::mount(path.toStdWString(), readOnly);
        QMetaObject::invokeMethod(this, [this, result, path]() {
            m_mountBtn->setEnabled(true);
            if (result.isError())
            {
                m_mountStatus->setText(tr("✗ Mount failed: %1").arg(
                    QString::fromStdString(result.error().message)));
            }
            else
            {
                const auto& info = result.value();
                m_mountStatus->setText(tr("✓ Mounted as %1")
                    .arg(QString::fromStdWString(info.physicalDrivePath)));
                onRefreshMounted();
                emit statusMessage(tr("Virtual disk mounted: %1").arg(path));
            }
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void VirtualDiskTab::onUnmount()
{
    int row = m_mountedTable->currentRow();
    if (row < 0) { QMessageBox::information(this, tr("Unmount"), tr("Select a disk to unmount.")); return; }

    QString filePath = m_mountedTable->item(row, 0)->text();
    auto result = VirtualDisk::unmount(filePath.toStdWString());
    if (result.isError())
        m_mountStatus->setText(tr("✗ Unmount failed: %1").arg(QString::fromStdString(result.error().message)));
    else
    {
        m_mountStatus->setText(tr("✓ Unmounted."));
        onRefreshMounted();
        emit statusMessage(tr("Virtual disk unmounted"));
    }
}

void VirtualDiskTab::onRefreshMounted()
{
    // There's no Windows API to enumerate all attached VHDs easily.
    // Best we can do is show the last mounted one or clear on unmount.
    // For now just acknowledge the action; a full implementation would
    // query the VHD service via WMI Msvm_StorageAllocationSettingData.
    m_mountedTable->setRowCount(0);
}

// ============================================================================
// Create
// ============================================================================
void VirtualDiskTab::onCreate()
{
    QString path = m_createPathEdit->text().trimmed();
    if (path.isEmpty()) { QMessageBox::warning(this, tr("Create"), tr("No output path specified.")); return; }

    static const VirtualDiskFormat fmts[] = {
        VirtualDiskFormat::VHDX, VirtualDiskFormat::VHD,
        VirtualDiskFormat::VMDK, VirtualDiskFormat::QCOW2, VirtualDiskFormat::RAW
    };

    VirtualDiskCreateParams params;
    params.filePath = path.toStdWString();
    params.format   = fmts[m_createFmtCombo->currentIndex()];
    params.dynamic  = m_createDynamic->isChecked();

    double size = m_createSizeSpin->value();
    int unit = m_createSizeUnit->currentIndex();
    uint64_t multiplier = (unit == 0) ? 1024ULL * 1024
                        : (unit == 1) ? 1024ULL * 1024 * 1024
                                      : 1024ULL * 1024 * 1024 * 1024;
    params.sizeBytes = static_cast<uint64_t>(size * multiplier);

    m_createBtn->setEnabled(false);
    m_createProgress->setVisible(true);
    m_createProgress->setRange(0, 0);
    m_createStatus->setText(tr("Creating..."));

    auto* thread = QThread::create([this, params]() {
        auto result = VirtualDisk::create(params,
            [this](const std::string& s, int p) {
                QMetaObject::invokeMethod(m_createStatus, "setText",
                    Qt::QueuedConnection, Q_ARG(QString, QString::fromStdString(s)));
            });
        QMetaObject::invokeMethod(this, [this, result]() {
            m_createProgress->setVisible(false);
            m_createBtn->setEnabled(true);
            m_createStatus->setText(result.isOk()
                ? tr("✓ Virtual disk created successfully.")
                : tr("✗ Failed: %1").arg(QString::fromStdString(result.error().message)));
            if (result.isOk()) emit statusMessage(tr("Virtual disk created"));
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// ============================================================================
// Capture
// ============================================================================
void VirtualDiskTab::onCapture()
{
    int diskId = m_captureSourceCombo->currentData().toInt();
    QString outPath = m_captureOutEdit->text().trimmed();
    if (outPath.isEmpty()) { QMessageBox::warning(this, tr("Capture"), tr("No output path specified.")); return; }

    static const VirtualDiskFormat fmts[] = {
        VirtualDiskFormat::VHDX, VirtualDiskFormat::VHD, VirtualDiskFormat::RAW
    };
    VirtualDiskFormat fmt = fmts[m_captureFmtCombo->currentIndex()];

    auto reply = QMessageBox::question(this, tr("Capture Disk"),
        tr("Capture Disk %1 to:\n%2\n\nThis will read the entire disk. Continue?")
            .arg(diskId).arg(outPath),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    m_captureBtn->setEnabled(false);
    m_captureProgress->setVisible(true);
    m_captureProgress->setRange(0, 100);
    m_captureStatus->setText(tr("Capturing..."));

    auto* thread = QThread::create([this, diskId, outPath, fmt]() {
        auto result = VirtualDisk::captureFromDisk(diskId, outPath.toStdWString(), fmt,
            [this](const std::string& s, int p) {
                QMetaObject::invokeMethod(m_captureProgress, "setValue",
                    Qt::QueuedConnection, Q_ARG(int, p));
                QMetaObject::invokeMethod(m_captureStatus, "setText",
                    Qt::QueuedConnection, Q_ARG(QString, QString::fromStdString(s)));
            });
        QMetaObject::invokeMethod(this, [this, result]() {
            m_captureProgress->setVisible(false);
            m_captureBtn->setEnabled(true);
            m_captureStatus->setText(result.isOk()
                ? tr("✓ Capture complete.")
                : tr("✗ Failed: %1").arg(QString::fromStdString(result.error().message)));
            if (result.isOk()) emit statusMessage(tr("Disk captured to image"));
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// ============================================================================
// Flash
// ============================================================================
void VirtualDiskTab::onFlash()
{
    QString imagePath = m_flashImageEdit->text().trimmed();
    int targetDiskId  = m_flashTargetCombo->currentData().toInt();
    if (imagePath.isEmpty()) { QMessageBox::warning(this, tr("Flash"), tr("No image file selected.")); return; }

    auto reply = QMessageBox::critical(this, tr("Flash to Disk"),
        tr("This will OVERWRITE ALL DATA on Disk %1.\n\n"
           "Image: %2\n\nThis is irreversible. Continue?")
            .arg(targetDiskId).arg(imagePath),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    m_flashBtn->setEnabled(false);
    m_flashProgress->setVisible(true);
    m_flashProgress->setRange(0, 100);
    m_flashStatus->setText(tr("Flashing..."));

    auto* thread = QThread::create([this, imagePath, targetDiskId]() {
        auto result = VirtualDisk::flashToDisk(imagePath.toStdWString(), targetDiskId,
            [this](const std::string& s, int p) {
                QMetaObject::invokeMethod(m_flashProgress, "setValue",
                    Qt::QueuedConnection, Q_ARG(int, p));
                QMetaObject::invokeMethod(m_flashStatus, "setText",
                    Qt::QueuedConnection, Q_ARG(QString, QString::fromStdString(s)));
            });
        QMetaObject::invokeMethod(this, [this, result]() {
            m_flashProgress->setVisible(false);
            m_flashBtn->setEnabled(true);
            m_flashStatus->setText(result.isOk()
                ? tr("✓ Flash complete.")
                : tr("✗ Failed: %1").arg(QString::fromStdString(result.error().message)));
            if (result.isOk()) emit statusMessage(tr("Virtual disk flashed to physical disk"));
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// ============================================================================
// Convert
// ============================================================================
void VirtualDiskTab::onConvert()
{
    QString inPath  = m_convInEdit->text().trimmed();
    QString outPath = m_convOutEdit->text().trimmed();
    if (inPath.isEmpty() || outPath.isEmpty())
    {
        QMessageBox::warning(this, tr("Convert"), tr("Specify both input and output files."));
        return;
    }

    static const VirtualDiskFormat fmts[] = {
        VirtualDiskFormat::VHDX, VirtualDiskFormat::VHD,
        VirtualDiskFormat::VMDK, VirtualDiskFormat::QCOW2, VirtualDiskFormat::RAW
    };
    VirtualDiskFormat fmt = fmts[m_convFmtCombo->currentIndex()];

    m_convertBtn->setEnabled(false);
    m_convProgress->setVisible(true);
    m_convStatus->setText(tr("Converting..."));

    auto* thread = QThread::create([this, inPath, outPath, fmt]() {
        auto result = VirtualDisk::convert(inPath.toStdWString(), outPath.toStdWString(), fmt,
            [this](const std::string& s, int) {
                QMetaObject::invokeMethod(m_convStatus, "setText",
                    Qt::QueuedConnection, Q_ARG(QString, QString::fromStdString(s)));
            });
        QMetaObject::invokeMethod(this, [this, result]() {
            m_convProgress->setVisible(false);
            m_convertBtn->setEnabled(true);
            m_convStatus->setText(result.isOk()
                ? tr("✓ Conversion complete.")
                : tr("✗ Failed: %1").arg(QString::fromStdString(result.error().message)));
            if (result.isOk()) emit statusMessage(tr("Virtual disk conversion complete"));
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// ============================================================================
// WSL2 slots (placeholder — full implementation requires NonWindowsFsTab)
// ============================================================================

void VirtualDiskTab::onWslCheckAvailable()
{
    // Check if WSL2 is available and has wsl --mount support (Win10 21H2+)
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start("wsl.exe", {"--status"});
    p.waitForFinished(5000);
    if (p.exitCode() == 0)
        emit statusMessage(tr("WSL2 is available — use 'Linux Filesystems' tab to mount ext4/Btrfs/XFS"));
    else
        emit statusMessage(tr("WSL2 not detected — install WSL2 for Linux filesystem access"));
}

void VirtualDiskTab::onWslMount()
{
    // Full implementation is in NonWindowsFsTab
    QMessageBox::information(this, tr("WSL2 Mount"),
        tr("Use the 'Linux Filesystems' tab to mount ext4, Btrfs, XFS, F2FS, and other "
           "Linux filesystems via WSL2.\n\n"
           "That tab provides full mount/unmount control with drive letter assignment."));
}

void VirtualDiskTab::onWslUnmount()
{
    QProcess p;
    p.start("wsl.exe", {"--unmount"});
    p.waitForFinished(10000);
    emit statusMessage(tr("WSL2 disk unmount complete"));
}

} // namespace spw
