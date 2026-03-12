#pragma once

// NonWindowsFsTab — Mount and access filesystems that Windows cannot natively read:
// ext2/3/4, Btrfs, XFS, ZFS, F2FS, JFFS2, HFS+, UFS, ReiserFS, etc.
//
// Three mounting strategies, used in order of preference:
//   1. WSL2 wsl --mount  (Windows 10 21H2+, no extra drivers needed)
//   2. Third-party kernel drivers (Ext2Fsd, WinBtrfs, ZFSin, WinHFSPlus)
//   3. Read-only access via libext2fs/raw parsing (planned)

#include "core/common/Types.h"
#include "core/disk/DiskEnumerator.h"

#include <QWidget>

class QComboBox;
class QGroupBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTextEdit;

namespace spw
{

class NonWindowsFsTab : public QWidget
{
    Q_OBJECT

public:
    explicit NonWindowsFsTab(QWidget* parent = nullptr);
    ~NonWindowsFsTab() override;

public slots:
    void refreshDisks(const SystemDiskSnapshot& snapshot);

signals:
    void statusMessage(const QString& msg);

private slots:
    void onWslMount();
    void onWslUnmount();
    void onWslUnmountAll();
    void onWslRefreshMounts();
    void onDriverMount();
    void onDriverUnmount();
    void onRefreshDriverStatus();
    void onOpenMountPoint();

private:
    void setupUi();
    void setupWslTab();
    void setupDriverTab();
    void setupInfoTab();
    void populateDiskCombo();
    void checkWslAvailability();
    void checkDriverAvailability();
    static QString formatSize(uint64_t bytes);

    // WSL2 mount section
    QComboBox*    m_wslDiskCombo     = nullptr;
    QSpinBox*     m_wslPartSpin      = nullptr;
    QComboBox*    m_wslFsTypeCombo   = nullptr;
    QPushButton*  m_wslMountBtn      = nullptr;
    QPushButton*  m_wslUnmountBtn    = nullptr;
    QPushButton*  m_wslUnmountAllBtn = nullptr;
    QPushButton*  m_wslRefreshBtn    = nullptr;
    QTableWidget* m_wslMountsTable   = nullptr;
    QPushButton*  m_wslOpenBtn       = nullptr;
    QLabel*       m_wslAvailLabel    = nullptr;
    QLabel*       m_wslStatusLabel   = nullptr;

    // Driver-based mount section
    QComboBox*    m_drvDiskCombo     = nullptr;
    QSpinBox*     m_drvPartSpin      = nullptr;
    QComboBox*    m_drvDriverCombo   = nullptr;
    QPushButton*  m_drvMountBtn      = nullptr;
    QPushButton*  m_drvUnmountBtn    = nullptr;
    QTableWidget* m_drvMountsTable   = nullptr;
    QLabel*       m_drvStatusLabel   = nullptr;
    QTextEdit*    m_drvDriverStatus  = nullptr;

    // Info tab
    QTextEdit*    m_infoText         = nullptr;

    SystemDiskSnapshot m_snapshot;
    bool m_wslAvailable = false;
};

} // namespace spw
