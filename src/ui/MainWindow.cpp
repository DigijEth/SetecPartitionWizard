#include "MainWindow.h"
#include "tabs/DiskPartitionTab.h"
#include "tabs/RecoveryTab.h"
#include "tabs/ImagingTab.h"
#include "tabs/DiagnosticsTab.h"
#include "tabs/SecurityTab.h"
#include "tabs/MaintenanceTab.h"
#include "core/common/Version.h"

#include <QAction>
#include <QApplication>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabWidget>
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
}

MainWindow::~MainWindow() = default;

void MainWindow::setupMenuBar()
{
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&Refresh Disks"), this, &MainWindow::onRefreshDisks, QKeySequence::Refresh);
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

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About..."), this, &MainWindow::onAbout);
}

void MainWindow::setupToolBar()
{
    m_toolBar = addToolBar(tr("Main Toolbar"));
    m_toolBar->setMovable(false);
    m_toolBar->setIconSize(QSize(24, 24));

    m_toolBar->addAction(tr("Refresh"));
    m_toolBar->addSeparator();
    m_toolBar->addAction(tr("Create"));
    m_toolBar->addAction(tr("Delete"));
    m_toolBar->addAction(tr("Resize"));
    m_toolBar->addAction(tr("Format"));
    m_toolBar->addSeparator();
    m_toolBar->addAction(tr("Clone"));
    m_toolBar->addAction(tr("Flash"));
    m_toolBar->addSeparator();

    // Apply button (prominent)
    auto* applyAction = m_toolBar->addAction(tr("Apply"));
    if (auto* widget = m_toolBar->widgetForAction(applyAction))
    {
        widget->setObjectName("applyButton");
    }

    auto* cancelAction = m_toolBar->addAction(tr("Undo All"));
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

    m_tabWidget->addTab(m_diskPartitionTab, tr("Disks && Partitions"));
    m_tabWidget->addTab(m_recoveryTab, tr("Recovery"));
    m_tabWidget->addTab(m_imagingTab, tr("Imaging && Flashing"));
    m_tabWidget->addTab(m_diagnosticsTab, tr("Diagnostics"));
    m_tabWidget->addTab(m_securityTab, tr("Security Keys"));
    m_tabWidget->addTab(m_maintenanceTab, tr("Maintenance"));

    setCentralWidget(m_tabWidget);
}

void MainWindow::setupStatusBar()
{
    statusBar()->showMessage(tr("Ready — No pending operations"));
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
    statusBar()->showMessage(tr("Refreshing disk list..."), 2000);
    // TODO: Call DiskController::refresh()
}

} // namespace spw
