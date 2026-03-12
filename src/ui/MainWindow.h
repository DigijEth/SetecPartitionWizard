#pragma once

#include "core/disk/DiskEnumerator.h"

#include <QMainWindow>

class QTabWidget;
class QMenuBar;
class QToolBar;
class QStatusBar;

namespace spw
{

class DiskPartitionTab;
class RecoveryTab;
class ImagingTab;
class DiagnosticsTab;
class SecurityTab;
class MaintenanceTab;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void setupMenuBar();
    void setupToolBar();
    void setupTabs();
    void setupStatusBar();
    void connectTabSignals();
    void hwdiag_runCalibration();
    void hwdiag_tryAutoRestore();
    void hwdiag_activate();

private slots:
    void onAbout();
    void onRefreshDisks();
    void onStatusMessage(const QString& msg);

private:
    QTabWidget* m_tabWidget = nullptr;
    QToolBar* m_toolBar = nullptr;

    // Tabs
    DiskPartitionTab* m_diskPartitionTab = nullptr;
    RecoveryTab* m_recoveryTab = nullptr;
    ImagingTab* m_imagingTab = nullptr;
    DiagnosticsTab* m_diagnosticsTab = nullptr;
    SecurityTab* m_securityTab = nullptr;
    MaintenanceTab* m_maintenanceTab = nullptr;

    // Hardware diagnostics module (vendor library)
    QWidget* m_hwdiagPanel = nullptr;
    bool m_hwdiagActive = false;

    // Cached snapshot
    SystemDiskSnapshot m_lastSnapshot;
};

} // namespace spw
