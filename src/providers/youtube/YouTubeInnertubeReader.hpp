// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

namespace chatterino {

class YouTubeInnertubeReader final : public QObject
{
public:
    struct VideoInfo {
        QString title;
        QString channelTitle;
        QString thumbnailUrl;
    };

    using BootstrapCallback = std::function<void(VideoInfo, bool)>;
    using MessageCallback = std::function<void(QJsonObject)>;
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
    int retryAttempt_ = 0;
    bool stopped_ = false;
    bool bootstrapLoaded_ = false;
    bool errorReported_ = false;
    bool retryLoadsPage_ = false;

    void loadWatchPage();
    void requestChat();
    void scheduleRetry(bool reloadPage, int minimumDelayMs = 0);
    void reportError(const QString &message);
};

}  // namespace chatterino
