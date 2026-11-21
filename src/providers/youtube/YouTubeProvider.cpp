// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeProvider.hpp"

#include "Application.hpp"
#include "common/enums/MessageContext.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Emote.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageColor.hpp"
#include "messages/MessageElement.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "providers/youtube/YouTubeInnertubeReader.hpp"
#include "singletons/Settings.hpp"

#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <utility>
#include <variant>

namespace chatterino {

namespace {

constexpr auto YOUTUBE_API = "https://www.googleapis.com/youtube/v3/";
constexpr auto YOUTUBE_SCOPE =
    "https://www.googleapis.com/auth/youtube.force-ssl";

enum class ChannelLookupKind {
    Id,
    Handle,
    Username,
    Name,
};

struct ChannelLookup {
    ChannelLookupKind kind;
    QString value;
};

std::optional<ChannelLookup> parseChannelLookup(const QString &input)
{
    const auto trimmed = input.trimmed();
    if (trimmed.isEmpty())
    {
        return std::nullopt;
    }

    static const QRegularExpression handlePattern("^@[A-Za-z0-9._-]+$");
    if (handlePattern.match(trimmed).hasMatch())
    {
        return ChannelLookup{ChannelLookupKind::Handle, trimmed};
    }

    static const QRegularExpression channelIdPattern("^UC[A-Za-z0-9_-]{22}$");
    if (channelIdPattern.match(trimmed).hasMatch())
    {
        return ChannelLookup{ChannelLookupKind::Id, trimmed};
    }

    const bool looksLikeUrl =
        trimmed.contains("://") ||
        trimmed.startsWith("www.", Qt::CaseInsensitive) ||
        trimmed.startsWith("youtube.com", Qt::CaseInsensitive) ||
        trimmed.startsWith("m.youtube.com", Qt::CaseInsensitive);
    if (looksLikeUrl)
    {
        const auto url = QUrl::fromUserInput(trimmed);
        const auto host = url.host().toLower();
        if (host != "youtube.com" && host != "www.youtube.com" &&
            host != "m.youtube.com" && !host.endsWith(".youtube.com"))
        {
            return std::nullopt;
        }

        const auto segments = url.path().split('/', Qt::SkipEmptyParts);
        if (segments.isEmpty())
        {
            return std::nullopt;
        }

        const auto first = segments.at(0);
        if (handlePattern.match(first).hasMatch())
        {
            return ChannelLookup{ChannelLookupKind::Handle, first};
        }
        if (segments.size() >= 2 && segments.at(0) == "channel" &&
            channelIdPattern.match(segments.at(1)).hasMatch())
        {
            return ChannelLookup{ChannelLookupKind::Id, segments.at(1)};
        }
        if (segments.size() >= 2 && segments.at(0) == "user")
        {
            return ChannelLookup{ChannelLookupKind::Username, segments.at(1)};
        }
        if (segments.size() >= 2 && segments.at(0) == "c")
        {
            return ChannelLookup{ChannelLookupKind::Name, segments.at(1)};
        }
        return std::nullopt;
    }

    return ChannelLookup{ChannelLookupKind::Name, trimmed};
}

QString channelKey(const QString &input)
{
    if (const auto videoId = YouTubeProvider::extractVideoId(input))
    {
        return "video:" + *videoId;
    }
    const auto lookup = parseChannelLookup(input);
    if (!lookup)
    {
        return {};
    }
    return QString("channel:%1:%2")
        .arg(static_cast<int>(lookup->kind))
        .arg(lookup->value.toCaseFolded());
}

QString randomUrlSafe(size_t byteCount)
{
    QByteArray bytes;
    bytes.reserve(static_cast<qsizetype>(byteCount));
    auto *random = QRandomGenerator::system();
    while (static_cast<size_t>(bytes.size()) < byteCount)
    {
        const auto value = random->generate();
        for (size_t i = 0;
             i < sizeof(value) && static_cast<size_t>(bytes.size()) < byteCount;
             ++i)
        {
            bytes.append(static_cast<char>((value >> (i * 8)) & 0xff));
        }
    }
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding |
                                              QByteArray::OmitTrailingEquals));
}

NetworkRequest makeApiRequest(const QString &endpoint, const QUrlQuery &query)
{
    QUrl url(QString::fromLatin1(YOUTUBE_API) + endpoint);
    url.setQuery(query);
    const auto accessToken = getSettings()->youtubeAccessToken.getValue();
    const auto expiry = getSettings()->youtubeTokenExpiry.getValue();
    if (!accessToken.isEmpty() &&
        expiry > QDateTime::currentSecsSinceEpoch() + 60)
    {
        return NetworkRequest(url).header("Authorization",
                                          "Bearer " + accessToken);
    }

    const auto apiKey = getSettings()->youtubeApiKey.getValue();
    if (!apiKey.isEmpty())
    {
        return NetworkRequest(url).header("X-Goog-Api-Key", apiKey);
    }
    return NetworkRequest(url);
}

QByteArray formBody(const QUrlQuery &form)
{
    return form.toString(QUrl::FullyEncoded).toUtf8();
}

void addApiError(YouTubeChannel *channel, const NetworkResult &result)
{
    auto response = result.parseJson();
    auto message =
        response.value("error").toObject().value("message").toString();
    if (message.isEmpty())
    {
        message = result.formatError();
    }
    channel->addSystemMessage("YouTube: " + message);
}

void sendOAuthResponse(QTcpSocket *socket, int status,
                       const QByteArray &statusText, const QByteArray &body)
{
    QByteArray response{"HTTP/1.1 "};
    response += QByteArray::number(status) + " " + statusText +
                "\r\nContent-Type: text/plain; charset=utf-8\r\n"
                "Connection: close\r\nContent-Length: ";
    response += QByteArray::number(body.size()) + "\r\n\r\n" + body;
    socket->write(response);
    socket->disconnectFromHost();
}

}  // namespace

YouTubeProvider::YouTubeProvider()
{
    QObject::connect(
        &this->oauthServer_, &QTcpServer::newConnection, this, [this] {
            while (this->oauthServer_.hasPendingConnections())
            {
                auto *socket = this->oauthServer_.nextPendingConnection();
                QObject::connect(
                    socket, &QTcpSocket::readyRead, this, [this, socket] {
                        if (socket->property("oauthHandled").toBool())
                        {
                            return;
                        }
                        auto request =
                            socket->property("oauthBuffer").toByteArray();
                        request.append(socket->readAll());
                        socket->setProperty("oauthBuffer", request);
                        if (!request.contains("\r\n"))
                        {
                            return;
                        }
                        const auto requestLine =
                            QString::fromUtf8(request).section("\r\n", 0, 0);
                        const auto parts = requestLine.split(' ');
                        if (parts.size() < 2)
                        {
                            return;
                        }

                        socket->setProperty("oauthHandled", true);
                        const QUrl callbackUrl("http://127.0.0.1" +
                                               parts.at(1));
                        const QUrlQuery query(callbackUrl);
                        const auto state = query.queryItemValue("state");
                        const auto code = query.queryItemValue("code");
                        const auto oauthError = query.queryItemValue("error");
                        if (state != this->oauthState_)
                        {
                            this->authError_ =
                                "Google returned an invalid sign-in callback.";
                            this->authChanged.invoke();
                            sendOAuthResponse(
                                socket, 400, "Bad Request",
                                "Invalid YouTube sign-in callback.");
                            return;
                        }
                        this->oauthServer_.close();
                        const QByteArray body =
                            oauthError.isEmpty()
                                ? "Sign-in complete. Return to Chatterino."
                                : "Sign-in canceled. Return to Chatterino.";
                        sendOAuthResponse(socket, 200, "OK", body);
                        if (!oauthError.isEmpty())
                        {
                            this->authError_ =
                                query.queryItemValue("error_description");
                            if (this->authError_.isEmpty())
                            {
                                this->authError_ = oauthError;
                            }
                            this->oauthState_.clear();
                            this->codeVerifier_.clear();
                            this->authChanged.invoke();
                            return;
                        }
                        if (code.isEmpty())
                        {
                            this->authError_ =
                                "Google returned no authorization code.";
                            this->oauthState_.clear();
                            this->codeVerifier_.clear();
                            this->authChanged.invoke();
                            return;
                        }
                        this->exchangeAuthorizationCode(code,
                                                        this->redirectUri_);
                    });
            }
        });
}

YouTubeProvider::~YouTubeProvider()
{
    for (auto it = this->channels_.begin(); it != this->channels_.end(); ++it)
    {
        if (auto channel = it.value().lock())
        {
            channel->stopStream();
        }
    }
}

bool YouTubeProvider::isLoggedIn() const
{
    return !getSettings()->youtubeRefreshToken.getValue().isEmpty() ||
           !getSettings()->youtubeAccessToken.getValue().isEmpty();
}

QString YouTubeProvider::authError() const
{
    return this->authError_;
}

bool YouTubeProvider::startLogin(QString *error)
{
    const auto clientId =
        getSettings()->youtubeOAuthClientId.getValue().trimmed();
    if (clientId.isEmpty())
    {
        if (error)
        {
            *error = "Add a Google Desktop OAuth client ID in Settings > "
                     "Accounts first.";
        }
        return false;
    }
    if (this->oauthServer_.isListening())
    {
        this->oauthServer_.close();
    }
    if (!this->oauthServer_.listen(QHostAddress::LocalHost, 0))
    {
        if (error)
        {
            *error = "Could not start the local sign-in callback.";
        }
        return false;
    }

    this->authError_.clear();
    this->authChanged.invoke();

    this->redirectUri_ =
        QString("http://127.0.0.1:%1/").arg(this->oauthServer_.serverPort());
    this->oauthState_ = randomUrlSafe(24);
    this->codeVerifier_ = randomUrlSafe(32);
    const auto challenge = QString::fromLatin1(
        QCryptographicHash::hash(this->codeVerifier_.toUtf8(),
                                 QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding |
                      QByteArray::OmitTrailingEquals));

    QUrl authUrl("https://accounts.google.com/o/oauth2/v2/auth");
    QUrlQuery query;
    query.addQueryItem("client_id", clientId);
    query.addQueryItem("redirect_uri", this->redirectUri_);
    query.addQueryItem("response_type", "code");
    query.addQueryItem("scope", QLatin1String(YOUTUBE_SCOPE));
    query.addQueryItem("state", this->oauthState_);
    query.addQueryItem("code_challenge", challenge);
    query.addQueryItem("code_challenge_method", "S256");
    query.addQueryItem("access_type", "offline");
    query.addQueryItem("prompt", "consent");
    authUrl.setQuery(query);
    if (!QDesktopServices::openUrl(authUrl))
    {
        this->oauthServer_.close();
        this->oauthState_.clear();
        this->codeVerifier_.clear();
        if (error)
        {
            *error = "Could not open your default browser for Google sign-in.";
        }
        return false;
    }
    return true;
}

void YouTubeProvider::logout()
{
    getSettings()->youtubeAccessToken.setValue({});
    getSettings()->youtubeRefreshToken.setValue({});
    getSettings()->youtubeTokenExpiry.setValue(0);
    this->authError_.clear();
    this->authChanged.invoke();
}

void YouTubeProvider::exchangeAuthorizationCode(const QString &code,
                                                const QString &redirectUri)
{
    QUrlQuery form;
    form.addQueryItem("client_id",
                      getSettings()->youtubeOAuthClientId.getValue().trimmed());
    const auto clientSecret =
        getSettings()->youtubeOAuthClientSecret.getValue().trimmed();
    if (!clientSecret.isEmpty())
    {
        form.addQueryItem("client_secret", clientSecret);
    }
    form.addQueryItem("code", code);
    form.addQueryItem("code_verifier", this->codeVerifier_);
    form.addQueryItem("redirect_uri", redirectUri);
    form.addQueryItem("grant_type", "authorization_code");
    this->codeVerifier_.clear();
    this->oauthState_.clear();

    auto request = NetworkRequest(QUrl("https://oauth2.googleapis.com/token"),
                                  NetworkRequestType::Post);
    std::move(request)
        .header("Content-Type", "application/x-www-form-urlencoded")
        .payload(formBody(form))
        .caller(this)
        .onSuccess([this](const NetworkResult &result) {
            const auto token = result.parseJson();
            const auto accessToken = token.value("access_token").toString();
            if (accessToken.isEmpty())
            {
                this->authError_ = token.value("error_description").toString();
                if (this->authError_.isEmpty())
                {
                    this->authError_ = "Google did not return an access token.";
                }
                this->authChanged.invoke();
                return;
            }
            this->authError_.clear();
            getSettings()->youtubeAccessToken.setValue(accessToken);
            const auto refreshToken = token.value("refresh_token").toString();
            if (!refreshToken.isEmpty())
            {
                getSettings()->youtubeRefreshToken.setValue(refreshToken);
            }
            getSettings()->youtubeTokenExpiry.setValue(
                static_cast<int>(QDateTime::currentSecsSinceEpoch() +
                                 token.value("expires_in").toInt()));
            this->authChanged.invoke();
        })
        .onError([this](const NetworkResult &result) {
            const auto response = result.parseJson();
            const auto errorCode = response.value("error").toString();
            this->authError_ = response.value("error_description").toString();
            if (this->authError_.isEmpty())
            {
                this->authError_ = result.formatError();
            }
            if (!errorCode.isEmpty())
            {
                this->authError_ = errorCode + ": " + this->authError_;
            }
            qCWarning(chatterinoCommon)
                << "Could not complete YouTube sign-in." << this->authError_;
            this->authChanged.invoke();
        })
        .execute();
}

void YouTubeProvider::refreshAccessToken(std::function<void(bool)> callback)
{
    const auto refreshToken = getSettings()->youtubeRefreshToken.getValue();
    if (refreshToken.isEmpty())
    {
        callback(false);
        return;
    }
    QUrlQuery form;
    form.addQueryItem("client_id",
                      getSettings()->youtubeOAuthClientId.getValue().trimmed());
    const auto clientSecret =
        getSettings()->youtubeOAuthClientSecret.getValue().trimmed();
    if (!clientSecret.isEmpty())
    {
        form.addQueryItem("client_secret", clientSecret);
    }
    form.addQueryItem("refresh_token", refreshToken);
    form.addQueryItem("grant_type", "refresh_token");
    const auto callbackCopy = callback;
    auto request = NetworkRequest(QUrl("https://oauth2.googleapis.com/token"),
                                  NetworkRequestType::Post);
    std::move(request)
        .header("Content-Type", "application/x-www-form-urlencoded")
        .payload(formBody(form))
        .caller(this)
        .onSuccess(
            [callback = callbackCopy](const NetworkResult &result) mutable {
                const auto token = result.parseJson();
                const auto accessToken = token.value("access_token").toString();
                if (accessToken.isEmpty())
                {
                    callback(false);
                    return;
                }
                getSettings()->youtubeAccessToken.setValue(accessToken);
                getSettings()->youtubeTokenExpiry.setValue(
                    static_cast<int>(QDateTime::currentSecsSinceEpoch() +
                                     token.value("expires_in").toInt()));
                callback(true);
            })
        .onError([callback = callbackCopy](const NetworkResult &) mutable {
            callback(false);
        })
        .execute();
}

void YouTubeProvider::ensureAccessToken(std::function<void(bool)> callback)
{
    const auto token = getSettings()->youtubeAccessToken.getValue();
    const auto expiry = getSettings()->youtubeTokenExpiry.getValue();
    if (!token.isEmpty() && expiry > QDateTime::currentSecsSinceEpoch() + 60)
    {
        callback(true);
        return;
    }
    this->refreshAccessToken(std::move(callback));
}

void YouTubeProvider::ensureApiAccess(YouTubeChannel *channel,
                                      std::function<void()> callback)
{
    auto channelPtr =
        std::dynamic_pointer_cast<YouTubeChannel>(channel->shared_from_this());
    const auto apiKey = getSettings()->youtubeApiKey.getValue();
    if (this->isLoggedIn())
    {
        this->ensureAccessToken(
            [channelPtr, apiKey,
             callback = std::move(callback)](bool success) mutable {
                if (success || !apiKey.isEmpty())
                {
                    callback();
                    return;
                }
                channelPtr->addSystemMessage(
                    "YouTube sign-in expired. Sign in again, or add an API key "
                    "to resolve YouTube channels.");
            });
        return;
    }

    if (apiKey.isEmpty())
    {
        channelPtr->addSystemMessage(
            "YouTube channel lookup needs sign-in or an API key. Direct "
            "public video links can be viewed without either.");
        return;
    }
    callback();
}

void YouTubeProvider::sendMessage(YouTubeChannel *channel,
                                  const QString &message)
{
    auto channelPtr =
        std::dynamic_pointer_cast<YouTubeChannel>(channel->shared_from_this());
    if (channelPtr->liveChatId().isEmpty())
    {
        channelPtr->addSystemMessage(
            "YouTube has not provided the official chat ID yet. Wait for "
            "video details to load, then try again.");
        return;
    }
    this->ensureAccessToken([this, channelPtr, message](bool success) {
        if (!success)
        {
            channelPtr->addSystemMessage(
                "Sign in to YouTube to send chat messages.");
            return;
        }
        QUrlQuery query;
        query.addQueryItem("part", "snippet");
        QJsonObject root{
            {"snippet",
             QJsonObject{
                 {"liveChatId", channelPtr->liveChatId()},
                 {"type", "textMessageEvent"},
                 {"textMessageDetails", QJsonObject{{"messageText", message}}},
             }},
        };
        auto request = makeApiRequest("liveChat/messages", query);
        std::move(request)
            .type(NetworkRequestType::Post)
            .json(root)
            .caller(this)
            .onError([channelPtr](const NetworkResult &result) {
                addApiError(channelPtr.get(), result);
            })
            .execute();
    });
}

void YouTubeProvider::deleteMessage(YouTubeChannel *channel,
                                    const QString &messageId)
{
    auto channelPtr =
        std::dynamic_pointer_cast<YouTubeChannel>(channel->shared_from_this());
    if (channelPtr->liveChatId().isEmpty())
    {
        channelPtr->addSystemMessage(
            "YouTube has not provided the official chat ID yet. Wait for "
            "video details to load, then try again.");
        return;
    }
    this->ensureAccessToken([this, channelPtr, messageId](bool success) {
        if (!success)
        {
            channelPtr->addSystemMessage(
                "Sign in to YouTube to moderate live chat.");
            return;
        }

        QUrlQuery query;
        query.addQueryItem("id", messageId);
        auto request = makeApiRequest("liveChat/messages", query);
        std::move(request)
            .type(NetworkRequestType::Delete)
            .caller(this)
            .onSuccess([channelPtr, messageId](const NetworkResult &) {
                channelPtr->disableMessage(messageId);
            })
            .onError([channelPtr](const NetworkResult &result) {
                addApiError(channelPtr.get(), result);
            })
            .execute();
    });
}

void YouTubeProvider::banUser(YouTubeChannel *channel, const QString &userId,
                              int durationSeconds)
{
    auto channelPtr =
        std::dynamic_pointer_cast<YouTubeChannel>(channel->shared_from_this());
    if (userId.isEmpty())
    {
        channelPtr->addSystemMessage(
            "YouTube did not provide a channel ID for this user.");
        return;
    }
    if (channelPtr->liveChatId().isEmpty())
    {
        channelPtr->addSystemMessage(
            "YouTube has not provided the official chat ID yet. Wait for "
            "video details to load, then try again.");
        return;
    }
    this->ensureAccessToken(
        [this, channelPtr, userId, durationSeconds](bool success) {
            if (!success)
            {
                channelPtr->addSystemMessage(
                    "Sign in to YouTube to moderate live chat.");
                return;
            }

            QUrlQuery query;
            query.addQueryItem("part", "snippet");
            QJsonObject snippet{
                {"liveChatId", channelPtr->liveChatId()},
                {"type", durationSeconds > 0 ? "temporary" : "permanent"},
                {"bannedUserDetails", QJsonObject{{"channelId", userId}}},
            };
            if (durationSeconds > 0)
            {
                snippet.insert("banDurationSeconds", durationSeconds);
            }
            auto request = makeApiRequest("liveChat/bans", query);
            std::move(request)
                .type(NetworkRequestType::Post)
                .json(QJsonObject{{"snippet", snippet}})
                .caller(this)
                .onError([channelPtr](const NetworkResult &result) {
                    addApiError(channelPtr.get(), result);
                })
                .execute();
        });
}

void YouTubeProvider::createPoll(YouTubeChannel *channel,
                                 const QString &question,
                                 const QStringList &options)
{
    auto channelPtr =
        std::dynamic_pointer_cast<YouTubeChannel>(channel->shared_from_this());
    if (channelPtr->liveChatId().isEmpty())
    {
        channelPtr->addSystemMessage(
            "YouTube has not provided the official chat ID yet. Wait for "
            "video details to load, then try again.");
        return;
    }
    this->ensureAccessToken(
        [this, channelPtr, question, options](bool success) {
            if (!success)
            {
                channelPtr->addSystemMessage(
                    "Sign in to YouTube to create a live chat poll.");
                return;
            }

            QJsonArray pollOptions;
            for (const auto &option : options)
            {
                pollOptions.append(QJsonObject{{"optionText", option}});
            }
            const QJsonObject root{
                {"snippet",
                 QJsonObject{
                     {"liveChatId", channelPtr->liveChatId()},
                     {"type", "pollEvent"},
                     {
                         "pollDetails",
                         QJsonObject{{"metadata",
                                      QJsonObject{{"questionText", question},
                                                  {"options", pollOptions}}}},
                     },
                 }},
            };
            QUrlQuery query;
            query.addQueryItem("part", "snippet");
            auto request = makeApiRequest("liveChat/messages", query);
            std::move(request)
                .type(NetworkRequestType::Post)
                .json(root)
                .caller(this)
                .onError([channelPtr](const NetworkResult &result) {
                    addApiError(channelPtr.get(), result);
                })
                .execute();
        });
}

std::optional<QString> YouTubeProvider::extractVideoId(
    const QString &urlOrVideoId)
{
    const auto trimmed = urlOrVideoId.trimmed();
    static const QRegularExpression idPattern("^[A-Za-z0-9_-]{11}$");
    if (idPattern.match(trimmed).hasMatch())
    {
        return trimmed;
    }

    const QUrl url = QUrl::fromUserInput(trimmed);
    const auto host = url.host().toLower();
    QString videoId;
    if (host == "youtu.be" || host.endsWith(".youtu.be"))
    {
        videoId = url.path().section('/', 1, 1);
    }
    else if (host == "youtube.com" || host.endsWith(".youtube.com"))
    {
        const QUrlQuery query(url);
        videoId = query.queryItemValue("v");
        if (videoId.isEmpty())
        {
            const auto segments = url.path().split('/', Qt::SkipEmptyParts);
            if (segments.size() >= 2 &&
                (segments.front() == "live" || segments.front() == "shorts"))
            {
                videoId = segments.at(1);
            }
        }
    }

    if (idPattern.match(videoId).hasMatch())
    {
        return videoId;
    }
    return std::nullopt;
}

bool YouTubeProvider::isValidInput(const QString &input)
{
    return extractVideoId(input).has_value() ||
           parseChannelLookup(input).has_value();
}

ChannelPtr YouTubeProvider::getOrAddChannel(const QString &urlOrVideoId)
{
    const auto videoId = extractVideoId(urlOrVideoId);
    const auto key = channelKey(urlOrVideoId);
    if (key.isEmpty())
    {
        return Channel::getEmpty();
    }

    auto existing = this->channels_.find(key);
    if (existing != this->channels_.end())
    {
        if (auto channel = existing.value().lock())
        {
            return channel;
        }
    }

    const auto input = videoId.value_or(urlOrVideoId.trimmed());
    auto channel = std::make_shared<YouTubeChannel>(input, !videoId, this);
    this->channels_[key] = channel;
    channel->start();
    return channel;
}

ChannelPtr YouTubeProvider::getChannelOrEmpty(const QString &urlOrVideoId)
{
    const auto key = channelKey(urlOrVideoId);
    if (key.isEmpty())
    {
        return Channel::getEmpty();
    }
    auto it = this->channels_.find(key);
    if (it == this->channels_.end())
    {
        return Channel::getEmpty();
    }
    auto channel = it.value().lock();
    return channel ? std::static_pointer_cast<Channel>(channel)
                   : Channel::getEmpty();
}

void YouTubeProvider::loadVideo(YouTubeChannel *channel)
{
    auto channelPtr =
        std::dynamic_pointer_cast<YouTubeChannel>(channel->shared_from_this());
    if (channelPtr->isChannelLookup())
    {
        this->ensureApiAccess(channel, [this, channelPtr] {
            this->loadLiveVideoForChannel(channelPtr.get());
        });
        return;
    }

    this->startInnertubeReader(channelPtr.get());

    // Innertube supplies public chat without a Data API key. Fetch the
    // official liveChatId only when OAuth is configured so the existing
    // send and moderation actions remain available.
    if (this->isLoggedIn())
    {
        this->ensureApiAccess(channel, [this, channelPtr] {
            this->loadVideoDetails(channelPtr.get());
        });
    }
}

void YouTubeProvider::startInnertubeReader(YouTubeChannel *channel)
{
    if (channel->videoId_.isEmpty())
    {
        return;
    }
    if (channel->reader_)
    {
        channel->reader_->stop();
    }

    auto channelPtr =
        std::dynamic_pointer_cast<YouTubeChannel>(channel->shared_from_this());
    const auto weakChannel = std::weak_ptr<YouTubeChannel>(channelPtr);
    channel->reader_ = std::make_unique<YouTubeInnertubeReader>(
        channel->videoId_,
        [weakChannel](YouTubeInnertubeReader::VideoInfo info,
                      bool chatAvailable) {
            auto channel = weakChannel.lock();
            if (!channel)
            {
                return;
            }
            if (!info.title.isEmpty())
            {
                channel->videoTitle_ = std::move(info.title);
            }
            if (!info.channelTitle.isEmpty())
            {
                channel->channelTitle_ = std::move(info.channelTitle);
            }
            if (!info.thumbnailUrl.isEmpty())
            {
                channel->thumbnailUrl_ = std::move(info.thumbnailUrl);
            }
            channel->chatAvailable_ = chatAvailable;
            channel->displayNameChanged.invoke();
            if (chatAvailable)
            {
                channel->addSystemMessage(
                    "Connected to YouTube live chat using Innertube.");
            }
        },
        [this, weakChannel](QJsonObject root) {
            if (auto channel = weakChannel.lock())
            {
                this->processMessageBatch(channel.get(), root);
            }
        },
        [weakChannel](QString error) {
            if (auto channel = weakChannel.lock())
            {
                channel->addSystemMessage("YouTube chat: " + error);
            }
        });
    channel->reader_->start();
}

void YouTubeProvider::loadLiveVideoForChannel(YouTubeChannel *channel)
{
    const auto lookup = parseChannelLookup(channel->lookupInput());
    if (!lookup)
    {
        channel->addSystemMessage("YouTube channel name or URL is invalid.");
        return;
    }

    auto channelPtr =
        std::dynamic_pointer_cast<YouTubeChannel>(channel->shared_from_this());
    const auto findLiveStream = [this, channelPtr](const QString &channelId) {
        if (channelId.isEmpty())
        {
            channelPtr->addSystemMessage("YouTube channel was not found.");
            return;
        }
        this->searchLiveVideo(channelPtr.get(), channelId);
    };

    if (lookup->kind == ChannelLookupKind::Name)
    {
        QUrlQuery query;
        query.addQueryItem("part", "snippet");
        query.addQueryItem("type", "channel");
        query.addQueryItem("q", lookup->value);
        query.addQueryItem("maxResults", "5");
        auto request = makeApiRequest("search", query);
        std::move(request)
            .caller(this)
            .onSuccess([channelPtr, findLiveStream,
                        lookup](const NetworkResult &result) {
                const auto items = result.parseJson().value("items").toArray();
                QString matchedChannelId;
                int exactMatchCount = 0;
                for (const auto &itemValue : items)
                {
                    const auto item = itemValue.toObject();
                    const auto snippet = item.value("snippet").toObject();
                    const auto title = snippet.value("title").toString();
                    const auto id = item.value("id")
                                        .toObject()
                                        .value("channelId")
                                        .toString();
                    if (id.isEmpty())
                    {
                        continue;
                    }
                    if (title.compare(lookup->value, Qt::CaseInsensitive) == 0)
                    {
                        matchedChannelId = id;
                        ++exactMatchCount;
                    }
                }

                if (exactMatchCount > 1)
                {
                    channelPtr->addSystemMessage(
                        "Several YouTube channels match this name. Use its "
                        "@handle or channel URL.");
                    return;
                }
                if (exactMatchCount == 0 || matchedChannelId.isEmpty())
                {
                    channelPtr->addSystemMessage(
                        "Could not find an exact YouTube channel name. Try its "
                        "@handle or channel URL.");
                    return;
                }
                findLiveStream(matchedChannelId);
            })
            .onError([channelPtr](const NetworkResult &result) {
                addApiError(channelPtr.get(), result);
            })
            .execute();
        return;
    }

    QUrlQuery query;
    query.addQueryItem("part", "id");
    switch (lookup->kind)
    {
        case ChannelLookupKind::Id:
            query.addQueryItem("id", lookup->value);
            break;
        case ChannelLookupKind::Handle:
            query.addQueryItem("forHandle", lookup->value);
            break;
        case ChannelLookupKind::Username:
            query.addQueryItem("forUsername", lookup->value);
            break;
        case ChannelLookupKind::Name:
            break;
    }
    auto request = makeApiRequest("channels", query);
    std::move(request)
        .caller(this)
        .onSuccess([channelPtr, findLiveStream](const NetworkResult &result) {
            const auto items = result.parseJson().value("items").toArray();
            if (items.isEmpty())
            {
                channelPtr->addSystemMessage("YouTube channel was not found.");
                return;
            }
            const auto channelId =
                items.at(0).toObject().value("id").toString();
            findLiveStream(channelId);
        })
        .onError([channelPtr](const NetworkResult &result) {
            addApiError(channelPtr.get(), result);
        })
        .execute();
}

void YouTubeProvider::searchLiveVideo(YouTubeChannel *channel,
                                      const QString &channelId)
{
    QUrlQuery query;
    query.addQueryItem("part", "snippet");
    query.addQueryItem("channelId", channelId);
    query.addQueryItem("eventType", "live");
    query.addQueryItem("type", "video");
    query.addQueryItem("order", "date");
    query.addQueryItem("maxResults", "1");
    auto channelPtr =
        std::dynamic_pointer_cast<YouTubeChannel>(channel->shared_from_this());
    auto request = makeApiRequest("search", query);
    std::move(request)
        .caller(this)
        .onSuccess([this, channelPtr](const NetworkResult &result) {
            const auto items = result.parseJson().value("items").toArray();
            if (items.isEmpty())
            {
                channelPtr->addSystemMessage("No active YouTube live stream "
                                             "was found for this channel.");
                return;
            }
            const auto videoId = items.at(0)
                                     .toObject()
                                     .value("id")
                                     .toObject()
                                     .value("videoId")
                                     .toString();
            if (videoId.isEmpty())
            {
                channelPtr->addSystemMessage("YouTube did not return an active "
                                             "live video for this channel.");
                return;
            }
            channelPtr->videoId_ = videoId;
            this->startInnertubeReader(channelPtr.get());
            this->loadVideoDetails(channelPtr.get());
        })
        .onError([channelPtr](const NetworkResult &result) {
            addApiError(channelPtr.get(), result);
        })
        .execute();
}

void YouTubeProvider::loadVideoDetails(YouTubeChannel *channel)
{
    QUrlQuery query;
    query.addQueryItem("part", "snippet,liveStreamingDetails");
    query.addQueryItem("id", channel->videoId());
    auto channelPtr =
        std::dynamic_pointer_cast<YouTubeChannel>(channel->shared_from_this());
    auto request = makeApiRequest("videos", query);
    std::move(request)
        .caller(this)
        .onSuccess([channelPtr](const NetworkResult &result) {
            auto *channel = channelPtr.get();
            const auto items = result.parseJson().value("items").toArray();
            if (items.isEmpty())
            {
                channel->addSystemMessage("YouTube video was not found.");
                return;
            }

            const auto video = items.at(0).toObject();
            const auto snippet = video.value("snippet").toObject();
            const auto liveDetails =
                video.value("liveStreamingDetails").toObject();
            channel->channelTitle_ =
                snippet.value("channelTitle").toString().trimmed();
            channel->videoTitle_ = snippet.value("title").toString().trimmed();
            channel->thumbnailUrl_.clear();
            const auto thumbnails = snippet.value("thumbnails").toObject();
            const QStringList thumbnailPriority{
                "uhd",      "qhd",  "fhd",    "maxres",
                "standard", "high", "medium", "default",
            };
            for (const auto &thumbnailName : thumbnailPriority)
            {
                const auto url = thumbnails.value(thumbnailName)
                                     .toObject()
                                     .value("url")
                                     .toString();
                if (!url.isEmpty())
                {
                    channel->thumbnailUrl_ = url;
                    break;
                }
            }
            channel->actualStartTime_ = QDateTime::fromString(
                liveDetails.value("actualStartTime").toString(), Qt::ISODate);
            const auto viewerCount = liveDetails.value("concurrentViewers");
            if (viewerCount.isString())
            {
                bool ok = false;
                channel->concurrentViewers_ =
                    viewerCount.toString().toLongLong(&ok);
                if (!ok)
                {
                    channel->concurrentViewers_ = -1;
                }
            }
            else
            {
                channel->concurrentViewers_ = viewerCount.toInteger(-1);
            }
            channel->liveChatId_ =
                liveDetails.value("activeLiveChatId").toString();
            channel->displayNameChanged.invoke();
        })
        .onError([channelPtr](const NetworkResult &result) {
            addApiError(channelPtr.get(), result);
        })
        .execute();
}

void YouTubeProvider::processMessageBatch(YouTubeChannel *channel,
                                          const QJsonObject &root)
{
    const auto items = root.value("items").toArray();
    for (const auto &itemValue : items)
    {
        const auto item = itemValue.toObject();
        const auto snippet = item.value("snippet").toObject();
        const auto author = item.value("authorDetails").toObject();
        const auto type = snippet.value("type").toString();
        QString text = snippet.value("displayMessage").toString();
        if (type == "textMessageEvent")
        {
            text = snippet.value("textMessageDetails")
                       .toObject()
                       .value("messageText")
                       .toString();
        }
        if (type == "superChatEvent")
        {
            const auto amount = snippet.value("superChatDetails")
                                    .toObject()
                                    .value("amountDisplayString")
                                    .toString();
            text.prepend(amount.isEmpty() ? "[Super Chat] "
                                          : "[Super Chat " + amount + "] ");
        }
        else if (type == "superStickerEvent")
        {
            if (text.isEmpty())
            {
                text = snippet.value("superStickerDetails")
                           .toObject()
                           .value("superStickerMetadata")
                           .toObject()
                           .value("altText")
                           .toString();
            }
            text.prepend("[Super Sticker] ");
        }
        else if (type == "newSponsorEvent" ||
                 type == "membershipGiftingEvent" ||
                 type == "giftMembershipReceivedEvent")
        {
            if (text.isEmpty() && type == "newSponsorEvent")
            {
                const auto level = snippet.value("newSponsorDetails")
                                       .toObject()
                                       .value("memberLevelName")
                                       .toString();
                text = level.isEmpty() ? "New member" : "New member: " + level;
            }
            else if (text.isEmpty() && type == "membershipGiftingEvent")
            {
                const auto gifting =
                    snippet.value("membershipGiftingDetails").toObject();
                text = QString("Gifted %1 memberships")
                           .arg(gifting.value("giftMembershipsCount").toInt());
            }
            else if (text.isEmpty())
            {
                text = "Received a membership";
            }
            text.prepend("[Membership] ");
        }
        else if (type == "memberMilestoneChatEvent")
        {
            if (text.isEmpty())
            {
                const auto milestone =
                    snippet.value("memberMilestoneChatDetails").toObject();
                text = QString("%1 month membership")
                           .arg(milestone.value("memberMonth").toInt());
            }
            text.prepend("[Membership milestone] ");
        }
        else if (type == "pollEvent")
        {
            const auto poll = snippet.value("pollDetails")
                                  .toObject()
                                  .value("metadata")
                                  .toObject();
            text = "[Poll] " + poll.value("questionText").toString();
            QStringList options;
            for (const auto &optionValue : poll.value("options").toArray())
            {
                const auto option = optionValue.toObject();
                options.append(QString("%1 (%2)").arg(
                    option.value("optionText").toString(),
                    option.value("tally").toString()));
            }
            if (!options.isEmpty())
            {
                text += " — " + options.join(", ");
            }
        }
        if (text.isEmpty())
        {
            continue;
        }

        const auto authorId = author.value("channelId").toString();
        auto authorName = author.value("displayName").toString().trimmed();
        if (authorName.isEmpty())
        {
            authorName = authorId.isEmpty() ? "YouTube user" : authorId;
        }
        while (authorName.startsWith('@'))
        {
            authorName.remove(0, 1);
        }

        const auto milestone =
            snippet.value("memberMilestoneChatDetails").toObject();
        const auto memberMonth = milestone.value("memberMonth").toInt();
        auto memberLevel =
            milestone.value("memberLevelName").toString().trimmed();
        const auto newSponsor = snippet.value("newSponsorDetails").toObject();
        if (memberLevel.isEmpty())
        {
            memberLevel =
                newSponsor.value("memberLevelName").toString().trimmed();
        }
        const auto giftReceived =
            snippet.value("giftMembershipReceivedDetails").toObject();
        if (memberLevel.isEmpty())
        {
            memberLevel =
                giftReceived.value("memberLevelName").toString().trimmed();
        }

        const bool isOwner = author.value("isChatOwner").toBool();
        const bool isModerator = author.value("isChatModerator").toBool();
        const bool isMember = author.value("isChatSponsor").toBool() ||
                              type == "memberMilestoneChatEvent" ||
                              type == "newSponsorEvent" ||
                              type == "giftMembershipReceivedEvent";
        const bool isVerified = author.value("isVerified").toBool();

        MessageBuilder builder;
        builder->id = item.value("id").toString();
        builder->messageText = text;
        builder->searchText = authorName + " " + text;
        builder->loginName = authorName;
        builder->displayName = authorName;
        builder->userID = authorId;
        builder->channelName = channel->getName();
        auto publishedAt = QDateTime::fromString(
            snippet.value("publishedAt").toString(), Qt::ISODate);
        if (!publishedAt.isValid())
        {
            publishedAt = QDateTime::currentDateTime();
        }
        builder->timestamp = publishedAt;

        builder.emplace<TimestampElement>(publishedAt.toLocalTime().time());
        auto appendYouTubeBadge = [&builder](const QString &glyph,
                                             const QString &tooltip,
                                             MessageElementFlag badgeCategory) {
            const auto parsed =
                getApp()->getEmotes()->getEmojis()->parse(glyph);
            for (const auto &part : parsed)
            {
                const auto *emoji = std::get_if<EmotePtr>(&part);
                if (emoji == nullptr)
                {
                    continue;
                }

                auto badge = std::make_shared<Emote>(**emoji);
                badge->tooltip = Tooltip{tooltip};
                builder.emplace<BadgeElement>(std::move(badge), badgeCategory);
                return;
            }

            // Keep the badge visible if the local emoji set does not contain
            // the selected glyph.
            builder
                .emplace<TextElement>(glyph, badgeCategory,
                                      MessageColor::System,
                                      FontStyle::ChatMediumBold)
                ->setTooltip(tooltip);
        };
        MessageColor usernameColor = MessageColor::Text;
        if (isOwner)
        {
            usernameColor = QColor("#ff4b55");
        }
        else if (isModerator)
        {
            usernameColor = QColor("#27ae60");
        }
        else if (isVerified)
        {
            usernameColor = QColor("#3ea6ff");
        }
        else if (isMember)
        {
            usernameColor = QColor("#21a67a");
        }
        if (isOwner)
        {
            appendYouTubeBadge("👑", "YouTube broadcaster",
                               MessageElementFlag::BadgeChannelAuthority);
        }
        if (isModerator)
        {
            appendYouTubeBadge("🔧", "YouTube chat moderator",
                               MessageElementFlag::BadgeChannelAuthority);
        }
        if (isMember)
        {
            auto tooltip = QString("YouTube channel member");
            if (!memberLevel.isEmpty())
            {
                tooltip += " — " + memberLevel;
            }
            if (memberMonth > 0)
            {
                tooltip += QString(" — %1 months").arg(memberMonth);
            }
            appendYouTubeBadge("💠", tooltip,
                               MessageElementFlag::BadgeSubscription);
            if (!memberLevel.isEmpty())
            {
                builder
                    .emplace<TextElement>(
                        memberLevel, MessageElementFlag::BadgeSubscription,
                        MessageColor::System, FontStyle::ChatMediumBold)
                    ->setTooltip(tooltip);
            }
        }
        if (memberMonth > 0)
        {
            appendYouTubeBadge(
                QString("%1 mo").arg(memberMonth),
                QString("YouTube membership: %1 months").arg(memberMonth),
                MessageElementFlag::BadgeSubscription);
        }
        if (isVerified)
        {
            appendYouTubeBadge("✔️", "Verified YouTube account",
                               MessageElementFlag::BadgeVanity);
        }
        builder.emplace<TextElement>(authorName + ':',
                                     MessageElementFlag::Username,
                                     usernameColor, FontStyle::ChatMediumBold);
        builder.appendTextWithEmojis(text);
        channel->addMessage(builder.release(), MessageContext::Original);
    }
}

}  // namespace chatterino
