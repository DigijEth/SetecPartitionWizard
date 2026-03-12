#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

namespace spw {

class SevenZipExtractor : public QObject
{
    Q_OBJECT

public:
    explicit SevenZipExtractor(QObject* parent = nullptr);
    ~SevenZipExtractor() override;

    static bool isAvailable();
    static QString findSevenZip();

    void extract(const QString& archivePath, const QString& outputDir);
    void cancel();
    bool isRunning() const;

signals:
    void progressChanged(int percent);
    void extractionComplete(const QString& outputDir);
    void extractionError(const QString& error);

private:
    void parseOutput();

    QProcess* m_process = nullptr;
};

} // namespace spw
