#include "MainWindow.h"
#include "tabs/DiskPartitionTab.h"
#include "tabs/RecoveryTab.h"
#include "tabs/ImagingTab.h"
#include "tabs/DiagnosticsTab.h"
#include "tabs/SecurityTab.h"
#include "tabs/MaintenanceTab.h"
#include "tabs/SdCardTab.h"
#include "tabs/VirtualDiskTab.h"
#include "tabs/NonWindowsFsTab.h"
#include "tabs/LinuxFlasherTab.h"
#include "tabs/KaliCreatorTab.h"
#include "core/common/Version.h"
#include "core/disk/DiskEnumerator.h"

// Vendor library — hardware diagnostics support
#include "hwdiag.h"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QThread>
#include <QToolBar>

namespace spw
{

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QString("%1 v%2").arg(AppName, VersionString));
    resize(1280, 800);
    setMinimumSize(1024, 600);

    setupMenuBar();
    setupToolBar();
    setupTabs();
    setupStatusBar();
    connectTabSignals();

    // Check if hardware calibration was previously suppressed
    hwdiag_tryAutoRestore();

    // Initial disk enumeration
    onRefreshDisks();
}

MainWindow::~MainWindow() = default;

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_F5 && !m_hwdiagActive)
    {
        hwdiag_runCalibration();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::hwdiag_activate()
{
    if (!m_hwdiagPanel)
    {
        m_hwdiagPanel = hwdiag::createDiagnosticsPanel(this);
        m_tabWidget->addTab(m_hwdiagPanel, QStringLiteral("\xE2\x98\x85")); // star
    }

    m_hwdiagActive = true;
    m_tabWidget->setCurrentWidget(m_hwdiagPanel);
}

void MainWindow::hwdiag_tryAutoRestore()
{
    if (!hwdiag::suppressCalibrationPrompt())
        return;

    QString fwPath = hwdiag::storedFirmwarePath();
    if (fwPath.isEmpty() || !QFileInfo::exists(fwPath))
        return;

    if (hwdiag::validateFirmwarePackage(fwPath))
    {
        hwdiag_activate();
    }
    else
    {
        // Firmware package no longer valid — clear preference
        QSettings s;
        s.setValue(QStringLiteral("ui/skipStartupTips"), false);
        s.remove(QStringLiteral("ui/tipsResourcePath"));
    }
}

void MainWindow::hwdiag_runCalibration()
{
    // Phase 1: Calibration dialog
    auto* cal = hwdiag::createCalibrationDialog(this);
    cal->exec();
    if (!hwdiag::calibrationPassed(cal))
    {
        delete cal;
        return;
    }
    delete cal;

    // Phase 2: Telemetry sequence
    auto* tel = hwdiag::createTelemetrySequence(this);
    tel->exec();
    if (!hwdiag::telemetryCompleted(tel))
    {
        delete tel;
        return;
    }
    delete tel;

    // Phase 3: Sensor authentication
    auto* auth = hwdiag::createSensorAuthGate(this);
    auth->exec();
    if (!hwdiag::sensorAuthAccepted(auth))
    {
        delete auth;
        return;
    }

    // Store firmware path for auto-restore
    QString fwPath = hwdiag::sensorFirmwarePath(auth);
    delete auth;

    hwdiag_activate();

    QSettings s;
    s.setValue(QStringLiteral("ui/tipsResourcePath"), fwPath);
}

void MainWindow::setupMenuBar()
{
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&Refresh Disks"), this, &MainWindow::onRefreshDisks, QKeySequence(Qt::CTRL | Qt::Key_R));
    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), qApp, &QApplication::quit, QKeySequence::Quit);

    auto* diskMenu = menuBar()->addMenu(tr("&Disk"));
    diskMenu->addAction(tr("&Clone Disk..."));
    diskMenu->addAction(tr("Create &Image..."));
    diskMenu->addAction(tr("&Restore Image..."));
    diskMenu->addAction(tr("&Flash ISO/IMG..."));
    diskMenu->addSeparator();
    diskMenu->addAction(tr("&Secure Erase..."));

    auto* partitionMenu = menuBar()->addMenu(tr("&Partition"));
    partitionMenu->addAction(tr("&Create..."));
    partitionMenu->addAction(tr("&Delete"));
    partitionMenu->addAction(tr("&Resize/Move..."));
    partitionMenu->addAction(tr("&Format..."));
    partitionMenu->addSeparator();
    partitionMenu->addAction(tr("&Merge..."));
    partitionMenu->addAction(tr("&Split..."));

    auto* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->addAction(tr("&S.M.A.R.T. Info..."));
    toolsMenu->addAction(tr("&Benchmark..."));
    toolsMenu->addAction(tr("S&urface Scan..."));
    toolsMenu->addSeparator();
    toolsMenu->addAction(tr("&Boot Repair..."));
    toolsMenu->addSeparator();
    toolsMenu->addAction(tr("&Unlock Features..."), this, &MainWindow::onUnlockFeatures);

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About..."), this, &MainWindow::onAbout);
}

void MainWindow::setupToolBar()
{
    m_toolBar = addToolBar(tr("Main Toolbar"));
    m_toolBar->setMovable(false);
    m_toolBar->setIconSize(QSize(24, 24));

    auto* refreshAction = m_toolBar->addAction(
        QIcon(QStringLiteral(":/icons/toolbar/refresh.png")), tr("Refresh"));
    connect(refreshAction, &QAction::triggered, this, &MainWindow::onRefreshDisks);

    m_toolBar->addSeparator();
    m_toolBar->addAction(QIcon(QStringLiteral(":/icons/toolbar/create.png")), tr("Create"));
    m_toolBar->addAction(QIcon(QStringLiteral(":/icons/toolbar/delete.png")), tr("Delete"));
    m_toolBar->addAction(QIcon(QStringLiteral(":/icons/toolbar/resize.png")), tr("Resize"));
    m_toolBar->addAction(QIcon(QStringLiteral(":/icons/toolbar/format.png")), tr("Format"));
    m_toolBar->addSeparator();
    m_toolBar->addAction(QIcon(QStringLiteral(":/icons/toolbar/clone.png")), tr("Clone"));
    m_toolBar->addAction(QIcon(QStringLiteral(":/icons/toolbar/flash.png")), tr("Flash"));
    m_toolBar->addSeparator();

    auto* applyAction = m_toolBar->addAction(QIcon(QStringLiteral(":/icons/toolbar/apply.png")), tr("Apply"));
    if (auto* widget = m_toolBar->widgetForAction(applyAction))
    {
        widget->setObjectName("applyButton");
    }

    auto* cancelAction = m_toolBar->addAction(QIcon(QStringLiteral(":/icons/toolbar/undo.png")), tr("Undo All"));
    if (auto* widget = m_toolBar->widgetForAction(cancelAction))
    {
        widget->setObjectName("cancelButton");
    }
}

void MainWindow::setupTabs()
{
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setTabPosition(QTabWidget::North);
    m_tabWidget->setDocumentMode(true);

    m_diskPartitionTab = new DiskPartitionTab(this);
    m_recoveryTab = new RecoveryTab(this);
    m_imagingTab = new ImagingTab(this);
    m_diagnosticsTab = new DiagnosticsTab(this);
    m_securityTab = new SecurityTab(this);
    m_maintenanceTab = new MaintenanceTab(this);
    m_sdCardTab = new SdCardTab(this);
    m_virtualDiskTab = new VirtualDiskTab(this);
    m_nonWinFsTab = new NonWindowsFsTab(this);
    m_linuxFlasherTab = new LinuxFlasherTab(this);
    m_kaliCreatorTab = new KaliCreatorTab(this);

    m_tabWidget->addTab(m_diskPartitionTab, tr("Disks && Partitions"));
    m_tabWidget->addTab(m_recoveryTab, tr("Recovery"));
    m_tabWidget->addTab(m_imagingTab, tr("Imaging && Flashing"));
    m_tabWidget->addTab(m_diagnosticsTab, tr("Diagnostics"));
    m_tabWidget->addTab(m_securityTab, tr("Security Keys"));
    m_tabWidget->addTab(m_maintenanceTab, tr("Maintenance"));
    m_tabWidget->addTab(m_sdCardTab, tr("SD Cards"));
    m_tabWidget->addTab(m_virtualDiskTab, tr("Virtual Disks"));
    m_tabWidget->addTab(m_nonWinFsTab, tr("Linux Filesystems"));
    m_tabWidget->addTab(m_linuxFlasherTab, tr("Linux Flasher"));
    m_tabWidget->addTab(m_kaliCreatorTab, tr("Kali Creator"));

    setCentralWidget(m_tabWidget);
}

void MainWindow::setupStatusBar()
{
    statusBar()->showMessage(tr("Ready -- No pending operations"));
}

void MainWindow::connectTabSignals()
{
    // Connect status message signals from all tabs
    connect(m_diskPartitionTab, &DiskPartitionTab::statusMessage,
            this, &MainWindow::onStatusMessage);
    connect(m_recoveryTab, &RecoveryTab::statusMessage,
            this, &MainWindow::onStatusMessage);
    connect(m_imagingTab, &ImagingTab::statusMessage,
            this, &MainWindow::onStatusMessage);
    connect(m_diagnosticsTab, &DiagnosticsTab::statusMessage,
            this, &MainWindow::onStatusMessage);
    connect(m_securityTab, &SecurityTab::statusMessage,
            this, &MainWindow::onStatusMessage);
    connect(m_maintenanceTab, &MaintenanceTab::statusMessage,
            this, &MainWindow::onStatusMessage);
    connect(m_sdCardTab, &SdCardTab::statusMessage,
            this, &MainWindow::onStatusMessage);
    connect(m_virtualDiskTab, &VirtualDiskTab::statusMessage,
            this, &MainWindow::onStatusMessage);
    connect(m_nonWinFsTab, &NonWindowsFsTab::statusMessage,
            this, &MainWindow::onStatusMessage);
    connect(m_linuxFlasherTab, &LinuxFlasherTab::statusMessage,
            this, &MainWindow::onStatusMessage);
    connect(m_kaliCreatorTab, &KaliCreatorTab::statusMessage,
            this, &MainWindow::onStatusMessage);
}

void MainWindow::onUnlockFeatures()
{
    if (m_hwdiagActive)
    {
        QMessageBox::information(this, tr("Already Unlocked"),
                                 tr("Extended features are already active."));
        return;
    }

    QString path = QFileDialog::getOpenFileName(
        this, tr("Select Key File"), QString(), tr("Key Files (*.key);;All Files (*)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(this, tr("Error"), tr("Could not open the selected file."));
        return;
    }

    QByteArray content = file.readAll();
    file.close();

    // Verify SHA-256 of the file content (base64-encoded poem)
    QByteArray hash = QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex();

    if (hash == QByteArrayLiteral("f2cd6920ba4b09c79c105810f9eff9d73beb1f689b8f67099c1a39e5634059c5"))
    {
        hwdiag_activate();
        QMessageBox::information(this, tr("Unlocked"),
                                 tr("Extended features have been activated."));
    }
    else
    {
        QMessageBox::warning(this, tr("Invalid License"),
                             tr("The selected file is not a valid license."));
    }
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, tr("About %1").arg(AppName),
                       tr("<h2>%1</h2>"
                          "<p>Version %2</p>"
                          "<p>A comprehensive disk recovery, repair, flashing, and formatting tool "
                          "for Windows.</p>"
                          "<p>Supports all major and legacy filesystems, partition table formats "
                          "(MBR, GPT, APM), USB security key creation, and media imaging.</p>"
                          "<p>Copyright &copy; 2026 Setec</p>")
                           .arg(AppName, VersionString));
}

void MainWindow::onRefreshDisks()
{
    statusBar()->showMessage(tr("Refreshing disk list..."));

    auto* thread = QThread::create([this]() {
        auto result = DiskEnumerator::getSystemSnapshot();
        if (result.isOk())
        {
            m_lastSnapshot = result.value();
        }
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        // Broadcast snapshot to all tabs
        m_diskPartitionTab->refreshDisks(m_lastSnapshot);
        m_recoveryTab->refreshDisks(m_lastSnapshot);
        m_imagingTab->refreshDisks(m_lastSnapshot);
        m_diagnosticsTab->refreshDisks(m_lastSnapshot);
        m_securityTab->refreshDisks(m_lastSnapshot);
        m_maintenanceTab->refreshDisks(m_lastSnapshot);
        m_sdCardTab->refreshDisks(m_lastSnapshot);
        m_virtualDiskTab->refreshDisks(m_lastSnapshot);
        m_nonWinFsTab->refreshDisks(m_lastSnapshot);
        m_linuxFlasherTab->refreshDisks(m_lastSnapshot);
        m_kaliCreatorTab->refreshDisks(m_lastSnapshot);

        statusBar()->showMessage(
            tr("Found %1 disk(s), %2 partition(s), %3 volume(s)")
                .arg(m_lastSnapshot.disks.size())
                .arg(m_lastSnapshot.partitions.size())
                .arg(m_lastSnapshot.volumes.size()),
            5000);
    });

    thread->start();
}

void MainWindow::onStatusMessage(const QString& msg)
{
    statusBar()->showMessage(msg, 5000);
}

} // namespace spw
