// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeInnertubeReader.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>

namespace chatterino {

namespace {

QJsonObject embeddedJsonObject(const QByteArray &source,
                               const QByteArray &marker)
{
    const auto markerPosition = source.indexOf(marker);
    if (markerPosition < 0)
    {
        return {};
    }
    const auto start = source.indexOf('{', markerPosition + marker.size());
    if (start < 0)
    {
        return {};
    }

    qsizetype depth = 0;
    bool inString = false;
    bool escaped = false;
    for (qsizetype index = start; index < source.size(); ++index)
    {
        const auto character = source.at(index);
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (character == '\\')
            {
                escaped = true;
            }
            else if (character == '"')
            {
                inString = false;
            }
            continue;
        }
        if (character == '"')
        {
            inString = true;
        }
        else if (character == '{')
        {
            ++depth;
        }
        else if (character == '}' && --depth == 0)
        {
            const auto document =
                QJsonDocument::fromJson(source.mid(start, index - start + 1));
            return document.object();
        }
    }
    return {};
}

QString textValue(const QJsonValue &value)
{
    if (value.isString())
    {
        return value.toString();
    }
    const auto object = value.toObject();
    const auto simpleText = object.value("simpleText").toString();
    if (!simpleText.isEmpty())
    {
        return simpleText;
    }

    QString text;
    for (const auto &runValue : object.value("runs").toArray())
    {
        const auto run = runValue.toObject();
        const auto runText = run.value("text").toString();
        if (!runText.isEmpty())
        {
            text += runText;
            continue;
        }

        const auto emoji = run.value("emoji").toObject();
        const auto accessibility =
            emoji.value("image").toObject().value("accessibility").toObject();
        const auto label = accessibility.value("accessibilityData")
                               .toObject()
                               .value("label")
                               .toString();
        if (!label.isEmpty())
        {
            text += label;
            continue;
        }
        const auto shortcuts = emoji.value("shortcuts").toArray();
        if (!shortcuts.isEmpty())
        {
            text += shortcuts.first().toString();
        }
        else
        {
            text += emoji.value("emojiId").toString();
        }
    }
    return text;
}

QString continuationFromObject(const QJsonObject &object, int *timeoutMs)
{
    const auto continuations = object.value("continuations").toArray();
    for (const auto &continuationValue : continuations)
    {
        const auto continuation = continuationValue.toObject();
        for (const auto *key : {"timedContinuationData",
                                "invalidationContinuationData",
                                "reloadContinuationData"})
        {
            const auto data = continuation.value(QLatin1String(key)).toObject();
            const auto token = data.value("continuation").toString();
            if (token.isEmpty())
            {
                continue;
            }
            if (timeoutMs != nullptr && data.value("timeoutMs").isDouble())
            {
                *timeoutMs = data.value("timeoutMs").toInt();
            }
            return token;
        }
    }
    return {};
}

QString findContinuation(const QJsonValue &value, int *timeoutMs,
                         int depth = 0)
{
    if (depth > 80)
    {
        return {};
    }
    if (value.isObject())
    {
        const auto object = value.toObject();
        if (object.contains("liveChatContinuation"))
        {
            const auto liveChat =
                object.value("liveChatContinuation").toObject();
            const auto token = continuationFromObject(liveChat, timeoutMs);
            if (!token.isEmpty())
            {
                return token;
            }
        }
        if (object.contains("liveChatRenderer"))
        {
            const auto renderer = object.value("liveChatRenderer").toObject();
            const auto token = continuationFromObject(renderer, timeoutMs);
            if (!token.isEmpty())
            {
                return token;
            }
        }
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        {
            const auto token = findContinuation(it.value(), timeoutMs, depth + 1);
            if (!token.isEmpty())
            {
                return token;
            }
        }
    }
    else if (value.isArray())
    {
        for (const auto &child : value.toArray())
        {
            const auto token = findContinuation(child, timeoutMs, depth + 1);
            if (!token.isEmpty())
            {
                return token;
            }
        }
    }
    return {};
}

QJsonObject normalizeChatItem(const QJsonObject &item)
{
    QJsonObject renderer;
    QString type;
    QString message;
    QString amount;
    QString memberLevel;

    renderer = item.value("liveChatTextMessageRenderer").toObject();
    if (!renderer.isEmpty())
    {
        type = "textMessageEvent";
        message = textValue(renderer.value("message"));
    }
    else
    {
        renderer = item.value("liveChatPaidMessageRenderer").toObject();
        if (!renderer.isEmpty())
        {
            type = "superChatEvent";
            message = textValue(renderer.value("message"));
            amount = textValue(renderer.value("purchaseAmountText"));
        }
    }
    if (renderer.isEmpty())
    {
        renderer = item.value("liveChatPaidStickerRenderer").toObject();
        if (!renderer.isEmpty())
        {
            type = "superStickerEvent";
            message = textValue(renderer.value("sticker"));
            amount = textValue(renderer.value("purchaseAmountText"));
        }
    }
    if (renderer.isEmpty())
    {
        renderer = item.value("liveChatMembershipItemRenderer").toObject();
        if (!renderer.isEmpty())
        {
            type = "newSponsorEvent";
            message = textValue(renderer.value("headerPrimaryText"));
            memberLevel = textValue(renderer.value("headerSubtext"));
            if (message.isEmpty())
            {
                message = textValue(renderer.value("message"));
            }
        }
    }
    if (renderer.isEmpty() || type.isEmpty())
    {
        return {};
    }

    const auto authorName = textValue(renderer.value("authorName"));
    const auto publishedAtUsec = renderer.value("timestampUsec").toString();
    bool timestampOk = false;
    const auto publishedAtMs = publishedAtUsec.toLongLong(&timestampOk) / 1000;
    const auto publishedAt =
        timestampOk
            ? QDateTime::fromMSecsSinceEpoch(publishedAtMs, Qt::UTC)
                  .toString(Qt::ISODateWithMs)
            : QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    bool isOwner = false;
    bool isModerator = false;
    bool isMember = false;
    bool isVerified = false;
    for (const auto &badgeValue : renderer.value("authorBadges").toArray())
    {
        const auto badge =
            badgeValue.toObject().value("liveChatAuthorBadgeRenderer").toObject();
        const auto tooltip = badge.value("tooltip").toString().toLower();
        const auto iconType = badge.value("icon")
                                  .toObject()
                                  .value("iconType")
                                  .toString()
                                  .toUpper();
        isOwner = isOwner || iconType == "OWNER" ||
                  tooltip.contains("owner") || tooltip.contains("broadcaster");
        isModerator = isModerator || iconType == "MODERATOR" ||
                      tooltip.contains("moderator");
        isMember = isMember || iconType == "SPONSOR" ||
                   tooltip.contains("member") || tooltip.contains("sponsor");
        isVerified = isVerified || iconType == "VERIFIED" ||
                     tooltip.contains("verified");
        if (memberLevel.isEmpty() && isMember)
        {
            memberLevel = badge.value("tooltip").toString();
        }
    }

    const auto authorType = renderer.value("authorType").toString().toLower();
    isOwner = isOwner || authorType == "owner";
    isModerator = isModerator || authorType == "moderator";

    QJsonObject snippet{
        {"type", type},
        {"publishedAt", publishedAt},
        {"displayMessage", message},
    };
    if (type == "textMessageEvent")
    {
        snippet.insert("textMessageDetails",
                       QJsonObject{{"messageText", message}});
    }
    else if (type == "superChatEvent")
    {
        snippet.insert("superChatDetails",
                       QJsonObject{{"amountDisplayString", amount}});
    }
    else if (type == "superStickerEvent")
    {
        snippet.insert("superStickerDetails",
                       QJsonObject{{"superStickerMetadata",
                                    QJsonObject{{"altText", message}}}});
    }
    else if (type == "newSponsorEvent")
    {
        snippet.insert("newSponsorDetails",
                       QJsonObject{{"memberLevelName", memberLevel}});
    }

    QJsonObject author{
        {"channelId", renderer.value("authorExternalChannelId").toString()},
        {"displayName", authorName},
        {"isChatOwner", isOwner},
        {"isChatModerator", isModerator},
        {"isChatSponsor", isMember},
        {"isVerified", isVerified},
    };

    return QJsonObject{
        {"id", renderer.value("id").toString()},
        {"snippet", snippet},
        {"authorDetails", author},
    };
}

QJsonObject normalizeChatResponse(const QJsonObject &response)
{
    const auto liveChat = response.value("continuationContents")
                              .toObject()
                              .value("liveChatContinuation")
                              .toObject();
    QJsonArray items;
    for (const auto &actionValue : liveChat.value("actions").toArray())
    {
        const auto action = actionValue.toObject();
        const auto item = action.value("addChatItemAction")
                              .toObject()
                              .value("item")
                              .toObject();
        if (item.isEmpty())
        {
            continue;
        }
        auto normalized = normalizeChatItem(item);
        if (!normalized.isEmpty())
        {
            items.append(normalized);
        }
    }
    return QJsonObject{{"items", items}};
}

}  // namespace

YouTubeInnertubeReader::YouTubeInnertubeReader(
    QString videoId, BootstrapCallback bootstrapCallback,
    MessageCallback messageCallback, ErrorCallback errorCallback,
    QObject *parent)
    : QObject(parent)
    , videoId_(std::move(videoId))
    , bootstrapCallback_(std::move(bootstrapCallback))
    , messageCallback_(std::move(messageCallback))
    , errorCallback_(std::move(errorCallback))
{
    this->requestTimer_.setSingleShot(true);
    QObject::connect(&this->requestTimer_, &QTimer::timeout, this, [this] {
        if (this->stopped_)
        {
            return;
        }
        if (this->bootstrapLoaded_ && !this->retryLoadsPage_)
        {
            this->requestChat();
        }
        else
        {
            this->retryLoadsPage_ = false;
            this->loadWatchPage();
        }
    });
}

void YouTubeInnertubeReader::start()
{
    if (this->stopped_)
    {
        return;
    }
    this->loadWatchPage();
}

void YouTubeInnertubeReader::stop()
{
    this->stopped_ = true;
    this->requestTimer_.stop();
}

void YouTubeInnertubeReader::loadWatchPage()
{
    if (this->stopped_ || this->videoId_.isEmpty())
    {
        return;
    }

    QUrl url("https://www.youtube.com/watch");
    QUrlQuery query;
    query.addQueryItem("v", this->videoId_);
    query.addQueryItem("hl", "en");
    url.setQuery(query);

    auto request = NetworkRequest(url);
    std::move(request)
        .header("Accept-Language", "en-US,en;q=0.9")
        .header("User-Agent",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/131.0.0.0 Safari/537.36")
        .timeout(20000)
        .caller(this)
        .onSuccess([this](const NetworkResult &result) {
            if (this->stopped_)
            {
                return;
            }
            const auto html = result.getData();
            static const QRegularExpression apiKeyPattern(
                R"re("INNERTUBE_API_KEY"\s*:\s*"([^"]+)")re");
            static const QRegularExpression clientVersionPattern(
                R"re("INNERTUBE_CLIENT_VERSION"\s*:\s*"([^"]+)")re");
            const auto apiKeyMatch = apiKeyPattern.match(QString::fromUtf8(html));
            const auto clientVersionMatch =
                clientVersionPattern.match(QString::fromUtf8(html));
            this->apiKey_ = apiKeyMatch.captured(1);
            this->clientVersion_ = clientVersionMatch.captured(1);
            const auto player =
                embeddedJsonObject(html, "ytInitialPlayerResponse");
            const auto initialData = embeddedJsonObject(html, "ytInitialData");

            VideoInfo info;
            const auto videoDetails = player.value("videoDetails").toObject();
            info.title = videoDetails.value("title").toString();
            info.channelTitle = videoDetails.value("author").toString();
            const auto thumbnails =
                videoDetails.value("thumbnail").toObject().value("thumbnails").toArray();
            if (!thumbnails.isEmpty())
            {
                info.thumbnailUrl =
                    thumbnails.last().toObject().value("url").toString();
            }

            int timeoutMs = 1000;
            this->continuation_ = findContinuation(initialData, &timeoutMs);
            const bool hasClientConfig = !this->apiKey_.isEmpty() &&
                                         !this->clientVersion_.isEmpty();
            if (this->bootstrapCallback_)
            {
                this->bootstrapCallback_(
                    info, hasClientConfig && !this->continuation_.isEmpty());
            }
            if (!hasClientConfig)
            {
                this->reportError(
                    "Could not read YouTube's public chat configuration from the "
                    "video page.");
                this->scheduleRetry(true, 5000);
                return;
            }
            if (this->continuation_.isEmpty())
            {
                this->reportError(
                    "No active public YouTube live chat was found for this video.");
                return;
            }

            this->bootstrapLoaded_ = true;
            this->retryLoadsPage_ = false;
            this->retryAttempt_ = 0;
            this->errorReported_ = false;
            this->requestChat();
        })
        .onError([this](const NetworkResult &result) {
            if (this->stopped_)
            {
                return;
            }
            this->reportError("Could not open the YouTube video page: " +
                              result.formatError());
            this->scheduleRetry(true);
        })
        .execute();
}

void YouTubeInnertubeReader::requestChat()
{
    if (this->stopped_ || !this->bootstrapLoaded_ ||
        this->continuation_.isEmpty())
    {
        return;
    }

    QUrl url("https://www.youtube.com/youtubei/v1/live_chat/get_live_chat");
    QUrlQuery query;
    query.addQueryItem("key", this->apiKey_);
    query.addQueryItem("prettyPrint", "false");
    url.setQuery(query);

    const QJsonObject payload{
        {"context",
         QJsonObject{
             {"client",
              QJsonObject{
                  {"clientName", "WEB"},
                  {"clientVersion", this->clientVersion_},
                  {"hl", "en"},
                  {"gl", "US"},
              }},
         }},
        {"continuation", this->continuation_},
    };

    auto request = NetworkRequest(url, NetworkRequestType::Post);
    std::move(request)
        .header("Content-Type", "application/json")
        .header("Origin", "https://www.youtube.com")
        .header("Referer", "https://www.youtube.com/watch?v=" + this->videoId_)
        .header("X-YouTube-Client-Name", "1")
        .header("X-YouTube-Client-Version", this->clientVersion_)
        .json(payload)
        .timeout(20000)
        .caller(this)
        .onSuccess([this](const NetworkResult &result) {
            if (this->stopped_)
            {
                return;
            }
            const auto response = result.parseJson();
            const auto error = response.value("error").toObject();
            if (!error.isEmpty())
            {
                this->reportError(error.value("message").toString());
                this->scheduleRetry(false, 5000);
                return;
            }

            int timeoutMs = 1000;
            const auto nextContinuation =
                findContinuation(response, &timeoutMs);
            if (nextContinuation.isEmpty())
            {
                this->reportError(
                    "YouTube did not return a continuation for live chat.");
                this->scheduleRetry(true, 5000);
                return;
            }

            const auto normalized = normalizeChatResponse(response);
            if (!normalized.value("items").toArray().isEmpty() &&
                this->messageCallback_)
            {
                this->messageCallback_(normalized);
            }
            this->continuation_ = nextContinuation;
            this->retryAttempt_ = 0;
            this->errorReported_ = false;
            // Bound the website's suggested wait so busy chats do not sit
            // idle for several seconds between continuation requests.
            this->requestTimer_.start(std::clamp(timeoutMs, 500, 1500));
        })
        .onError([this](const NetworkResult &result) {
            if (this->stopped_)
            {
                return;
            }
            const auto status = result.status().value_or(0);
            this->reportError("YouTube chat read failed: " +
                              result.formatError());
            this->scheduleRetry(false, status == 429 ? 30000 : 0);
        })
        .execute();
}

void YouTubeInnertubeReader::scheduleRetry(bool reloadPage,
                                          int minimumDelayMs)
{
    if (this->stopped_)
    {
        return;
    }
    this->retryLoadsPage_ = reloadPage;
    const int delays[] = {1000, 2000, 5000, 10000, 30000, 60000};
    const auto delay = std::max(
        minimumDelayMs,
        delays[std::min(this->retryAttempt_,
                        static_cast<int>(std::size(delays)) - 1)]);
    ++this->retryAttempt_;
    this->requestTimer_.start(delay);
}

void YouTubeInnertubeReader::reportError(const QString &message)
{
    if (this->errorReported_ || message.isEmpty())
    {
        return;
    }
    this->errorReported_ = true;
    if (this->errorCallback_)
    {
        this->errorCallback_(message);
    }
}

}  // namespace chatterino
