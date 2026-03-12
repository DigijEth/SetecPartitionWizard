#pragma once

#include "core/common/Types.h"
#include "core/disk/DiskEnumerator.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

namespace spw
{

class ImageCatalog;
class DownloadManager;

class LinuxFlasherTab : public QWidget
{
    Q_OBJECT

public:
    explicit LinuxFlasherTab(QWidget* parent = nullptr);
    ~LinuxFlasherTab() override;

public slots:
    void refreshDisks(const SystemDiskSnapshot& snapshot);

signals:
    void statusMessage(const QString& msg);

private slots:
    void onCategoryChanged(int index);
    void onOsChanged(int index);
    void onBrowseCustomImage();
    void onDownloadAndFlash();
    void onDownloadOnly();
    void onCancel();
    void onCatalogUpdated();

private:
    void setupUi();
    void populateTargetDriveCombo();
    void startPipeline(bool flashAfter);
    void decompressAndMaybeFlash(const QString& downloadedPath, bool flashAfter);
    void flashImage(const QString& imagePath);
    void setOperationRunning(bool running);

    static QString formatSize(uint64_t bytes);

    // OS Selection
    QComboBox* m_categoryCombo = nullptr;
    QComboBox* m_osCombo = nullptr;
    QLabel* m_descriptionLabel = nullptr;

    // Custom image
    QLineEdit* m_customImageEdit = nullptr;

    // Target drive
    QComboBox* m_targetDriveCombo = nullptr;

    // Progress
    QProgressBar* m_progressBar = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_speedLabel = nullptr;

    // Buttons
    QPushButton* m_downloadFlashBtn = nullptr;
    QPushButton* m_downloadOnlyBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;

    // Core objects
    ImageCatalog* m_catalog = nullptr;
    DownloadManager* m_downloader = nullptr;

    // Data
    SystemDiskSnapshot m_snapshot;
    bool m_cancelled = false;
};

} // namespace spw
