#include "SevenZipExtractor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

namespace spw {

SevenZipExtractor::SevenZipExtractor(QObject* parent)
    : QObject(parent)
{
}

SevenZipExtractor::~SevenZipExtractor()
{
    cancel();
}

bool SevenZipExtractor::isAvailable()
{
    return !findSevenZip().isEmpty();
}

QString SevenZipExtractor::findSevenZip()
{
    // 1. Check PATH via QStandardPaths
    QString path = QStandardPaths::findExecutable(QStringLiteral("7z"));
    if (!path.isEmpty())
        return path;

    // 2. Check common installation directories
    const QStringList commonPaths = {
        QStringLiteral("C:/Program Files/7-Zip/7z.exe"),
        QStringLiteral("C:/Program Files (x86)/7-Zip/7z.exe"),
    };

    for (const QString& candidate : commonPaths) {
        if (QFileInfo::exists(candidate))
            return candidate;
    }

    // 3. Check application directory
    const QString appDirCandidate = QCoreApplication::applicationDirPath()
                                    + QStringLiteral("/7z.exe");
    if (QFileInfo::exists(appDirCandidate))
        return appDirCandidate;

    return {};
}

void SevenZipExtractor::extract(const QString& archivePath, const QString& outputDir)
{
    if (m_process) {
        emit extractionError(tr("An extraction is already in progress."));
        return;
    }

    const QString exe = findSevenZip();
    if (exe.isEmpty()) {
        emit extractionError(tr("7z.exe not found. Please install 7-Zip or add it to PATH."));
        return;
    }

    if (!QFileInfo::exists(archivePath)) {
        emit extractionError(tr("Archive not found: %1").arg(archivePath));
        return;
    }

    // Ensure output directory exists
    QDir().mkpath(outputDir);

    m_process = new QProcess(this);

    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &SevenZipExtractor::parseOutput);

    connect(m_process, &QProcess::finished, this,
            [this, outputDir](int exitCode, QProcess::ExitStatus exitStatus) {
                QProcess* proc = m_process;
                m_process = nullptr;

                if (exitStatus == QProcess::CrashExit) {
                    emit extractionError(tr("7z process crashed."));
                } else if (exitCode != 0) {
                    const QString errText = QString::fromLocal8Bit(proc->readAllStandardError()).trimmed();
                    const QString msg = errText.isEmpty()
                        ? tr("7z exited with code %1.").arg(exitCode)
                        : tr("7z exited with code %1: %2").arg(exitCode).arg(errText);
                    emit extractionError(msg);
                } else {
                    emit progressChanged(100);
                    emit extractionComplete(outputDir);
                }

                proc->deleteLater();
            });

    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (!m_process)
                    return;

                QString msg;
                switch (error) {
                case QProcess::FailedToStart:
                    msg = tr("Failed to start 7z process.");
                    break;
                case QProcess::Timedout:
                    msg = tr("7z process timed out.");
                    break;
                default:
                    msg = tr("7z process error (%1).").arg(static_cast<int>(error));
                    break;
                }

                QProcess* proc = m_process;
                m_process = nullptr;
                proc->deleteLater();

                emit extractionError(msg);
            });

    // Build arguments: x = extract with full paths, -o = output dir, -y = yes to all, -bsp1 = progress to stdout
    const QStringList args = {
        QStringLiteral("x"),
        archivePath,
        QStringLiteral("-o%1").arg(outputDir),
        QStringLiteral("-y"),
        QStringLiteral("-bsp1"),
    };

    m_process->start(exe, args);
}

void SevenZipExtractor::cancel()
{
    if (!m_process)
        return;

    m_process->kill();
    m_process->waitForFinished(3000);

    QProcess* proc = m_process;
    m_process = nullptr;
    proc->deleteLater();
}

bool SevenZipExtractor::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

void SevenZipExtractor::parseOutput()
{
    if (!m_process)
        return;

    // 7z with -bsp1 outputs progress lines like:
    //   " 42% 12 - somefile.txt"
    //   " 100%"
    // We look for a leading percentage value.
    static const QRegularExpression percentRe(QStringLiteral(R"(^\s*(\d{1,3})%)"),
                                              QRegularExpression::MultilineOption);

    const QByteArray data = m_process->readAllStandardOutput();
    const QString text = QString::fromLocal8Bit(data);

    // Process all percentage matches and take the last (most recent) one
    int lastPercent = -1;
    QRegularExpressionMatchIterator it = percentRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        bool ok = false;
        const int pct = match.captured(1).toInt(&ok);
        if (ok && pct >= 0 && pct <= 100)
            lastPercent = pct;
    }

    if (lastPercent >= 0)
        emit progressChanged(lastPercent);
}

} // namespace spw
