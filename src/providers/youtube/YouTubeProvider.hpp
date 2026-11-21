// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"

#include <pajlada/signals/signal.hpp>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpServer>

#include <functional>
#include <memory>
#include <optional>

namespace chatterino {

class YouTubeChannel;
struct Emote;

class YouTubeProvider final : public QObject
{
public:
    YouTubeProvider();
    ~YouTubeProvider() override;

    pajlada::Signals::NoArgSignal authChanged;

    ChannelPtr getOrAddChannel(const QString &urlOrVideoId);
    ChannelPtr getChannelOrEmpty(const QString &urlOrVideoId);
    static bool isValidInput(const QString &input);

    bool isLoggedIn() const;
    QString accountName() const;
    QString authError() const;
    bool startLogin(QString *error = nullptr);
    void logout();
    void sendMessage(YouTubeChannel *channel, const QString &message);

    void loadVideo(YouTubeChannel *channel);
    void loadVideoDetails(YouTubeChannel *channel);
    void loadLiveVideoForChannel(YouTubeChannel *channel);
    void searchLiveVideo(YouTubeChannel *channel, const QString &channelId);
    void deleteMessage(YouTubeChannel *channel, const QString &messageId);
    void banUser(YouTubeChannel *channel, const QString &userId,
                 int durationSeconds);
    void createPoll(YouTubeChannel *channel, const QString &question,
                    const QStringList &options);

    static std::optional<QString> extractVideoId(const QString &urlOrVideoId);

private:
    QHash<QString, std::weak_ptr<YouTubeChannel>> channels_;
    QHash<QString, std::shared_ptr<const Emote>> customEmotes_;
    QTcpServer oauthServer_;
    QString oauthState_;
    QString codeVerifier_;
    QString redirectUri_;
    QString authError_;
    bool accountNameLookupInProgress_{};
    quint64 accountNameRequestId_{};

    void loadAccountName();
    void processMessageBatch(YouTubeChannel *channel, const QJsonObject &root);
    void startInnertubeReader(YouTubeChannel *channel);
    void exchangeAuthorizationCode(const QString &code,
                                   const QString &redirectUri);
    void refreshAccessToken(std::function<void(bool)> callback);
    void ensureAccessToken(std::function<void(bool)> callback);
    void ensureApiAccess(YouTubeChannel *channel,
                         std::function<void()> callback);
};

}  // namespace chatterino
