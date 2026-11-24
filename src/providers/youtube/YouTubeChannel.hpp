// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"

#include <QDateTime>
#include <QHash>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace chatterino {

class YouTubeProvider;
class YouTubeInnertubeReader;

class YouTubeChannel final : public Channel
{
public:
    YouTubeChannel(QString input, bool channelLookup,
                   YouTubeProvider *provider);
    ~YouTubeChannel() override;

    bool canSendMessage() const override;
    void sendMessage(const QString &message) override;
    bool isLive() const override;
    bool canReconnect() const override;
    void reconnect() override;
    const QString &getDisplayName() const override;
    const QString &getLocalizedName() const override;

    const QString &videoId() const;
    const QString &liveChatId() const;
    const QString &videoTitle() const;
    const QString &thumbnailUrl() const;
    const QDateTime &actualStartTime() const;
    qint64 concurrentViewers() const;
    bool isChannelLookup() const;
    bool hasKnownLiveStatus() const;
    const QString &lookupInput() const;
    void deleteMessage(const QString &messageId);
    void banUser(const QString &userId, int durationSeconds);
    void createPoll(const QString &question, const QStringList &options);

private:
    friend class YouTubeProvider;

    struct ReplyTarget {
        QString messageId;
        QString authorId;
        QString authorName;
        QString messageText;
        QDateTime timestamp;
    };

    QString videoId_;
    QString lookupInput_;
    QString liveChatId_;
    QString channelId_;
    QString liveStatusUrl_;
    QString channelTitle_;
    QString videoTitle_;
    QString thumbnailUrl_;
    QDateTime actualStartTime_;
    qint64 concurrentViewers_ = -1;
    YouTubeProvider *provider_;
    bool channelLookup_ = false;
    bool chatAvailable_ = false;
    bool channelStatusKnown_ = false;
    bool channelIsLive_ = false;
    bool statusRequestInFlight_ = false;
    bool offlineNoticeSent_ = false;
    quint64 liveStatusRequestId_ = 0;
    QTimer liveStatusTimer_;
    std::unique_ptr<YouTubeInnertubeReader> reader_;
    QHash<QString, ReplyTarget> replyTargets_;

    void start();
    void stopStream();
};

}  // namespace chatterino
