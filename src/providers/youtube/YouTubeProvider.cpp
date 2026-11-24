// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeProvider.hpp"

#include "Application.hpp"
#include "common/LinkParser.hpp"
#include "common/enums/MessageContext.hpp"
#include "controllers/filters/lang/expressions/Expression.hpp"
#include "controllers/highlights/HighlightCheck.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageColor.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageParseArgs.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "providers/youtube/YouTubeDiagnostics.hpp"
#include "providers/youtube/YouTubeInnertubeReader.hpp"
#include "singletons/Resources.hpp"
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
#include <vector>

namespace chatterino {

namespace {

constexpr qsizetype YOUTUBE_CUSTOM_EMOTE_CACHE_LIMIT = 512;
constexpr qsizetype YOUTUBE_REPLY_TARGET_CACHE_LIMIT = 2048;

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

QUrl livePageUrlForChannel(const QString &input)
{
    const auto lookup = parseChannelLookup(input);
    if (!lookup)
    {
        return {};
    }

    QString path;
    switch (lookup->kind)
    {
        case ChannelLookupKind::Id:
            path = "/channel/" + lookup->value + "/live";
            break;
        case ChannelLookupKind::Handle:
            path = "/" + lookup->value + "/live";
            break;
        case ChannelLookupKind::Username:
            path = "/user/" + lookup->value + "/live";
            break;
        case ChannelLookupKind::Name: {
            const auto url = QUrl::fromUserInput(input.trimmed());
            const auto host = url.host().toLower();
            if (host != "youtube.com" && host != "www.youtube.com" &&
                host != "m.youtube.com" && !host.endsWith(".youtube.com"))
            {
                return {};
            }
            const auto segments = url.path().split('/', Qt::SkipEmptyParts);
            if (segments.size() >= 2 && segments.at(0) == "c")
            {
                path = "/c/" + segments.at(1) + "/live";
            }
            break;
        }
    }

    if (path.isEmpty())
    {
        return {};
    }
    QUrl result("https://www.youtube.com");
    result.setPath(path);
    return result;
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

QColor colorFromYouTube(const QJsonValue &value)
{
    if (value.isString())
    {
        return QColor(value.toString());
    }
    if (value.isDouble())
    {
        return QColor::fromRgba(static_cast<QRgb>(value.toInteger()));
    }
    return {};
}

void appendYouTubeText(MessageBuilder &builder, QStringView text,
                       MessageColor color = MessageColor::Text)
{
    while (!text.isEmpty())
    {
        const auto parsedLink = linkparser::parse(text);
        if (!parsedLink)
        {
            builder.appendTextWithEmojis(text, color);
            return;
        }

        const auto prefix = parsedLink->prefix(text);
        if (!prefix.isEmpty())
        {
            builder.appendTextWithEmojis(prefix, color);
        }

        const auto linkText = parsedLink->link;
        if (linkText.isEmpty())
        {
            builder.appendTextWithEmojis(text, color);
            return;
        }

        builder.addLink(*parsedLink, linkText);
        const auto consumed = prefix.size() + linkText.size();
        if (consumed < text.size())
        {
            if (auto *link = dynamic_cast<LinkElement *>(
                    builder.message().elements.back().get()))
            {
                link->setTrailingSpace(false);
            }
        }
        text = text.mid(consumed);
    }
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

    if (this->isLoggedIn() &&
        getSettings()->youtubeAccountName.getValue().isEmpty() &&
        !getSettings()->youtubeAccountNameLookupAttempted.getValue())
    {
        this->loadAccountName();
    }
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

QString YouTubeProvider::accountName() const
{
    return getSettings()->youtubeAccountName.getValue();
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
    getSettings()->youtubeAccountName.setValue({});
    getSettings()->youtubeAccountNameLookupAttempted.setValue(false);
    ++this->accountNameRequestId_;
    this->accountNameLookupInProgress_ = false;
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
            getSettings()->youtubeAccountName.setValue({});
            getSettings()->youtubeAccountNameLookupAttempted.setValue(false);
            ++this->accountNameRequestId_;
            this->accountNameLookupInProgress_ = false;
            this->authChanged.invoke();
            this->loadAccountName();
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

void YouTubeProvider::loadAccountName()
{
    auto *settings = getSettings();
    if (!this->isLoggedIn() || this->accountNameLookupInProgress_ ||
        settings->youtubeAccountNameLookupAttempted.getValue())
    {
        return;
    }

    this->accountNameLookupInProgress_ = true;
    const auto requestId = this->accountNameRequestId_;
    this->ensureAccessToken([this, settings, requestId](bool success) {
        if (requestId != this->accountNameRequestId_)
        {
            return;
        }
        if (!success || !this->isLoggedIn())
        {
            this->accountNameLookupInProgress_ = false;
            return;
        }

        QUrlQuery query;
        query.addQueryItem("part", "snippet");
        query.addQueryItem("mine", "true");
        auto request = makeApiRequest("channels", query);
        std::move(request)
            .caller(this)
            .onSuccess([this, settings,
                        requestId](const NetworkResult &result) {
                if (requestId != this->accountNameRequestId_ ||
                    !this->isLoggedIn())
                {
                    return;
                }
                this->accountNameLookupInProgress_ = false;
                settings->youtubeAccountNameLookupAttempted.setValue(true);

                const auto items = result.parseJson().value("items").toArray();
                QString accountName;
                if (!items.isEmpty())
                {
                    accountName = items.at(0)
                                      .toObject()
                                      .value("snippet")
                                      .toObject()
                                      .value("title")
                                      .toString()
                                      .trimmed();
                }
                settings->youtubeAccountName.setValue(accountName);
                this->authChanged.invoke();
            })
            .onError([this, settings, requestId](const NetworkResult &result) {
                if (requestId != this->accountNameRequestId_ ||
                    !this->isLoggedIn())
                {
                    return;
                }
                this->accountNameLookupInProgress_ = false;
                settings->youtubeAccountNameLookupAttempted.setValue(true);
                qCWarning(chatterinoCommon)
                    << "Could not load the signed-in YouTube channel name:"
                    << result.formatError();
            })
            .execute();
    });
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
        const auto livePageUrl =
            livePageUrlForChannel(channelPtr->lookupInput());
        if (!livePageUrl.isEmpty())
        {
            this->startLiveStatusMonitoring(channelPtr.get(),
                                            livePageUrl.toString());
            return;
        }
        // Resolve display names through YouTube's public search page first.
        // Channel ID, handle, and legacy username inputs go straight to their
        // public /live page above and never need an account or API key.
        this->loadLiveVideoForChannel(channelPtr.get());
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
        [this, weakChannel](YouTubeInnertubeReader::VideoInfo info,
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
            if (!info.channelId.isEmpty())
            {
                channel->channelId_ = info.channelId;
                const auto statusUrl = livePageUrlForChannel(info.channelId);
                if (!statusUrl.isEmpty())
                {
                    this->startLiveStatusMonitoring(channel.get(),
                                                    statusUrl.toString());
                }
            }
            if (info.liveStateKnown)
            {
                channel->channelStatusKnown_ = true;
                channel->channelIsLive_ = info.isLive;
            }
            if (info.actualStartTime.isValid())
            {
                channel->actualStartTime_ = info.actualStartTime;
            }
            if (info.concurrentViewers >= 0)
            {
                channel->concurrentViewers_ = info.concurrentViewers;
            }
            channel->chatAvailable_ = chatAvailable;
            channel->displayNameChanged.invoke();
            if (chatAvailable)
            {
                channel->addSystemMessage(
                    "Connected to YouTube live chat using Innertube.");
            }
        },
        [this, weakChannel](QJsonObject root, bool initialBatch) {
            if (auto channel = weakChannel.lock())
            {
                this->processMessageBatch(channel.get(), root, initialBatch);
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
    if (lookup->kind != ChannelLookupKind::Name)
    {
        channelPtr->addSystemMessage(
            "Use a YouTube channel ID, @handle, or channel URL to follow live "
            "status without a channel-name search.");
        return;
    }

    QUrl url("https://www.youtube.com/results");
    QUrlQuery query;
    query.addQueryItem("search_query", lookup->value);
    query.addQueryItem("hl", "en");
    url.setQuery(query);
    auto request = NetworkRequest(url);
    std::move(request)
        .followRedirects(true)
        .header("Accept-Language", "en-US,en;q=0.9")
        .header("User-Agent",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/131.0.0.0 Safari/537.36")
        .timeout(20000)
        .caller(this)
        .onSuccess([this, channelPtr, lookup](const NetworkResult &result) {
            if (result.status() != 200)
            {
                this->loadLiveVideoForChannelWithApi(channelPtr.get());
                return;
            }

            const auto candidates =
                YouTubeInnertubeReader::parseChannelSearchPage(
                    result.getData());
            const auto requestedName = lookup->value.trimmed();
            auto handleName = requestedName;
            if (handleName.startsWith('@'))
            {
                handleName.remove(0, 1);
            }

            QList<YouTubeInnertubeReader::ChannelSearchResult> handleMatches;
            QList<YouTubeInnertubeReader::ChannelSearchResult> titleMatches;
            for (const auto &candidate : candidates)
            {
                if (candidate.channelId.isEmpty())
                {
                    continue;
                }
                const auto path = QUrl(candidate.url).path();
                const auto candidateHandle =
                    path.startsWith("/@") ? path.mid(2).section('/', 0, 0)
                                            : QString();
                if (!candidateHandle.isEmpty() &&
                    candidateHandle.compare(handleName,
                                            Qt::CaseInsensitive) == 0)
                {
                    handleMatches.append(candidate);
                }
                if (candidate.title.compare(requestedName,
                                            Qt::CaseInsensitive) == 0)
                {
                    titleMatches.append(candidate);
                }
            }

            const auto &matches =
                handleMatches.isEmpty() ? titleMatches : handleMatches;
            if (matches.size() > 1)
            {
                channelPtr->addSystemMessage(
                    "Several YouTube channels match this name. Use the "
                    "channel's @handle or URL.");
                return;
            }
            if (matches.isEmpty())
            {
                this->loadLiveVideoForChannelWithApi(channelPtr.get());
                return;
            }
            const auto &match = matches.first();
            channelPtr->channelId_ = match.channelId;
            if (!match.title.isEmpty())
            {
                channelPtr->channelTitle_ = match.title;
            }
            const auto liveUrl = livePageUrlForChannel(match.channelId);
            this->startLiveStatusMonitoring(channelPtr.get(),
                                            liveUrl.toString());
            channelPtr->displayNameChanged.invoke();
        })
        .onError([this, channelPtr](const NetworkResult &result) {
            logYouTubeDiagnostic(
                "channel_name_public_search_failed",
                QString("input=%1 error=%2")
                    .arg(channelPtr->lookupInput(), result.formatError()));
            this->loadLiveVideoForChannelWithApi(channelPtr.get());
        })
        .execute();
}

void YouTubeProvider::loadLiveVideoForChannelWithApi(YouTubeChannel *channel)
{
    auto channelPtr =
        std::dynamic_pointer_cast<YouTubeChannel>(channel->shared_from_this());
    if (!this->isLoggedIn() &&
        getSettings()->youtubeApiKey.getValue().isEmpty())
    {
        channelPtr->addSystemMessage(
            "Could not resolve that channel name from YouTube's public search. "
            "Try its @handle or channel URL. An API key or sign-in also "
            "enables the official channel-name search fallback.");
        return;
    }

    const auto lookup = parseChannelLookup(channelPtr->lookupInput());
    if (!lookup || lookup->kind != ChannelLookupKind::Name)
    {
        return;
    }

    this->ensureApiAccess(channelPtr.get(), [this, channelPtr, lookup] {
        QUrlQuery query;
        query.addQueryItem("part", "snippet");
        query.addQueryItem("type", "channel");
        query.addQueryItem("q", lookup->value);
        query.addQueryItem("maxResults", "5");
        auto request = makeApiRequest("search", query);
        std::move(request)
            .caller(this)
            .onSuccess([this, channelPtr, lookup](const NetworkResult &result) {
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
                    if (!id.isEmpty() &&
                        title.compare(lookup->value, Qt::CaseInsensitive) == 0)
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
                channelPtr->channelId_ = matchedChannelId;
                const auto liveUrl = livePageUrlForChannel(matchedChannelId);
                this->startLiveStatusMonitoring(channelPtr.get(),
                                                liveUrl.toString());
            })
            .onError([channelPtr](const NetworkResult &result) {
                addApiError(channelPtr.get(), result);
            })
            .execute();
    });
}

void YouTubeProvider::startLiveStatusMonitoring(
    YouTubeChannel *channel, const QString &liveStatusUrl)
{
    if (channel->liveStatusUrl_ == liveStatusUrl &&
        channel->liveStatusTimer_.isActive())
    {
        return;
    }

    channel->liveStatusUrl_ = liveStatusUrl;
    channel->liveStatusTimer_.setInterval(5 * 60 * 1000);
    channel->liveStatusTimer_.start();
    logYouTubeDiagnostic(
        "channel_status_monitor_start",
        QString("input=%1 interval_ms=%2")
            .arg(channel->lookupInput())
            .arg(channel->liveStatusTimer_.interval()));
    this->checkLiveStatus(channel);
}

void YouTubeProvider::checkLiveStatus(YouTubeChannel *channel)
{
    if (channel->liveStatusUrl_.isEmpty() ||
        channel->statusRequestInFlight_)
    {
        return;
    }

    channel->statusRequestInFlight_ = true;
    const auto requestId = ++channel->liveStatusRequestId_;
    auto channelPtr =
        std::dynamic_pointer_cast<YouTubeChannel>(channel->shared_from_this());
    const auto weakChannel = std::weak_ptr<YouTubeChannel>(channelPtr);
    QUrl url(channel->liveStatusUrl_);
    auto request = NetworkRequest(url);
    std::move(request)
        .followRedirects(true)
        .header("Accept-Language", "en-US,en;q=0.9")
        .header("User-Agent",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/131.0.0.0 Safari/537.36")
        .timeout(20000)
        .caller(this)
        .onSuccess([this, weakChannel, requestId](const NetworkResult &result) {
            auto channel = weakChannel.lock();
            if (!channel || channel->liveStatusRequestId_ != requestId)
            {
                return;
            }
            channel->statusRequestInFlight_ = false;
            if (result.status() != 200)
            {
                logYouTubeDiagnostic(
                    "channel_status_http_error",
                    QString("input=%1 status=%2")
                        .arg(channel->lookupInput())
                        .arg(result.status().value_or(-1)));
                return;
            }

            const auto info =
                YouTubeInnertubeReader::parseWatchPage(result.getData());
            if (!info.pageParsed || !info.liveStateKnown)
            {
                logYouTubeDiagnostic(
                    "channel_status_unknown",
                    QString("input=%1 parsed=%2 live_state=%3 bytes=%4")
                        .arg(channel->lookupInput())
                        .arg(info.pageParsed)
                        .arg(info.liveStateKnown)
                        .arg(result.getData().size()));
                return;
            }

            const bool wasKnown = channel->channelStatusKnown_;
            const bool wasLive = channel->channelIsLive_;
            const bool isLive = info.isLive && !info.videoId.isEmpty();
            const bool isNewStream =
                isLive && (channel->videoId_ != info.videoId || !wasLive);
            bool metadataChanged = !wasKnown;

            channel->channelStatusKnown_ = true;
            channel->channelIsLive_ = isLive;
            if (!info.channelId.isEmpty())
            {
                channel->channelId_ = info.channelId;
            }
            if (!info.channelTitle.isEmpty() &&
                channel->channelTitle_ != info.channelTitle)
            {
                channel->channelTitle_ = info.channelTitle;
                metadataChanged = true;
            }

            if (isLive)
            {
                if (!info.title.isEmpty() &&
                    channel->videoTitle_ != info.title)
                {
                    channel->videoTitle_ = info.title;
                    metadataChanged = true;
                }
                if (!info.thumbnailUrl.isEmpty() &&
                    channel->thumbnailUrl_ != info.thumbnailUrl)
                {
                    channel->thumbnailUrl_ = info.thumbnailUrl;
                    metadataChanged = true;
                }
                if (info.actualStartTime.isValid() &&
                    channel->actualStartTime_ != info.actualStartTime)
                {
                    channel->actualStartTime_ = info.actualStartTime;
                    metadataChanged = true;
                }
                if (info.concurrentViewers >= 0 &&
                    channel->concurrentViewers_ != info.concurrentViewers)
                {
                    channel->concurrentViewers_ = info.concurrentViewers;
                    metadataChanged = true;
                }

                if (isNewStream)
                {
                    channel->videoId_ = info.videoId;
                    channel->liveChatId_.clear();
                    channel->chatAvailable_ = false;
                    if (channel->reader_)
                    {
                        auto reader = std::move(channel->reader_);
                        reader->stop();
                    }
                    this->startInnertubeReader(channel.get());
                    const auto title = channel->videoTitle_.isEmpty()
                                           ? QString("YouTube channel is live")
                                           : QString("Now live: %1")
                                                 .arg(channel->videoTitle_);
                    channel->addSystemMessage(title);
                    channel->offlineNoticeSent_ = false;
                    if (this->isLoggedIn())
                    {
                        const auto weakChannel =
                            std::weak_ptr<YouTubeChannel>(channel);
                        this->ensureApiAccess(channel.get(), [this, weakChannel] {
                            if (auto liveChannel = weakChannel.lock();
                                liveChannel && liveChannel->channelIsLive_)
                            {
                                this->loadVideoDetails(liveChannel.get());
                            }
                        });
                    }
                }
            }
            else
            {
                if (wasLive)
                {
                    if (channel->reader_)
                    {
                        auto reader = std::move(channel->reader_);
                        reader->stop();
                    }
                    channel->liveChatId_.clear();
                    channel->chatAvailable_ = false;
                    channel->videoTitle_.clear();
                    channel->thumbnailUrl_.clear();
                    channel->actualStartTime_ = {};
                    channel->concurrentViewers_ = -1;
                    if (channel->channelLookup_)
                    {
                        channel->videoId_.clear();
                    }
                    channel->addSystemMessage(
                        "The YouTube stream ended. Waiting for the next live "
                        "stream.");
                    channel->offlineNoticeSent_ = true;
                    metadataChanged = true;
                }
                else if (!channel->offlineNoticeSent_)
                {
                    channel->addSystemMessage(
                        "YouTube channel is offline. Checking every 5 minutes.");
                    channel->offlineNoticeSent_ = true;
                }
            }

            if (metadataChanged || isNewStream || wasLive != isLive)
            {
                channel->displayNameChanged.invoke();
            }
            logYouTubeDiagnostic(
                "channel_status_result",
                QString("input=%1 state=%2 video_id=%3 viewers=%4")
                    .arg(channel->lookupInput(),
                         isLive ? QStringLiteral("live")
                                : QStringLiteral("offline"),
                         isLive ? channel->videoId_ : QString())
                    .arg(channel->concurrentViewers_));
        })
        .onError([weakChannel, requestId](const NetworkResult &result) {
            if (auto channel = weakChannel.lock();
                channel && channel->liveStatusRequestId_ == requestId)
            {
                channel->statusRequestInFlight_ = false;
                logYouTubeDiagnostic(
                    "channel_status_request_failed",
                    QString("input=%1 error=%2")
                        .arg(channel->lookupInput(), result.formatError()));
            }
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
                                          const QJsonObject &root,
                                          bool initialBatch)
{
    const auto events = root.value("events").toArray();
    auto cacheCustomEmote = [this](const QString &key,
                                   const std::shared_ptr<const Emote> &emote) {
        this->customEmotes_.insert(key, emote);
        this->customEmoteCacheOrder_.enqueue(key);
        while (this->customEmoteCacheOrder_.size() >
               YOUTUBE_CUSTOM_EMOTE_CACHE_LIMIT)
        {
            this->customEmotes_.remove(
                this->customEmoteCacheOrder_.dequeue());
        }
    };
    auto getCustomEmoji = [this, &cacheCustomEmote](
                              const QJsonObject &run) -> EmotePtr {
        struct Thumbnail {
            QString url;
            QSize size;
        };

        const auto emojiId = run.value("emojiId").toString();
        const auto fallbackName = run.value("text").toString();
        const auto label = run.value("label").toString();
        const bool isGift = run.value("isGift").toBool();
        std::vector<Thumbnail> thumbnails;
        auto thumbnailValues = run.value("thumbnails").toArray();
        const auto giftUrl = run.value("giftUrl").toString();
        if (thumbnailValues.isEmpty() && !giftUrl.isEmpty())
        {
            thumbnailValues.append(QJsonObject{{"url", giftUrl},
                                               {"width", 128},
                                               {"height", 128}});
        }
        for (const auto &thumbnailValue : thumbnailValues)
        {
            const auto thumbnail = thumbnailValue.toObject();
            auto url = thumbnail.value("url").toString();
            if (url.startsWith("//"))
            {
                url.prepend("https:");
            }

            const QUrl parsedUrl(url);
            const auto host = parsedUrl.host().toLower();
            const bool youtubeImageHost =
                host == "ggpht.com" || host.endsWith(".ggpht.com") ||
                host == "googleusercontent.com" ||
                host.endsWith(".googleusercontent.com") ||
                host == "gstatic.com" || host.endsWith(".gstatic.com") ||
                host == "ytimg.com" || host.endsWith(".ytimg.com") ||
                host == "youtube.com" || host.endsWith(".youtube.com");
            if (parsedUrl.scheme() != "https" || !youtubeImageHost)
            {
                continue;
            }

            auto width = thumbnail.value("width").toInt();
            auto height = thumbnail.value("height").toInt();
            if (width <= 0 || height <= 0)
            {
                width = 64;
                height = 64;
            }
            thumbnails.push_back({std::move(url), QSize(width, height)});
        }
        if (thumbnails.empty())
        {
            return {};
        }

        std::sort(thumbnails.begin(), thumbnails.end(),
                  [](const auto &a, const auto &b) {
                      return a.size.width() * a.size.height() <
                             b.size.width() * b.size.height();
                  });

        const auto cacheKey =
            QString(isGift ? "youtube-gift|" : "youtube-emoji|") + emojiId +
            "|" + thumbnails.back().url;
        if (const auto cached = this->customEmotes_.constFind(cacheKey);
            cached != this->customEmotes_.cend())
        {
            return cached.value();
        }

        std::vector<Thumbnail> selectedThumbnails;
        if (thumbnails.size() <= 3)
        {
            selectedThumbnails = std::move(thumbnails);
        }
        else
        {
            selectedThumbnails = {thumbnails.front(),
                                  thumbnails[thumbnails.size() / 2],
                                  thumbnails.back()};
        }

        auto makeImage = [](const Thumbnail &thumbnail) {
            const auto largestDimension =
                std::max(thumbnail.size.width(), thumbnail.size.height());
            const qreal scale = 24.0 / largestDimension;
            return Image::fromUrl({thumbnail.url}, scale, thumbnail.size);
        };
        auto image1 = Image::getEmpty();
        auto image2 = Image::getEmpty();
        auto image3 = Image::getEmpty();
        if (!selectedThumbnails.empty())
        {
            image1 = makeImage(selectedThumbnails[0]);
        }
        if (selectedThumbnails.size() >= 2)
        {
            image2 = makeImage(selectedThumbnails[1]);
        }
        if (selectedThumbnails.size() >= 3)
        {
            image3 = makeImage(selectedThumbnails[2]);
        }

        const auto emoteName = emojiId.isEmpty() ? fallbackName : emojiId;
        const auto tooltipText = label.isEmpty() ? emoteName : label;
        auto emote = std::make_shared<Emote>(Emote{
            .name = EmoteName{emoteName},
            .images = ImageSet{image1, image2, image3},
            .tooltip = Tooltip{
                QString(isGift ? "YouTube gift<br>%1" : "YouTube emote<br>%1")
                    .arg(tooltipText.toHtmlEscaped())},
            .homePage = Url{"https://www.youtube.com/"},
            .zeroWidth = false,
            .id = EmoteId{emojiId.isEmpty() ? cacheKey : emojiId},
            .author = {},
        });
        cacheCustomEmote(cacheKey, emote);
        return emote;
    };
    auto getYouTubeBadge = [this, &cacheCustomEmote](
                               const QJsonObject &badge) -> EmotePtr {
        struct Thumbnail {
            QString url;
            QSize size;
        };

        std::vector<Thumbnail> thumbnails;
        for (const auto &thumbnailValue : badge.value("thumbnails").toArray())
        {
            const auto thumbnail = thumbnailValue.toObject();
            auto url = thumbnail.value("url").toString();
            if (url.startsWith("//"))
            {
                url.prepend("https:");
            }
            const QUrl parsedUrl(url);
            const auto host = parsedUrl.host().toLower();
            const bool youtubeImageHost =
                host == "ggpht.com" || host.endsWith(".ggpht.com") ||
                host == "googleusercontent.com" ||
                host.endsWith(".googleusercontent.com");
            if (parsedUrl.scheme() != "https" || !youtubeImageHost)
            {
                continue;
            }

            auto width = thumbnail.value("width").toInt();
            auto height = thumbnail.value("height").toInt();
            if (width <= 0 || height <= 0)
            {
                width = 48;
                height = 48;
            }
            thumbnails.push_back({std::move(url), QSize(width, height)});
        }
        if (thumbnails.empty())
        {
            return {};
        }

        std::sort(thumbnails.begin(), thumbnails.end(),
                  [](const auto &a, const auto &b) {
                      return a.size.width() * a.size.height() <
                             b.size.width() * b.size.height();
                  });
        auto tooltip = badge.value("tooltip").toString().trimmed();
        if (tooltip.isEmpty())
        {
            tooltip = "YouTube chat badge";
        }
        const auto cacheKey = "youtube-badge|" + tooltip + "|" +
                              thumbnails.back().url;
        if (const auto cached = this->customEmotes_.constFind(cacheKey);
            cached != this->customEmotes_.cend())
        {
            return cached.value();
        }

        std::vector<Thumbnail> selectedThumbnails;
        if (thumbnails.size() <= 3)
        {
            selectedThumbnails = std::move(thumbnails);
        }
        else
        {
            selectedThumbnails = {thumbnails.front(),
                                  thumbnails[thumbnails.size() / 2],
                                  thumbnails.back()};
        }

        auto makeImage = [](const Thumbnail &thumbnail) {
            const auto largestDimension =
                std::max(thumbnail.size.width(), thumbnail.size.height());
            const qreal scale = 18.0 / largestDimension;
            return Image::fromUrl({thumbnail.url}, scale, thumbnail.size);
        };
        auto image1 = Image::getEmpty();
        auto image2 = Image::getEmpty();
        auto image3 = Image::getEmpty();
        if (!selectedThumbnails.empty())
        {
            image1 = makeImage(selectedThumbnails[0]);
        }
        if (selectedThumbnails.size() >= 2)
        {
            image2 = makeImage(selectedThumbnails[1]);
        }
        if (selectedThumbnails.size() >= 3)
        {
            image3 = makeImage(selectedThumbnails[2]);
        }

        auto emote = std::make_shared<Emote>(Emote{
            .name = EmoteName{tooltip},
            .images = ImageSet{image1, image2, image3},
            .tooltip = Tooltip{tooltip.toHtmlEscaped()},
            .homePage = Url{"https://www.youtube.com/"},
            .zeroWidth = false,
            .id = EmoteId{cacheKey},
            .author = {},
        });
        cacheCustomEmote(cacheKey, emote);
        return emote;
    };

    for (const auto &eventValue : events)
    {
        const auto event = eventValue.toObject();
        const auto action = event.value("action").toString();
        const auto targetItemId = event.value("targetItemId").toString();
        if (action == "delete")
        {
            const auto deleted = channel->findMessageByID(targetItemId);
            if (deleted)
            {
                deleted->flags.set(MessageFlag::Disabled);
                deleted->flags.set(MessageFlag::InvalidReplyTarget);
            }
            const auto cachedTarget =
                channel->replyTargets_.value(targetItemId);
            if (cachedTarget.messageId.isEmpty())
            {
                channel->replyTargets_.remove(targetItemId);
            }
            else
            {
                for (auto it = channel->replyTargets_.begin();
                     it != channel->replyTargets_.end();)
                {
                    if (it.key() == targetItemId ||
                        it->messageId == cachedTarget.messageId)
                    {
                        it = channel->replyTargets_.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            continue;
        }
        if (action == "deleteByAuthor")
        {
            const auto authorChannelId =
                event.value("authorChannelId").toString();
            if (authorChannelId.isEmpty())
            {
                continue;
            }
            for (const auto &message : channel->getMessageSnapshot())
            {
                if (message->userID == authorChannelId)
                {
                    message->flags.set(MessageFlag::Disabled);
                    message->flags.set(MessageFlag::InvalidReplyTarget);
                }
            }
            for (auto it = channel->replyTargets_.begin();
                 it != channel->replyTargets_.end();)
            {
                if (it->authorId == authorChannelId)
                {
                    it = channel->replyTargets_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            continue;
        }
        if (action == "clear")
        {
            channel->disableAllMessages();
            continue;
        }

        const auto item = event.value("item").toObject();
        if (item.isEmpty())
        {
            continue;
        }
        const auto snippet = item.value("snippet").toObject();
        const auto author = item.value("authorDetails").toObject();
        const auto type = snippet.value("type").toString();
        if (initialBatch && type == "giftEvent")
        {
            // Initial chat data includes recent history. Do not replay old
            // gift animations when a YouTube chat is first opened.
            continue;
        }
        const auto superChatDetails =
            snippet.value("superChatDetails").toObject();
        const auto giftDetails = snippet.value("giftDetails").toObject();
        QString text = snippet.value("displayMessage").toString();
        if (type == "textMessageEvent")
        {
            text = snippet.value("textMessageDetails")
                       .toObject()
                       .value("messageText")
                       .toString();
        }
        if (type == "giftEvent")
        {
            const auto giftName = giftDetails.value("giftName").toString();
            static const QRegularExpression genericGiftText(
                R"(^\s*sent\s+(?:a\s+)?gift\s*$)",
                QRegularExpression::CaseInsensitiveOption);
            const auto namedGiftText = "sent " + giftName;
            if (text.isEmpty() || genericGiftText.match(text).hasMatch() ||
                (!giftName.isEmpty() &&
                 text.trimmed().compare(namedGiftText,
                                        Qt::CaseInsensitive) == 0))
            {
                text = "sent a gift";
            }
            if (!giftName.isEmpty())
            {
                const auto giftSuffix =
                    QStringLiteral(" (") + giftName + QStringLiteral(")");
                if (!text.trimmed().endsWith(giftSuffix,
                                             Qt::CaseInsensitive))
                {
                    text = text.trimmed() + giftSuffix;
                }
            }
        }
        if (type == "superChatEvent")
        {
            const auto amount =
                superChatDetails.value("amountDisplayString").toString();
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
        else if (type == "chatEndedEvent")
        {
            text = "YouTube live chat ended.";
        }
        if (text.isEmpty())
        {
            continue;
        }

        const auto authorId = author.value("channelId").toString();
        auto authorName = author.value("displayName").toString().trimmed();
        if (authorName.isEmpty())
        {
            authorName = type == "chatEndedEvent" ||
                                 type == "chatModeChangeEvent" ||
                                 type == "sponsorOnlyModeStartedEvent" ||
                                 type == "sponsorOnlyModeEndedEvent" ||
                                 type == "viewerEngagementMessageEvent" ||
                                 type == "tombstone" || type == "raidEvent"
                             ? "YouTube"
                             : (authorId.isEmpty() ? "YouTube user" : authorId);
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
        if (type == "pollEvent")
        {
            builder->id = "youtube-active-poll";
        }
        else if (action == "banner" && !targetItemId.isEmpty())
        {
            builder->id = type == "raidEvent"
                              ? QStringLiteral("youtube-raid-") + targetItemId
                              : targetItemId;
        }
        else if (type == "chatEndedEvent")
        {
            builder->id = "youtube-chat-ended";
        }
        else if (action == "replace" && !targetItemId.isEmpty())
        {
            builder->id = targetItemId;
        }
        builder->messageText = text;
        builder->searchText = authorName + " " + text;
        builder->loginName = authorName;
        builder->displayName = authorName;
        builder->userID = authorId;
        builder->channelName = channel->getName();
        if (type == "chatEndedEvent" || type == "chatModeChangeEvent" ||
            type == "sponsorOnlyModeStartedEvent" ||
            type == "sponsorOnlyModeEndedEvent" ||
            type == "viewerEngagementMessageEvent" || type == "tombstone")
        {
            builder->flags.set(MessageFlag::System);
        }
        if (type == "superChatEvent" || type == "superStickerEvent")
        {
            const auto stickerDetails =
                snippet.value("superStickerDetails")
                    .toObject()
                    .value("superStickerMetadata")
                    .toObject();
            auto backgroundColor =
                type == "superChatEvent"
                    ? colorFromYouTube(
                          superChatDetails.value("bodyBackgroundColor"))
                    : colorFromYouTube(
                          stickerDetails.value("backgroundColor"));
            if (!backgroundColor.isValid() || backgroundColor.alpha() == 0)
            {
                backgroundColor = QColor("#00E5FF");
            }
            builder->flags.set(MessageFlag::Highlighted);
            builder->highlightColor = std::make_shared<QColor>(backgroundColor);
        }
        else if (type == "giftEvent" || type == "newSponsorEvent" ||
                 type == "membershipGiftingEvent" ||
                 type == "giftMembershipReceivedEvent" ||
                 type == "memberMilestoneChatEvent")
        {
            // YouTube highlights membership and gift announcements.
            builder->flags.set(MessageFlag::Highlighted);
            builder->highlightColor =
                std::make_shared<QColor>(QColor("#13A567"));
        }
        else if (type == "raidEvent")
        {
            builder->flags.set(MessageFlag::System);
            builder->flags.set(MessageFlag::Highlighted);
            builder->highlightColor =
                std::make_shared<QColor>(QColor("#7446B8"));
        }
        auto publishedAt = QDateTime::fromString(
            snippet.value("publishedAt").toString(), Qt::ISODate);
        if (!publishedAt.isValid())
        {
            publishedAt = QDateTime::currentDateTime();
        }
        builder->timestamp = publishedAt;

        if (type == "superChatEvent")
        {
            YouTubeChannel::ReplyTarget target;
            target.messageId = item.value("id").toString();
            if (target.messageId.isEmpty())
            {
                target.messageId = builder->id;
            }
            target.authorId = authorId;
            target.authorName = authorName;
            target.messageText = builder->messageText;
            target.timestamp = publishedAt;

            if (!target.messageId.isEmpty())
            {
                channel->replyTargets_.insert(target.messageId, target);
            }
            if (!builder->id.isEmpty())
            {
                channel->replyTargets_.insert(builder->id, target);
            }
            if (!targetItemId.isEmpty())
            {
                channel->replyTargets_.insert(targetItemId, target);
            }

            // Keep recent Super Chat reply context without retaining every
            // paid message for the lifetime of the channel.
            while (channel->replyTargets_.size() >
                   YOUTUBE_REPLY_TARGET_CACHE_LIMIT)
            {
                auto oldest = channel->replyTargets_.begin();
                for (auto it = channel->replyTargets_.begin();
                     it != channel->replyTargets_.end(); ++it)
                {
                    if (it->timestamp < oldest->timestamp)
                    {
                        oldest = it;
                    }
                }
                channel->replyTargets_.erase(oldest);
            }
        }

        const auto replyTargetId =
            snippet.value("replyTargetId").toString();
        auto replyParent =
            replyTargetId.isEmpty()
                ? MessagePtr{}
                : channel->findMessageByID(replyTargetId);
        auto replyTargetName = snippet.value("replyTargetName").toString();
        auto replyTargetMessage =
            snippet.value("replyTargetMessage").toString();
        const auto replyTargetAmount =
            snippet.value("replyTargetAmount").toString().trimmed();
        const auto replyTargetUserId =
            snippet.value("replyTargetUserId").toString();
        auto replyTargetIsSuperChat =
            snippet.value("replyTargetIsSuperChat").toBool();
        if (!replyParent && !replyTargetId.isEmpty())
        {
            const auto cachedTarget =
                channel->replyTargets_.constFind(replyTargetId);
            if (cachedTarget != channel->replyTargets_.cend())
            {
                replyTargetName = cachedTarget->authorName;
                replyTargetMessage = cachedTarget->messageText;
                replyTargetIsSuperChat = true;
            }
        }
        static const QRegularExpression timestampOnlyPattern(
            R"(^\d{1,2}:\d{2}(?::\d{2})?$)");
        const auto trimmedReplyTargetMessage = replyTargetMessage.trimmed();
        const bool missingReplyTargetMessage =
            trimmedReplyTargetMessage.isEmpty() ||
            timestampOnlyPattern.match(trimmedReplyTargetMessage).hasMatch() ||
            trimmedReplyTargetMessage.compare("[Super Chat]",
                                              Qt::CaseInsensitive) == 0 ||
            (replyTargetIsSuperChat && !replyTargetAmount.isEmpty() &&
             trimmedReplyTargetMessage.compare(replyTargetAmount,
                                               Qt::CaseInsensitive) == 0);
        if (!replyParent && replyTargetIsSuperChat &&
            missingReplyTargetMessage)
        {
            auto lookupName = replyTargetName;
            while (lookupName.startsWith('@'))
            {
                lookupName.remove(0, 1);
            }

            const YouTubeChannel::ReplyTarget *cachedParent = nullptr;
            for (auto it = channel->replyTargets_.cbegin();
                 it != channel->replyTargets_.cend(); ++it)
            {
                if (it->timestamp > publishedAt)
                {
                    continue;
                }
                const bool matchesUserId =
                    !replyTargetUserId.isEmpty() &&
                    it->authorId == replyTargetUserId;
                const bool matchesName =
                    !lookupName.isEmpty() &&
                    it->authorName.compare(lookupName,
                                           Qt::CaseInsensitive) == 0;
                if (!matchesUserId && !matchesName)
                {
                    continue;
                }
                if (cachedParent == nullptr ||
                    it->timestamp > cachedParent->timestamp)
                {
                    cachedParent = &it.value();
                }
            }
            if (cachedParent != nullptr)
            {
                replyTargetName = cachedParent->authorName;
                replyTargetMessage = cachedParent->messageText;
            }
        }
        if (!replyParent &&
            (!replyTargetName.isEmpty() || !replyTargetUserId.isEmpty()) &&
            missingReplyTargetMessage)
        {
            auto lookupName = replyTargetName;
            while (lookupName.startsWith('@'))
            {
                lookupName.remove(0, 1);
            }

            for (const auto &candidate : channel->getMessageSnapshot())
            {
                if (!candidate->timestamp.isValid() ||
                    candidate->timestamp > publishedAt)
                {
                    continue;
                }
                const auto ageSeconds =
                    candidate->timestamp.secsTo(publishedAt);
                if (ageSeconds < 0 ||
                    (!replyTargetIsSuperChat && ageSeconds > 300))
                {
                    continue;
                }
                const bool matchesUserId =
                    !replyTargetUserId.isEmpty() &&
                    candidate->userID == replyTargetUserId;
                const bool matchesName =
                    candidate->displayName.compare(
                        lookupName, Qt::CaseInsensitive) == 0 ||
                    candidate->loginName.compare(
                        lookupName, Qt::CaseInsensitive) == 0;
                if (!matchesUserId && !matchesName)
                {
                    continue;
                }
                if (replyTargetIsSuperChat &&
                    !candidate->messageText.startsWith(
                        "[Super Chat", Qt::CaseInsensitive))
                {
                    continue;
                }
                if (!replyParent ||
                    candidate->timestamp > replyParent->timestamp)
                {
                    replyParent = candidate;
                }
            }
        }
        if (replyParent)
        {
            if (!replyParent->displayName.isEmpty())
            {
                replyTargetName = replyParent->displayName;
            }
            else if (!replyParent->loginName.isEmpty())
            {
                replyTargetName = replyParent->loginName;
            }
            replyTargetIsSuperChat =
                replyTargetIsSuperChat ||
                replyParent->messageText.startsWith(
                    "[Super Chat", Qt::CaseInsensitive);
            if (!replyParent->messageText.isEmpty() &&
                !timestampOnlyPattern
                     .match(replyParent->messageText.trimmed())
                     .hasMatch())
            {
                replyTargetMessage = replyParent->messageText;
            }
            builder->replyParent = replyParent;
        }
        if (timestampOnlyPattern.match(replyTargetMessage.trimmed()).hasMatch())
        {
            replyTargetMessage.clear();
        }
        if (replyTargetIsSuperChat)
        {
            static const QRegularExpression superChatPrefixPattern(
                R"(^\s*\[Super Chat(?:\s+([^\]]+))?\]\s*)",
                QRegularExpression::CaseInsensitiveOption);
            const auto prefixMatch =
                superChatPrefixPattern.match(replyTargetMessage);
            if (prefixMatch.hasMatch())
            {
                const auto amount = prefixMatch.captured(1).trimmed();
                replyTargetMessage.remove(0, prefixMatch.capturedLength());
                if (!amount.isEmpty() &&
                    !replyTargetMessage.trimmed().startsWith(
                        amount, Qt::CaseInsensitive))
                {
                    auto originalText = replyTargetMessage.trimmed();
                    if (!originalText.isEmpty())
                    {
                        originalText.prepend(' ');
                    }
                    replyTargetMessage = amount + originalText;
                }
            }
        }
        while (replyTargetName.startsWith('@'))
        {
            replyTargetName.remove(0, 1);
        }
        if (!replyTargetName.isEmpty() || !replyTargetMessage.isEmpty() ||
            replyTargetIsSuperChat)
        {
            builder->flags.set(MessageFlag::ReplyMessage);
            builder.emplace<ReplyCurveElement>();
            builder.emplace<TextElement>(
                "Replying to", MessageElementFlag::RepliedMessage,
                MessageColor::System, FontStyle::ChatMediumSmall);

            auto targetColor = MessageColor{MessageColor::Text};
            if (replyParent && replyParent->usernameColor.isValid())
            {
                targetColor = MessageColor{replyParent->usernameColor};
            }
            else
            {
                const auto parsedTargetColor =
                    colorFromYouTube(snippet.value("replyTargetNameColor"));
                if (parsedTargetColor.isValid() &&
                    parsedTargetColor.alpha() > 0)
                {
                    targetColor = MessageColor{parsedTargetColor};
                }
            }

            if (!replyTargetName.isEmpty())
            {
                builder
                    .emplace<TextElement>(
                        "@" + replyTargetName + ':',
                        MessageElementFlag::RepliedMessage, targetColor,
                        FontStyle::ChatMediumSmall)
                    ->setLink({Link::UserInfo, replyTargetName});
            }
            if (!replyTargetMessage.isEmpty())
            {
                builder.emplace<TextElement>(
                    replyTargetMessage,
                    MessageElementFlags({MessageElementFlag::RepliedMessage,
                                         MessageElementFlag::Text}),
                    MessageColor::Text, FontStyle::ChatMediumSmall);
            }
            if (replyTargetIsSuperChat)
            {
                auto &replyIcon = getResources().buttons.replyDark;
                builder.emplace<CircularImageElement>(
                    Image::fromResourcePixmap(replyIcon, 0.15), 2, Qt::gray,
                    MessageElementFlag::RepliedMessage);
                builder.emplace<TextElement>(
                    "SUPERCHAT:", MessageElementFlag::RepliedMessage,
                    MessageColor{QColor("#ff4b55")},
                    FontStyle::ChatMediumBold);
            }
            builder.emplace<LinebreakElement>(
                MessageElementFlag::RepliedMessage);
        }

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
        QColor storedUsernameColor;
        if (isOwner)
        {
            usernameColor = QColor("#ff4b55");
            storedUsernameColor = QColor("#ff4b55");
        }
        else if (isModerator)
        {
            usernameColor = QColor("#27ae60");
            storedUsernameColor = QColor("#27ae60");
        }
        else if (isVerified)
        {
            usernameColor = QColor("#3ea6ff");
            storedUsernameColor = QColor("#3ea6ff");
        }
        else if (isMember)
        {
            usernameColor = QColor("#21a67a");
            storedUsernameColor = QColor("#21a67a");
        }
        if (type == "superChatEvent")
        {
            const auto paidUsernameColor =
                colorFromYouTube(superChatDetails.value("authorNameTextColor"));
            if (paidUsernameColor.isValid() && paidUsernameColor.alpha() > 0)
            {
                usernameColor = paidUsernameColor;
                storedUsernameColor = paidUsernameColor;
            }
        }
        else if (type == "newSponsorEvent" ||
                 type == "membershipGiftingEvent" ||
                 type == "giftMembershipReceivedEvent")
        {
            usernameColor = QColor("#ffffff");
            storedUsernameColor = QColor("#ffffff");
        }
        else
        {
            const auto nativeNameColor =
                colorFromYouTube(author.value("nameTextColor"));
            if (nativeNameColor.isValid() && nativeNameColor.alpha() > 0)
            {
                usernameColor = nativeNameColor;
                storedUsernameColor = nativeNameColor;
            }
        }
        builder->usernameColor = storedUsernameColor;
        bool hasOwnerBadgeImage = false;
        bool hasModeratorBadgeImage = false;
        bool hasMemberBadgeImage = false;
        bool hasVerifiedBadgeImage = false;
        for (const auto &badgeValue : author.value("badges").toArray())
        {
            const auto badge = badgeValue.toObject();
            const auto iconType = badge.value("iconType").toString();
            const auto tooltip = badge.value("tooltip").toString().toLower();
            const bool isBadgeOwner = iconType == "OWNER" ||
                                      tooltip.contains("owner") ||
                                      tooltip.contains("broadcaster");
            const bool isBadgeModerator = iconType == "MODERATOR" ||
                                          tooltip.contains("moderator");
            const bool isBadgeMember = badge.value("isMember").toBool();
            const bool isBadgeVerified = iconType == "VERIFIED" ||
                                         tooltip.contains("verified");
            const auto badgeCategory =
                isBadgeOwner || isBadgeModerator
                    ? MessageElementFlag::BadgeChannelAuthority
                    : (isBadgeMember ? MessageElementFlag::BadgeSubscription
                                     : MessageElementFlag::BadgeVanity);
            if (const auto badgeImage = getYouTubeBadge(badge))
            {
                builder.emplace<BadgeElement>(badgeImage, badgeCategory);
                hasOwnerBadgeImage = hasOwnerBadgeImage || isBadgeOwner;
                hasModeratorBadgeImage =
                    hasModeratorBadgeImage || isBadgeModerator;
                hasMemberBadgeImage = hasMemberBadgeImage || isBadgeMember;
                hasVerifiedBadgeImage =
                    hasVerifiedBadgeImage || isBadgeVerified;
            }
        }
        if (isOwner && !hasOwnerBadgeImage)
        {
            appendYouTubeBadge("👑", "YouTube broadcaster",
                               MessageElementFlag::BadgeChannelAuthority);
        }
        if (isModerator && !hasModeratorBadgeImage)
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
            if (!hasMemberBadgeImage)
            {
                appendYouTubeBadge("💠", tooltip,
                                   MessageElementFlag::BadgeSubscription);
            }
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
        if (isVerified && !hasVerifiedBadgeImage)
        {
            appendYouTubeBadge("✔️", "Verified YouTube account",
                               MessageElementFlag::BadgeVanity);
        }
        auto *usernameElement = builder.emplace<TextElement>(
            authorName + ':', MessageElementFlag::Username, usernameColor,
            FontStyle::ChatMediumBold);
        if (!builder->flags.has(MessageFlag::System))
        {
            usernameElement->setLink({Link::UserInfo, authorName});
        }
        const MessageColor bodyColor = MessageColor::Text;
        const auto messageRuns = snippet.value("messageRuns").toArray();
        if (type == "giftEvent")
        {
            const auto giftName = giftDetails.value("giftName").toString();
            const QJsonObject giftRun{
                {"emojiId", giftName},
                {"text", giftName},
                {"label", giftName.isEmpty() ? QStringLiteral("YouTube gift")
                                              : giftName},
                {"thumbnails", giftDetails.value("thumbnails")},
                {"giftUrl", giftDetails.value("giftUrl")},
                {"isGift", true},
            };
            const auto giftImage = getCustomEmoji(giftRun);
            appendYouTubeText(builder,
                              text + (giftImage ? QStringLiteral(" ")
                                                : QString{}),
                              bodyColor);
            if (giftImage)
            {
                builder.emplace<EmoteElement>(giftImage,
                                              MessageElementFlag::EmojiAll);
            }
        }
        else if (type == "superStickerEvent")
        {
            const auto stickerMetadata =
                snippet.value("superStickerDetails")
                    .toObject()
                    .value("superStickerMetadata")
                    .toObject();
            const auto amount = snippet.value("superStickerDetails")
                                    .toObject()
                                    .value("amountDisplayString")
                                    .toString();
            if (!amount.isEmpty())
            {
                appendYouTubeText(builder, amount + ' ', bodyColor);
            }
            const QJsonObject stickerRun{
                {"emojiId", stickerMetadata.value("stickerId")},
                {"text", stickerMetadata.value("altText")},
                {"label", stickerMetadata.value("altText")},
                {"thumbnails", stickerMetadata.value("thumbnails")},
            };
            if (const auto sticker = getCustomEmoji(stickerRun))
            {
                builder.emplace<EmoteElement>(sticker,
                                              MessageElementFlag::EmojiAll);
            }
            else
            {
                appendYouTubeText(builder,
                                  stickerMetadata.value("altText").toString(),
                                  bodyColor);
            }
        }
        else if (messageRuns.isEmpty())
        {
            appendYouTubeText(builder, text, bodyColor);
        }
        else
        {
            if (type == "superChatEvent")
            {
                const auto amount =
                    superChatDetails.value("amountDisplayString").toString();
                appendYouTubeText(
                    builder,
                    amount.isEmpty() ? QStringLiteral("[Super Chat] ")
                                     : amount + ' ',
                    bodyColor);
            }
            for (const auto &runValue : messageRuns)
            {
                const auto run = runValue.toObject();
                const auto runText = run.value("text").toString();
                if (run.value("isCustomEmoji").toBool())
                {
                    const auto emoji = getCustomEmoji(run);
                    if (emoji)
                    {
                        builder.emplace<EmoteElement>(
                            emoji, MessageElementFlag::EmojiAll);
                    }
                    else if (!runText.isEmpty())
                    {
                        appendYouTubeText(builder, runText, bodyColor);
                    }
                    continue;
                }
                if (!runText.isEmpty())
                {
                    appendYouTubeText(builder, runText, bodyColor);
                }
            }
        }

        if (!builder->id.isEmpty() && type != "pollEvent" &&
            type != "chatEndedEvent" && type != "chatModeChangeEvent" &&
            type != "sponsorOnlyModeStartedEvent" &&
            type != "sponsorOnlyModeEndedEvent" &&
            type != "viewerEngagementMessageEvent" && type != "tombstone" &&
            type != "raidEvent")
        {
            auto &replyIcon = getResources().buttons.replyDark;
            builder
                .emplace<CircularImageElement>(
                    Image::fromResourcePixmap(replyIcon, 0.15), 2, Qt::gray,
                    MessageElementFlag::ReplyButton)
                ->setLink({Link::ReplyToMessage, builder->id});
        }

        HighlightAlert alert;
        bool matchedHighlight = false;
        if (!builder->flags.has(MessageFlag::System))
        {
            MessageParseArgs args;
            const std::vector<TwitchBadge> noTwitchBadges;
            const auto currentAccount = this->accountName();
            auto [highlighted, result] = getApp()->getHighlights()->check({
                .args = args,
                .twitchBadges = noTwitchBadges,
                .senderName = authorName,
                .originalMessage = text,
                .messageFlags = builder.message().flags,
                .self = !currentAccount.isEmpty() &&
                        currentAccount.compare(authorName,
                                               Qt::CaseInsensitive) == 0,
                .runContext = filters::RunContext{
                    .message = builder.message(),
                    .channel = channel,
                },
            });
            if (highlighted)
            {
                matchedHighlight = true;
                builder->flags.set(MessageFlag::Highlighted);
                if (!builder->highlightColor && result.color)
                {
                    builder->highlightColor = result.color;
                }
                if (result.showInMentions)
                {
                    builder->flags.set(MessageFlag::ShowInMentions);
                }
                alert.sound = result.sound;
                alert.windowAlert = result.alert;
            }
        }

        auto message = builder.release();
        channel->applySimilarityFilters(message);
        const bool showMentions =
            message->flags.has(MessageFlag::Highlighted) &&
            message->flags.has(MessageFlag::ShowInMentions);
        QString oldMessageId;
        if (type == "pollEvent")
        {
            oldMessageId = QStringLiteral("youtube-active-poll");
        }
        else if (type == "chatEndedEvent")
        {
            oldMessageId = QStringLiteral("youtube-chat-ended");
        }
        else if (type == "raidEvent" && action == "banner" &&
                 !targetItemId.isEmpty())
        {
            oldMessageId = QStringLiteral("youtube-raid-") + targetItemId;
        }
        else
        {
            oldMessageId = targetItemId.isEmpty() ? message->id : targetItemId;
        }
        const auto oldMessage = channel->findMessageByID(oldMessageId);
        const bool replacing = action == "replace" || action == "banner" ||
                               type == "pollEvent" || oldMessage != nullptr;
        if (replacing && oldMessage)
        {
            channel->replaceMessage(oldMessage, message);
        }
        else if (!oldMessage)
        {
            channel->addMessage(message, MessageContext::Original);
        }

        if (!initialBatch && matchedHighlight &&
            (!message->flags.has(MessageFlag::Similar) ||
             (!getSettings()->hideSimilar &&
              getSettings()->shownSimilarTriggerHighlights)))
        {
            MessageBuilder::triggerHighlights(channel, alert);
        }
        if (!initialBatch && showMentions && !oldMessage &&
            channel->sinkTraits().has(
                MessageSinkTrait::AddMentionsToGlobalChannel))
        {
            getApp()->getTwitch()->getMentionsChannel()->addMessage(
                message, MessageContext::Original);
        }
    }
}

}  // namespace chatterino
