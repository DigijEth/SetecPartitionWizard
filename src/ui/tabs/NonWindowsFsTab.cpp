#include "NonWindowsFsTab.h"

#include "core/disk/DiskEnumerator.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QThread>
#include <QVBoxLayout>

namespace spw
{

NonWindowsFsTab::NonWindowsFsTab(QWidget* parent) : QWidget(parent)
{
    setupUi();
    checkWslAvailability();
    checkDriverAvailability();
}

NonWindowsFsTab::~NonWindowsFsTab() = default;

QString NonWindowsFsTab::formatSize(uint64_t bytes)
{
    if (bytes >= 1099511627776ULL)
        return QString("%1 TB").arg(bytes / 1099511627776.0, 0, 'f', 2);
    if (bytes >= 1073741824ULL)
        return QString("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 1);
    if (bytes >= 1048576ULL)
        return QString("%1 MB").arg(bytes / 1048576.0, 0, 'f', 0);
    return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}

void NonWindowsFsTab::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);

    auto* innerTabs = new QTabWidget();
    setupWslTab();
    setupDriverTab();
    setupInfoTab();

    // ---- WSL2 Tab ----
    auto* wslWidget = new QWidget();
    auto* wslLayout = new QVBoxLayout(wslWidget);

    m_wslAvailLabel = new QLabel();
    m_wslAvailLabel->setWordWrap(true);
    m_wslAvailLabel->setStyleSheet("font-weight: bold; padding: 4px;");
    wslLayout->addWidget(m_wslAvailLabel);

    auto* wslInfo = new QLabel(
        tr("WSL2's wsl --mount command (Windows 10 21H2 / Build 21364+) can attach a physical disk "
           "to WSL2, making ext4, Btrfs, XFS, F2FS, ZFS, JFFS2, and other Linux filesystems "
           "readable and writable directly from Windows via the \\\\wsl$ share."));
    wslInfo->setWordWrap(true);
    wslInfo->setStyleSheet("color: #aaaaaa; font-style: italic;");
    wslLayout->addWidget(wslInfo);

    auto* wslMountGroup = new QGroupBox(tr("Mount Disk via WSL2"));
    auto* wslMountLayout = new QVBoxLayout(wslMountGroup);

    auto* diskRow = new QHBoxLayout();
    diskRow->addWidget(new QLabel(tr("Disk:")));
    m_wslDiskCombo = new QComboBox();
    diskRow->addWidget(m_wslDiskCombo, 1);
    diskRow->addWidget(new QLabel(tr("Partition:")));
    m_wslPartSpin = new QSpinBox();
    m_wslPartSpin->setRange(0, 128);
    m_wslPartSpin->setValue(1);
    m_wslPartSpin->setSpecialValueText(tr("Whole disk (0)"));
    diskRow->addWidget(m_wslPartSpin);
    wslMountLayout->addLayout(diskRow);

    auto* fsRow = new QHBoxLayout();
    fsRow->addWidget(new QLabel(tr("Filesystem type:")));
    m_wslFsTypeCombo = new QComboBox();
    m_wslFsTypeCombo->addItems({
        tr("auto  (let WSL2 detect)"),
        tr("ext4"), tr("ext3"), tr("ext2"),
        tr("btrfs"), tr("xfs"), tr("f2fs"),
        tr("jffs2"), tr("nilfs2"),
        tr("hfsplus"), tr("ufs"),
        tr("vfat"), tr("exfat"), tr("ntfs"),
    });
    fsRow->addWidget(m_wslFsTypeCombo, 1);
    wslMountLayout->addLayout(fsRow);

    auto* mountBtnRow = new QHBoxLayout();
    m_wslMountBtn = new QPushButton(tr("Mount via WSL2"));
    m_wslMountBtn->setStyleSheet(
        "QPushButton { background-color: #d4a574; color: #1e1e2e; font-weight: bold; "
        "border-radius: 4px; } QPushButton:hover { background-color: #e0b584; }");
    connect(m_wslMountBtn, &QPushButton::clicked, this, &NonWindowsFsTab::onWslMount);
    mountBtnRow->addWidget(m_wslMountBtn);

    m_wslUnmountBtn = new QPushButton(tr("Unmount All WSL Disks"));
    connect(m_wslUnmountBtn, &QPushButton::clicked, this, &NonWindowsFsTab::onWslUnmountAll);
    mountBtnRow->addWidget(m_wslUnmountBtn);

    m_wslRefreshBtn = new QPushButton(tr("Refresh Mounts"));
    connect(m_wslRefreshBtn, &QPushButton::clicked, this, &NonWindowsFsTab::onWslRefreshMounts);
    mountBtnRow->addWidget(m_wslRefreshBtn);
    wslMountLayout->addLayout(mountBtnRow);

    wslLayout->addWidget(wslMountGroup);

    // Mounted disks table
    auto* wslMountedGroup = new QGroupBox(tr("Currently Mounted WSL2 Disks"));
    auto* wslMountedLayout = new QVBoxLayout(wslMountedGroup);

    m_wslMountsTable = new QTableWidget(0, 3);
    m_wslMountsTable->setHorizontalHeaderLabels({tr("Device"), tr("Mount Point"), tr("Filesystem")});
    m_wslMountsTable->horizontalHeader()->setStretchLastSection(true);
    m_wslMountsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_wslMountsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_wslMountsTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    wslMountedLayout->addWidget(m_wslMountsTable);

    auto* wslTableBtnRow = new QHBoxLayout();
    m_wslUnmountBtn = new QPushButton(tr("Unmount Selected"));
    m_wslUnmountBtn->setStyleSheet(
        "QPushButton { background-color: #cc3333; color: white; border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background-color: #ee4444; }");
    connect(m_wslUnmountBtn, &QPushButton::clicked, this, &NonWindowsFsTab::onWslUnmount);
    wslTableBtnRow->addWidget(m_wslUnmountBtn);

    m_wslOpenBtn = new QPushButton(tr("Open in Explorer"));
    connect(m_wslOpenBtn, &QPushButton::clicked, this, &NonWindowsFsTab::onOpenMountPoint);
    wslTableBtnRow->addWidget(m_wslOpenBtn);
    wslTableBtnRow->addStretch();
    wslMountedLayout->addLayout(wslTableBtnRow);

    wslLayout->addWidget(wslMountedGroup);

    m_wslStatusLabel = new QLabel();
    m_wslStatusLabel->setWordWrap(true);
    wslLayout->addWidget(m_wslStatusLabel);

    innerTabs->addTab(wslWidget, tr("WSL2 Mount"));

    // ---- Driver Tab ----
    auto* drvWidget = new QWidget();
    auto* drvLayout = new QVBoxLayout(drvWidget);

    auto* drvInfo = new QLabel(
        tr("Third-party kernel drivers give Windows native access to Linux/Mac filesystems "
           "with a real drive letter — no WSL2 required.\n\n"
           "Open-source drivers detected and installed automatically if present:"));
    drvInfo->setWordWrap(true);
    drvInfo->setStyleSheet("color: #aaaaaa; font-style: italic;");
    drvLayout->addWidget(drvInfo);

    m_drvDriverStatus = new QTextEdit();
    m_drvDriverStatus->setReadOnly(true);
    m_drvDriverStatus->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_drvDriverStatus->setFont(QFont("Courier New", 9));
    drvLayout->addWidget(m_drvDriverStatus);

    auto* drvMountGroup = new QGroupBox(tr("Mount with Driver"));
    auto* drvMountLayout = new QVBoxLayout(drvMountGroup);

    auto* drvDiskRow = new QHBoxLayout();
    drvDiskRow->addWidget(new QLabel(tr("Disk:")));
    m_drvDiskCombo = new QComboBox();
    drvDiskRow->addWidget(m_drvDiskCombo, 1);
    drvDiskRow->addWidget(new QLabel(tr("Part:")));
    m_drvPartSpin = new QSpinBox();
    m_drvPartSpin->setRange(1, 128);
    m_drvPartSpin->setValue(1);
    drvDiskRow->addWidget(m_drvPartSpin);
    drvMountLayout->addLayout(drvDiskRow);

    auto* drvSelectRow = new QHBoxLayout();
    drvSelectRow->addWidget(new QLabel(tr("Driver:")));
    m_drvDriverCombo = new QComboBox();
    m_drvDriverCombo->addItems({
        tr("Ext2Fsd  (ext2/3/4 — open source, requires install)"),
        tr("WinBtrfs  (Btrfs — open source, requires install)"),
        tr("WinHFSPlus  (HFS+ — open source, read-only)"),
        tr("ZFSin  (ZFS — OpenZFS port for Windows)"),
    });
    drvSelectRow->addWidget(m_drvDriverCombo, 1);
    drvMountLayout->addLayout(drvSelectRow);

    auto* drvBtnRow = new QHBoxLayout();
    m_drvMountBtn = new QPushButton(tr("Mount with Driver"));
    m_drvMountBtn->setStyleSheet(
        "QPushButton { background-color: #d4a574; color: #1e1e2e; font-weight: bold; "
        "border-radius: 4px; } QPushButton:hover { background-color: #e0b584; }");
    connect(m_drvMountBtn, &QPushButton::clicked, this, &NonWindowsFsTab::onDriverMount);
    drvBtnRow->addWidget(m_drvMountBtn);
    m_drvUnmountBtn = new QPushButton(tr("Unmount"));
    connect(m_drvUnmountBtn, &QPushButton::clicked, this, &NonWindowsFsTab::onDriverUnmount);
    drvBtnRow->addWidget(m_drvUnmountBtn);
    auto* drvRefreshBtn = new QPushButton(tr("Refresh Driver Status"));
    connect(drvRefreshBtn, &QPushButton::clicked, this, &NonWindowsFsTab::onRefreshDriverStatus);
    drvBtnRow->addWidget(drvRefreshBtn);
    drvMountLayout->addLayout(drvBtnRow);

    drvLayout->addWidget(drvMountGroup);

    m_drvStatusLabel = new QLabel();
    m_drvStatusLabel->setWordWrap(true);
    drvLayout->addWidget(m_drvStatusLabel);

    innerTabs->addTab(drvWidget, tr("Kernel Drivers"));

    // ---- Info Tab ----
    auto* infoWidget = new QWidget();
    auto* infoLayout = new QVBoxLayout(infoWidget);
    m_infoText = new QTextEdit();
    m_infoText->setReadOnly(true);
    m_infoText->setHtml(tr(
        "<h3>Linux &amp; Mac Filesystem Access on Windows</h3>"
        "<p>Windows cannot natively read ext4, Btrfs, XFS, HFS+, F2FS, ZFS etc. "
        "There are two ways to access them:</p>"

        "<h4>Option 1: WSL2 Mount (recommended, no install needed)</h4>"
        "<p>Windows 10 21H2+ and Windows 11 include <code>wsl --mount</code> which attaches "
        "a physical disk to WSL2. The files are then accessible at "
        "<code>\\\\wsl$\\Ubuntu\\mnt\\wsl\\PhysicalDrive1p1\\</code></p>"
        "<pre>wsl --mount \\\\.\\PhysicalDrive1 --partition 1 --type ext4</pre>"
        "<p>Supports: ext2, ext3, ext4, btrfs, xfs, f2fs, jffs2, nilfs2</p>"

        "<h4>Option 2: Third-party kernel drivers</h4>"
        "<ul>"
        "<li><b>Ext2Fsd</b> — ext2/3/4 read/write driver. Free &amp; open source. "
        "<a href='https://www.ext2fsd.com/'>ext2fsd.com</a></li>"
        "<li><b>WinBtrfs</b> — Full Btrfs read/write driver. Open source. "
        "<a href='https://github.com/maharmstone/btrfs'>github.com/maharmstone/btrfs</a></li>"
        "<li><b>WinHFSPlus</b> — HFS+ read-only. Open source. "
        "<a href='https://github.com/JetBrains/WinHFSPlus'>github.com/JetBrains/WinHFSPlus</a></li>"
        "<li><b>ZFSin</b> — OpenZFS port for Windows. "
        "<a href='https://github.com/openzfsonwindows/ZFSin'>github.com/openzfsonwindows/ZFSin</a></li>"
        "</ul>"

        "<h4>Future: Native Drivers</h4>"
        "<p>Setec Partition Wizard includes a roadmap for built-in kernel-mode filesystem "
        "drivers (IFS drivers) that will provide native Windows access to Linux/Mac filesystems "
        "without requiring any third-party software. This requires the Windows Driver Kit (WDK) "
        "and kernel signing — watch for updates.</p>"
    ));
    infoLayout->addWidget(m_infoText);
    innerTabs->addTab(infoWidget, tr("How It Works"));

    mainLayout->addWidget(innerTabs);
}

void NonWindowsFsTab::setupWslTab() {}
void NonWindowsFsTab::setupDriverTab() {}
void NonWindowsFsTab::setupInfoTab() {}

void NonWindowsFsTab::checkWslAvailability()
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start("wsl.exe", {"--status"});
    p.waitForFinished(5000);
    m_wslAvailable = (p.exitCode() == 0);

    if (m_wslAvailLabel)
    {
        if (m_wslAvailable)
        {
            m_wslAvailLabel->setText(tr("✓ WSL2 is available — Linux filesystem mounting enabled"));
            m_wslAvailLabel->setStyleSheet("color: #a8e6a0; font-weight: bold; padding: 4px;");
        }
        else
        {
            m_wslAvailLabel->setText(
                tr("✗ WSL2 not detected. Install WSL2: run 'wsl --install' in an admin PowerShell, "
                   "then restart. Windows 10 21H2+ required."));
            m_wslAvailLabel->setStyleSheet("color: #ff9944; font-weight: bold; padding: 4px;");
        }
    }
}

void NonWindowsFsTab::checkDriverAvailability()
{
    if (!m_drvDriverStatus) return;

    QString status;

    // Check for Ext2Fsd service
    {
        QProcess p;
        p.start("sc.exe", {"query", "Ext2Fsd"});
        p.waitForFinished(3000);
        bool found = (p.exitCode() == 0);
        status += found ? "✓ Ext2Fsd (ext2/3/4):   INSTALLED\n" : "✗ Ext2Fsd (ext2/3/4):   not installed\n";
    }

    // Check for WinBtrfs
    {
        QProcess p;
        p.start("sc.exe", {"query", "btrfs"});
        p.waitForFinished(3000);
        bool found = (p.exitCode() == 0);
        status += found ? "✓ WinBtrfs (Btrfs):     INSTALLED\n" : "✗ WinBtrfs (Btrfs):     not installed\n";
    }

    // Check for WinHFSPlus
    {
        QProcess p;
        p.start("sc.exe", {"query", "WinHFSPlus"});
        p.waitForFinished(3000);
        bool found = (p.exitCode() == 0);
        status += found ? "✓ WinHFSPlus (HFS+):    INSTALLED\n" : "✗ WinHFSPlus (HFS+):    not installed\n";
    }

    // Check for ZFSin
    {
        QProcess p;
        p.start("sc.exe", {"query", "zfs"});
        p.waitForFinished(3000);
        bool found = (p.exitCode() == 0);
        status += found ? "✓ ZFSin (ZFS):          INSTALLED\n" : "✗ ZFSin (ZFS):          not installed\n";
    }

    m_drvDriverStatus->setPlainText(status);
}

void NonWindowsFsTab::refreshDisks(const SystemDiskSnapshot& snapshot)
{
    m_snapshot = snapshot;
    populateDiskCombo();
}

void NonWindowsFsTab::populateDiskCombo()
{
    if (m_wslDiskCombo) m_wslDiskCombo->clear();
    if (m_drvDiskCombo) m_drvDiskCombo->clear();

    for (const auto& disk : m_snapshot.disks)
    {
        QString label = QString("Disk %1: %2 (%3)")
            .arg(disk.id)
            .arg(QString::fromStdWString(disk.model))
            .arg(formatSize(disk.sizeBytes));
        if (m_wslDiskCombo) m_wslDiskCombo->addItem(label, disk.id);
        if (m_drvDiskCombo) m_drvDiskCombo->addItem(label, disk.id);
    }
}

// ============================================================================
// WSL2 Slots
// ============================================================================

void NonWindowsFsTab::onWslMount()
{
    if (!m_wslAvailable)
    {
        QMessageBox::warning(this, tr("WSL2 Not Available"),
            tr("WSL2 is not installed or not running.\n\n"
               "Install WSL2: open an admin PowerShell and run:\n"
               "  wsl --install\n\nThen restart Windows."));
        return;
    }

    int diskId = m_wslDiskCombo->currentData().toInt();
    int partition = m_wslPartSpin->value();
    QString fsType = m_wslFsTypeCombo->currentText().split(' ').first();
    if (fsType == "auto") fsType.clear();

    QString devPath = QString("\\\\.\\PhysicalDrive%1").arg(diskId);

    QStringList args = {"--mount", devPath};
    if (partition > 0)
        args << "--partition" << QString::number(partition);
    if (!fsType.isEmpty())
        args << "--type" << fsType;

    m_wslStatusLabel->setText(tr("Mounting disk %1 via WSL2...").arg(diskId));

    auto* thread = QThread::create([this, args]() {
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start("wsl.exe", args);
        p.waitForFinished(30000);
        QString out = QString::fromLocal8Bit(p.readAll());
        int code = p.exitCode();

        QMetaObject::invokeMethod(this, [this, out, code]() {
            if (code == 0)
            {
                m_wslStatusLabel->setText(tr("✓ Mounted. Access via \\\\wsl$\\<distro>\\mnt\\wsl\\"));
                m_wslStatusLabel->setStyleSheet("color: #a8e6a0;");
                onWslRefreshMounts();
                emit statusMessage(tr("WSL2 disk mount successful"));
            }
            else
            {
                m_wslStatusLabel->setText(tr("✗ Mount failed (exit %1):\n%2").arg(code).arg(out));
                m_wslStatusLabel->setStyleSheet("color: #ff6b6b;");
            }
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void NonWindowsFsTab::onWslUnmount()
{
    int row = m_wslMountsTable->currentRow();
    if (row < 0) return;

    QString device = m_wslMountsTable->item(row, 0)->text();

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start("wsl.exe", {"--unmount", device});
    p.waitForFinished(15000);

    if (p.exitCode() == 0)
    {
        m_wslStatusLabel->setText(tr("✓ Unmounted: %1").arg(device));
        onWslRefreshMounts();
        emit statusMessage(tr("WSL2 disk unmounted"));
    }
    else
    {
        m_wslStatusLabel->setText(tr("✗ Unmount failed: %1")
            .arg(QString::fromLocal8Bit(p.readAll())));
    }
}

void NonWindowsFsTab::onWslUnmountAll()
{
    QProcess p;
    p.start("wsl.exe", {"--unmount"});
    p.waitForFinished(15000);
    m_wslStatusLabel->setText(p.exitCode() == 0
        ? tr("✓ All WSL2 disks unmounted.")
        : tr("Unmount all: %1").arg(QString::fromLocal8Bit(p.readAll())));
    onWslRefreshMounts();
    emit statusMessage(tr("All WSL2 disks unmounted"));
}

void NonWindowsFsTab::onWslRefreshMounts()
{
    // Parse `wsl --list --verbose` or check mounted disks via wsl
    m_wslMountsTable->setRowCount(0);

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    // wsl --mount --bare lists currently attached physical disks
    p.start("wsl.exe", {"--list", "--verbose"});
    p.waitForFinished(5000);
    // For now, just show a note — full parsing of wsl mount state requires
    // reading /proc/mounts from inside WSL which we do via wsl -e cat /proc/mounts
    QProcess p2;
    p2.start("wsl.exe", {"-e", "cat", "/proc/mounts"});
    p2.waitForFinished(5000);
    QString mounts = QString::fromUtf8(p2.readAll());

    int row = 0;
    for (const auto& line : mounts.split('\n'))
    {
        // Only show entries that look like physical disks mounted via wsl --mount
        if (!line.contains("/mnt/wsl/") && !line.startsWith("/dev/sd"))
            continue;
        auto parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 3) continue;

        m_wslMountsTable->insertRow(row);
        m_wslMountsTable->setItem(row, 0, new QTableWidgetItem(parts[0])); // device
        m_wslMountsTable->setItem(row, 1, new QTableWidgetItem(parts[1])); // mountpoint
        m_wslMountsTable->setItem(row, 2, new QTableWidgetItem(parts[2])); // fstype
        ++row;
    }
}

void NonWindowsFsTab::onOpenMountPoint()
{
    int row = m_wslMountsTable->currentRow();
    if (row < 0) return;

    // Open \\wsl$\ in Explorer
    QString mountPt = m_wslMountsTable->item(row, 1)->text();
    // Convert /mnt/wsl/... to \\wsl$\<distro>\mnt\wsl\...
    QProcess::startDetached("explorer.exe", {"\\\\wsl$"});
    emit statusMessage(tr("Opened \\\\wsl$ in Explorer — navigate to your mount point"));
}

// ============================================================================
// Driver-based mount slots
// ============================================================================

void NonWindowsFsTab::onDriverMount()
{
    int diskId = m_drvDiskCombo->currentData().toInt();
    int part   = m_drvPartSpin->value();
    int driver = m_drvDriverCombo->currentIndex();

    // Build a DevPath like \\.\PhysicalDrive1
    QString devPath = QString("\\\\.\\PhysicalDrive%1").arg(diskId);

    QString msg;
    switch (driver)
    {
    case 0: // Ext2Fsd
        msg = tr("Ext2Fsd assigns a drive letter automatically after mounting via its service.\n\n"
                 "If Ext2Fsd is installed, use 'Ext2 Volume Manager' from the Start menu for GUI control, "
                 "or the ext2mgr command line tool.\n\n"
                 "Device: %1  Partition: %2").arg(devPath).arg(part);
        break;
    case 1: // WinBtrfs
        msg = tr("WinBtrfs mounts automatically when a Btrfs partition is detected.\n\n"
                 "Ensure the WinBtrfs driver is installed and the service is running.\n"
                 "Device: %1  Partition: %2").arg(devPath).arg(part);
        break;
    case 2: // WinHFSPlus
        msg = tr("WinHFSPlus provides read-only HFS+ access.\n\n"
                 "Install the driver package, then the HFS+ partition should appear "
                 "automatically as a drive letter.\n"
                 "Device: %1  Partition: %2").arg(devPath).arg(part);
        break;
    case 3: // ZFSin
        msg = tr("ZFSin (OpenZFS on Windows) mounts ZFS pools automatically.\n\n"
                 "Import the pool: zpool import -d %1\n"
                 "Then mount: zfs mount -a").arg(devPath);
        break;
    default:
        msg = tr("Select a driver.");
    }

    m_drvStatusLabel->setText(msg);
    emit statusMessage(tr("Driver mount instructions shown"));
}

void NonWindowsFsTab::onDriverUnmount()
{
    m_drvStatusLabel->setText(
        tr("Use the driver's own tools to unmount:\n"
           "• Ext2Fsd: Ext2 Volume Manager → right-click → Disconnect\n"
           "• WinBtrfs: Disk Management → Remove Drive Letter\n"
           "• WinHFSPlus: Disk Management → Remove Drive Letter\n"
           "• ZFSin: zfs unmount <dataset>  then  zpool export <pool>"));
}

void NonWindowsFsTab::onRefreshDriverStatus()
{
    checkDriverAvailability();
    emit statusMessage(tr("Driver status refreshed"));
}

} // namespace spw
