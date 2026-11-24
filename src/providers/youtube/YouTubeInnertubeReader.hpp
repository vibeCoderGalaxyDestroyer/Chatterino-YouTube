// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

namespace chatterino {

class YouTubeInnertubeReader final : public QObject
{
public:
    struct VideoInfo {
        QString videoId;
        QString channelId;
        QString title;
        QString channelTitle;
        QString thumbnailUrl;
        QDateTime actualStartTime;
        qint64 concurrentViewers = -1;
        bool pageParsed = false;
        bool liveStateKnown = false;
        bool isLive = false;
    };

    struct ChannelSearchResult {
        QString channelId;
        QString title;
        QString url;
    };

    static VideoInfo parseWatchPage(const QByteArray &html);
    static QList<ChannelSearchResult> parseChannelSearchPage(
        const QByteArray &html);

    using BootstrapCallback = std::function<void(VideoInfo, bool)>;
    using MessageCallback = std::function<void(QJsonObject, bool)>;
    using ErrorCallback = std::function<void(QString)>;

    YouTubeInnertubeReader(QString videoId, BootstrapCallback bootstrapCallback,
                           MessageCallback messageCallback,
                           ErrorCallback errorCallback, QObject *parent = nullptr);
    ~YouTubeInnertubeReader() override = default;

    void start();
    void stop();

private:
    QString videoId_;
    QString apiKey_;
    QString clientVersion_;
    QString continuation_;
    BootstrapCallback bootstrapCallback_;
    MessageCallback messageCallback_;
    ErrorCallback errorCallback_;
    QTimer requestTimer_;
    QTimer diagnosticsTimer_;
    QElapsedTimer activeRequestElapsed_;
    int retryAttempt_ = 0;
    int lastRequestDurationMs_ = -1;
    quint64 diagnosticRequests_ = 0;
    quint64 diagnosticSuccesses_ = 0;
    quint64 diagnosticFailures_ = 0;
    quint64 diagnosticEvents_ = 0;
    bool requestInFlight_ = false;
    bool stopped_ = false;
    bool bootstrapLoaded_ = false;
    bool errorReported_ = false;
    bool retryLoadsPage_ = false;
    bool receivedChatResponse_ = false;

    void loadWatchPage();
    void requestChat();
    void scheduleRetry(bool reloadPage, int minimumDelayMs = 0);
    void reportError(const QString &message);
    void logHeartbeat();
};

}  // namespace chatterino
