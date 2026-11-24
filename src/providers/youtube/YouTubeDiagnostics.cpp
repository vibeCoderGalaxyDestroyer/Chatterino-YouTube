// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeDiagnostics.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

namespace chatterino {

namespace {

constexpr qint64 MAX_LOG_SIZE = 2 * 1024 * 1024;
constexpr int BACKUP_COUNT = 3;

QMutex logMutex;
QString logFilePath;

void rotateLogIfNeeded()
{
    if (QFileInfo(logFilePath).size() < MAX_LOG_SIZE)
    {
        return;
    }

    QFile::remove(logFilePath + "." + QString::number(BACKUP_COUNT));
    for (auto index = BACKUP_COUNT - 1; index >= 1; --index)
    {
        const auto oldPath = logFilePath + "." + QString::number(index);
        const auto newPath = logFilePath + "." + QString::number(index + 1);
        if (QFile::exists(oldPath))
        {
            QFile::rename(oldPath, newPath);
        }
    }
    QFile::rename(logFilePath, logFilePath + ".1");
}

QString oneLine(QString text)
{
    text.replace('\r', ' ');
    text.replace('\n', ' ');
    return text;
}

}  // namespace

void initializeYouTubeDiagnostics(const QString &logDirectory)
{
    QMutexLocker lock(&logMutex);
    if (!QDir().mkpath(logDirectory))
    {
        logFilePath.clear();
        return;
    }
    logFilePath = QDir(logDirectory).filePath("youtube-diagnostics.log");
}

void logYouTubeDiagnostic(const QString &event, const QString &details)
{
    QMutexLocker lock(&logMutex);
    if (logFilePath.isEmpty())
    {
        return;
    }

    rotateLogIfNeeded();
    QFile file(logFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
    {
        return;
    }

    const auto timestamp =
        QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    const auto line = QStringLiteral("%1 | %2 | %3\n")
                          .arg(timestamp, oneLine(event), oneLine(details));
    file.write(line.toUtf8());
    file.flush();
}

}  // namespace chatterino
