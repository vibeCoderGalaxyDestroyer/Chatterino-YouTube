// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeInnertubeReader.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/youtube/YouTubeDiagnostics.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <atomic>
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

QString emojiText(const QJsonObject &emoji)
{
    const auto emojiId = emoji.value("emojiId").toString();
    if (!emoji.value("isCustomEmoji").toBool() && !emojiId.isEmpty())
    {
        return emojiId;
    }

    const auto accessibility =
        emoji.value("image").toObject().value("accessibility").toObject();
    const auto label = accessibility.value("accessibilityData")
                           .toObject()
                           .value("label")
                           .toString();
    if (!label.isEmpty())
    {
        return label;
    }

    const auto shortcuts = emoji.value("shortcuts").toArray();
    if (!shortcuts.isEmpty())
    {
        return shortcuts.first().toString();
    }
    return emojiId;
}

QString textValue(const QJsonValue &value)
{
    if (value.isString())
    {
        return value.toString();
    }
    if (value.isDouble())
    {
        return QString::number(value.toInteger());
    }
    const auto object = value.toObject();
    const auto simpleText = object.value("simpleText").toString();
    if (!simpleText.isEmpty())
    {
        return simpleText;
    }
    for (const auto *key : {"content", "text"})
    {
        const auto directText = object.value(QLatin1String(key)).toString();
        if (!directText.isEmpty())
        {
            return directText;
        }
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

        text += emojiText(run.value("emoji").toObject());
    }
    return text;
}

void collectChannelSearchResults(
    const QJsonValue &value,
    QList<YouTubeInnertubeReader::ChannelSearchResult> &results,
    QSet<QString> &seenChannelIds, int depth = 0)
{
    if (depth > 80)
    {
        return;
    }
    if (value.isObject())
    {
        const auto object = value.toObject();
        const auto renderer = object.value("channelRenderer").toObject();
        if (!renderer.isEmpty())
        {
            const auto channelId = renderer.value("channelId").toString();
            if (!channelId.isEmpty() && !seenChannelIds.contains(channelId))
            {
                const auto endpoint =
                    renderer.value("navigationEndpoint").toObject();
                const auto command = endpoint.value("commandMetadata")
                                         .toObject()
                                         .value("webCommandMetadata")
                                         .toObject();
                const auto browse = endpoint.value("browseEndpoint").toObject();
                auto url = command.value("url").toString();
                if (url.isEmpty())
                {
                    url = browse.value("canonicalBaseUrl").toString();
                }
                if (url.isEmpty())
                {
                    url = "/channel/" + channelId;
                }
                results.append({channelId,
                                textValue(renderer.value("title")), url});
                seenChannelIds.insert(channelId);
            }
        }
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        {
            collectChannelSearchResults(it.value(), results, seenChannelIds,
                                        depth + 1);
        }
    }
    else if (value.isArray())
    {
        for (const auto &child : value.toArray())
        {
            collectChannelSearchResults(child, results, seenChannelIds,
                                        depth + 1);
        }
    }
}

QJsonArray normalizeMessageRuns(const QJsonValue &value)
{
    QJsonArray normalized;
    const auto runs = value.toObject().value("runs").toArray();
    for (const auto &runValue : runs)
    {
        const auto run = runValue.toObject();
        const auto text = run.value("text").toString();
        if (!text.isEmpty())
        {
            normalized.append(QJsonObject{{"text", text}});
            continue;
        }

        const auto emoji = run.value("emoji").toObject();
        if (emoji.isEmpty())
        {
            continue;
        }

        QJsonObject normalizedRun{{"text", emojiText(emoji)}};
        if (emoji.value("isCustomEmoji").toBool())
        {
            const auto image = emoji.value("image").toObject();
            const auto accessibility = image.value("accessibility").toObject();
            const auto label = accessibility.value("accessibilityData")
                                   .toObject()
                                   .value("label")
                                   .toString();
            normalizedRun.insert("isCustomEmoji", true);
            normalizedRun.insert("emojiId", emoji.value("emojiId"));
            normalizedRun.insert("label", label);
            normalizedRun.insert("thumbnails",
                                 image.value("thumbnails").toArray());
        }
        normalized.append(normalizedRun);
    }
    return normalized;
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

QJsonObject findObjectByName(const QJsonValue &value, const QString &name,
                             int depth = 0)
{
    if (depth > 80)
    {
        return {};
    }
    if (value.isObject())
    {
        const auto object = value.toObject();
        const auto candidate = object.value(name).toObject();
        if (!candidate.isEmpty())
        {
            return candidate;
        }
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        {
            const auto found = findObjectByName(it.value(), name, depth + 1);
            if (!found.isEmpty())
            {
                return found;
            }
        }
    }
    else if (value.isArray())
    {
        for (const auto &child : value.toArray())
        {
            const auto found = findObjectByName(child, name, depth + 1);
            if (!found.isEmpty())
            {
                return found;
            }
        }
    }
    return {};
}

QString findStringByName(const QJsonValue &value, const QString &name,
                         int depth = 0)
{
    if (depth > 80)
    {
        return {};
    }
    if (value.isObject())
    {
        const auto object = value.toObject();
        const auto candidate = object.value(name);
        if (candidate.isString())
        {
            return candidate.toString();
        }
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        {
            const auto found = findStringByName(it.value(), name, depth + 1);
            if (!found.isEmpty())
            {
                return found;
            }
        }
    }
    else if (value.isArray())
    {
        for (const auto &child : value.toArray())
        {
            const auto found = findStringByName(child, name, depth + 1);
            if (!found.isEmpty())
            {
                return found;
            }
        }
    }
    return {};
}

qint64 liveViewerCount(const QJsonValue &initialData)
{
    const auto renderer =
        findObjectByName(initialData, "videoViewCountRenderer");
    const auto countText = textValue(renderer.value("viewCount"));
    if (!countText.contains("watch", Qt::CaseInsensitive))
    {
        return -1;
    }

    static const QRegularExpression countPattern(
        R"re(([0-9]+(?:[.,][0-9]+)?)\s*([KMB])?\s+watch)re",
        QRegularExpression::CaseInsensitiveOption);
    const auto match = countPattern.match(countText);
    if (!match.hasMatch())
    {
        return -1;
    }

    auto numericText = match.captured(1);
    const auto suffix = match.captured(2).toUpper();
    double multiplier = 1.0;
    if (!suffix.isEmpty())
    {
        numericText.replace(',', '.');
        multiplier = suffix == "K" ? 1'000.0
                     : suffix == "M" ? 1'000'000.0
                                      : 1'000'000'000.0;
    }
    else
    {
        numericText.remove(',');
        numericText.remove('.');
    }

    bool ok = false;
    const auto count = numericText.toDouble(&ok);
    return ok ? static_cast<qint64>(count * multiplier) : -1;
}

QJsonObject normalizeChatItem(const QJsonObject &item)
{
    const auto redirectRenderer =
        findObjectByName(item, "liveChatBannerRedirectRenderer");
    if (!redirectRenderer.isEmpty())
    {
        const auto redirectType =
            textValue(redirectRenderer.value("redirectType"))
                .trimmed()
                .toUpper();
        const auto redirectVideoId =
            textValue(redirectRenderer.value("redirectVideoId")).trimmed();
        const auto redirectHandle =
            textValue(redirectRenderer.value("redirectChannelHandle"))
                .trimmed();
        auto bannerMessage =
            textValue(redirectRenderer.value("bannerMessage")).trimmed();
        const bool incomingRaid =
            redirectType == "RAID" ||
            (redirectVideoId.isEmpty() &&
             (!redirectHandle.isEmpty() ||
              bannerMessage.contains("joined", Qt::CaseInsensitive) ||
              bannerMessage.contains("their viewers", Qt::CaseInsensitive)));
        if (incomingRaid)
        {
            if (bannerMessage.isEmpty())
            {
                bannerMessage = redirectHandle.isEmpty()
                                    ? QStringLiteral(
                                          "Their viewers just joined. Say hello!")
                                    : redirectHandle +
                                          QStringLiteral(
                                              " and their viewers just joined. "
                                              "Say hello!");
            }
            if (!bannerMessage.startsWith("[RAID]", Qt::CaseInsensitive))
            {
                bannerMessage.prepend("[RAID] ");
            }

            auto timestamp = QDateTime::currentDateTimeUtc();
            bool timestampOk = false;
            const auto timestampUsec =
                redirectRenderer.value("timestampUsec").toString();
            const auto timestampMs = timestampUsec.toLongLong(&timestampOk) /
                                     1000;
            if (timestampOk)
            {
                timestamp = QDateTime::fromMSecsSinceEpoch(timestampMs,
                                                            Qt::UTC);
            }

            return QJsonObject{
                {"id", redirectRenderer.value("id").toString()},
                {"snippet",
                 QJsonObject{
                     {"type", "raidEvent"},
                     {"publishedAt", timestamp.toString(Qt::ISODateWithMs)},
                     {"displayMessage", bannerMessage},
                 }},
                {"authorDetails", QJsonObject{{"displayName", "YouTube"}}},
            };
        }
    }

    QJsonObject renderer;
    QString type;
    QString message;
    QString amount;
    QString memberLevel;
    int memberMonth = 0;

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
        renderer = item.value("liveChatDonationAnnouncementRenderer").toObject();
        if (!renderer.isEmpty())
        {
            type = "superChatEvent";
            message = textValue(renderer.value("message"));
            amount = textValue(renderer.value("amount"));
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
    if (renderer.isEmpty())
    {
        renderer =
            item.value("liveChatSponsorshipsMilestoneItemRenderer").toObject();
        if (!renderer.isEmpty())
        {
            type = "memberMilestoneChatEvent";
            message = textValue(renderer.value("message"));
            memberLevel = textValue(renderer.value("headerSubtext"));
            const auto header = textValue(renderer.value("headerPrimaryText"));
            static const QRegularExpression monthPattern(
                R"((\d+)\s*(?:st|nd|rd|th)?\s+month)" ,
                QRegularExpression::CaseInsensitiveOption);
            const auto match = monthPattern.match(header + ' ' + memberLevel);
            if (match.hasMatch())
            {
                memberMonth = match.captured(1).toInt();
            }
        }
    }
    if (renderer.isEmpty())
    {
        renderer = item.value(
            "liveChatSponsorshipsGiftPurchaseAnnouncementRenderer")
                       .toObject();
        if (!renderer.isEmpty())
        {
            type = "membershipGiftingEvent";
            const auto header = renderer.value("header").toObject();
            const auto headerRenderer =
                header.value("liveChatSponsorshipsHeaderRenderer").toObject();
            const auto giftHeader =
                headerRenderer.isEmpty() ? header : headerRenderer;
            message = textValue(giftHeader.value("primaryText"));
        }
    }
    if (renderer.isEmpty())
    {
        renderer = item.value(
            "liveChatSponsorshipsGiftRedemptionAnnouncementRenderer")
                       .toObject();
        if (!renderer.isEmpty())
        {
            type = "giftMembershipReceivedEvent";
            message = textValue(renderer.value("message"));
        }
    }
    if (renderer.isEmpty())
    {
        // Jewels gifts are separate from membership gifts. The renderer name
        // has changed across YouTube's live-chat clients, so accept the known
        // gift model names plus future non-membership gift renderers.
        for (auto it = item.constBegin(); it != item.constEnd(); ++it)
        {
            const auto key = it.key().toLower();
            if ((!key.contains("gift") && !key.contains("jewel")) ||
                key.contains("membership") || key.contains("sponsor"))
            {
                continue;
            }

            const auto candidate = it.value().toObject();
            if (candidate.isEmpty())
            {
                continue;
            }

            renderer = candidate;
            type = "giftEvent";
            message = textValue(renderer.value("message"));
            if (message.isEmpty())
            {
                message = textValue(renderer.value("displayMessage"));
            }
            if (message.isEmpty())
            {
                message = textValue(renderer.value("primaryText"));
            }
            if (message.isEmpty())
            {
                message = textValue(renderer.value("subtitle"));
            }
            break;
        }
    }
    if (renderer.isEmpty())
    {
        renderer = item.value("liveChatModeChangeMessageRenderer").toObject();
        if (!renderer.isEmpty())
        {
            type = "chatModeChangeEvent";
            message = textValue(renderer.value("text"));
            const auto lowered = message.toLower();
            if (lowered.contains("members-only") ||
                lowered.contains("members only"))
            {
                type = lowered.contains("off") || lowered.contains("ended")
                           ? "sponsorOnlyModeEndedEvent"
                           : "sponsorOnlyModeStartedEvent";
            }
        }
    }
    if (renderer.isEmpty())
    {
        renderer =
            item.value("liveChatViewerEngagementMessageRenderer").toObject();
        if (!renderer.isEmpty())
        {
            type = "viewerEngagementMessageEvent";
            message = textValue(renderer.value("message"));
        }
    }
    if (renderer.isEmpty())
    {
        renderer = item.value("liveChatPollRenderer").toObject();
        if (!renderer.isEmpty())
        {
            type = "pollEvent";
            message = textValue(renderer.value("question"));
        }
    }
    if (renderer.isEmpty())
    {
        renderer = item.value("liveChatTombstoneRenderer").toObject();
        if (!renderer.isEmpty())
        {
            type = "tombstone";
            message = textValue(renderer.value("bodyText"));
        }
    }
    if (!renderer.isEmpty() && type != "giftEvent" &&
        (!findObjectByName(renderer, "giftEventDetails").isEmpty() ||
         !findObjectByName(renderer, "giftDetails").isEmpty()))
    {
        // Jewels gifts can arrive as a text-message renderer with gift details
        // attached, rather than as a separate renderer key.
        type = "giftEvent";
    }
    if (renderer.isEmpty() || type.isEmpty())
    {
        return {};
    }

    auto authorName = textValue(renderer.value("authorName"));
    auto authorChannelId =
        renderer.value("authorExternalChannelId").toString();
    QJsonArray authorBadges = renderer.value("authorBadges").toArray();
    QJsonValue messageValue = renderer.value("message");
    if (type == "superStickerEvent")
    {
        messageValue = renderer.value("sticker");
    }
    if (type == "membershipGiftingEvent")
    {
        const auto header = renderer.value("header").toObject();
        const auto headerRenderer =
            header.value("liveChatSponsorshipsHeaderRenderer").toObject();
        const auto giftHeader =
            headerRenderer.isEmpty() ? header : headerRenderer;
        authorName = textValue(giftHeader.value("authorName"));
        if (authorChannelId.isEmpty())
        {
            authorChannelId =
                giftHeader.value("authorExternalChannelId").toString();
        }
        authorBadges = giftHeader.value("authorBadges").toArray();
        messageValue = giftHeader.value("primaryText");
    }
    if (type == "memberMilestoneChatEvent" && messageValue.isUndefined())
    {
        messageValue = renderer.value("message");
    }
    const auto messageRuns = normalizeMessageRuns(messageValue);

    QJsonObject giftDetails;
    QJsonObject giftMetadata;
    QString giftName;
    QString giftUrl;
    QJsonArray giftThumbnails;
    if (type == "giftEvent")
    {
        giftDetails = renderer.value("giftDetails").toObject();
        if (giftDetails.isEmpty())
        {
            giftDetails = findObjectByName(renderer, "giftDetails");
        }
        if (giftDetails.isEmpty())
        {
            giftDetails = findObjectByName(renderer, "giftEventDetails");
        }
        if (giftDetails.isEmpty())
        {
            giftDetails = findObjectByName(renderer, "giftMetadata");
        }
        if (giftDetails.isEmpty())
        {
            giftDetails = findObjectByName(renderer, "gift");
        }
        giftMetadata = giftDetails.value("giftMetadata").toObject();
        if (giftMetadata.isEmpty())
        {
            giftMetadata = findObjectByName(renderer, "giftMetadata");
        }

        giftName = textValue(giftDetails.value("giftName"));
        if (giftName.isEmpty())
        {
            giftName = textValue(giftMetadata.value("giftName"));
        }
        if (giftName.isEmpty())
        {
            giftName = textValue(giftDetails.value("name"));
        }
        if (giftName.isEmpty())
        {
            giftName = textValue(giftMetadata.value("name"));
        }
        if (giftName.isEmpty())
        {
            giftName = textValue(giftDetails.value("altText"));
        }
        if (giftName.isEmpty())
        {
            giftName = textValue(giftMetadata.value("altText"));
        }
        if (giftName.isEmpty())
        {
            giftName = textValue(renderer.value("giftName"));
        }
        if (giftName.isEmpty())
        {
            giftName = findStringByName(renderer, "giftName");
        }
        if (giftName.isEmpty())
        {
            giftName =
                textValue(renderer.value("giftImageA11yLabel")).trimmed();
        }
        if (giftName.isEmpty())
        {
            giftName = findStringByName(giftDetails, "altText");
        }
        if (giftName.isEmpty())
        {
            giftName = findStringByName(giftMetadata, "altText");
        }
        if (giftName.isEmpty())
        {
            giftName = findStringByName(giftDetails, "label");
        }
        if (giftName.isEmpty())
        {
            giftName = findStringByName(giftMetadata, "label");
        }
        if (giftName.isEmpty())
        {
            for (const auto &runValue : messageRuns)
            {
                const auto run = runValue.toObject();
                if (run.value("isCustomEmoji").toBool())
                {
                    giftName = run.value("label").toString().trimmed();
                    if (!giftName.isEmpty())
                    {
                        break;
                    }
                }
            }
        }
        static const QRegularExpression genericGiftName(
            R"(^\s*(?:sent\s+)?(?:a\s+)?gift\s*$)",
            QRegularExpression::CaseInsensitiveOption);
        if (genericGiftName.match(giftName).hasMatch())
        {
            giftName.clear();
        }
        if (giftName.isEmpty())
        {
            for (const auto *field : {"giftName", "name", "title",
                                      "productName", "displayName",
                                      "altText", "label"})
            {
                auto candidate =
                    findStringByName(giftDetails, QString::fromLatin1(field))
                        .trimmed();
                if (candidate.isEmpty() ||
                    genericGiftName.match(candidate).hasMatch())
                {
                    candidate =
                        findStringByName(giftMetadata,
                                         QString::fromLatin1(field))
                            .trimmed();
                }
                if (!candidate.isEmpty() &&
                    !genericGiftName.match(candidate).hasMatch())
                {
                    giftName = std::move(candidate);
                    break;
                }
            }
        }
        if (giftName.isEmpty())
        {
            static const QRegularExpression sentGiftPattern(
                R"(^\s*sent\s+(.+?)\s*$)",
                QRegularExpression::CaseInsensitiveOption);
            const auto match = sentGiftPattern.match(message);
            if (match.hasMatch())
            {
                const auto candidate = match.captured(1).trimmed();
                if (!genericGiftName.match(candidate).hasMatch())
                {
                    giftName = candidate;
                }
            }
        }

        giftThumbnails = giftDetails.value("thumbnails").toArray();
        if (giftThumbnails.isEmpty())
        {
            giftThumbnails = giftMetadata.value("thumbnails").toArray();
        }
        auto giftImage = giftDetails.value("giftUrl");
        if (giftImage.isUndefined())
        {
            giftImage = giftDetails.value("image");
        }
        if (giftImage.isUndefined())
        {
            giftImage = giftDetails.value("giftImage");
        }
        if (giftImage.isUndefined())
        {
            giftImage = giftMetadata.value("giftUrl");
        }
        if (giftImage.isUndefined())
        {
            giftImage = giftMetadata.value("giftImage");
        }
        if (giftImage.isUndefined())
        {
            giftImage = renderer.value("giftUrl");
        }
        if (giftImage.isUndefined())
        {
            giftImage = renderer.value("giftImage");
        }
        if (giftImage.isUndefined())
        {
            giftImage = renderer.value("productImage");
        }
        if (giftImage.isUndefined())
        {
            giftImage = renderer.value("image");
        }

        const auto imageObject = giftImage.toObject();
        if (giftName.isEmpty())
        {
            auto accessibility =
                findObjectByName(imageObject, "accessibilityData");
            giftName = textValue(accessibility.value("label")).trimmed();
            if (genericGiftName.match(giftName).hasMatch())
            {
                giftName.clear();
            }
        }
        if (giftName.isEmpty())
        {
            auto accessibility =
                findObjectByName(giftDetails, "accessibilityData");
            giftName = textValue(accessibility.value("label")).trimmed();
            if (genericGiftName.match(giftName).hasMatch())
            {
                giftName.clear();
            }
        }
        if (giftName.isEmpty())
        {
            static std::atomic_bool loggedMissingGiftNameShape = false;
            if (!loggedMissingGiftNameShape.exchange(true))
            {
                logYouTubeDiagnostic(
                    "gift_name_unavailable",
                    QString("renderer_keys=%1 gift_details_keys=%2 "
                            "gift_metadata_keys=%3 image_keys=%4")
                        .arg(renderer.keys().join(','),
                             giftDetails.keys().join(','),
                             giftMetadata.keys().join(','),
                             imageObject.keys().join(',')));
            }
        }
        if (giftThumbnails.isEmpty())
        {
            giftThumbnails = imageObject.value("thumbnails").toArray();
        }
        if (giftThumbnails.isEmpty())
        {
            giftThumbnails = imageObject.value("sources").toArray();
        }
        if (giftImage.isString())
        {
            giftUrl = giftImage.toString();
        }
        if (giftUrl.isEmpty())
        {
            giftUrl = imageObject.value("url").toString();
        }
        if (giftUrl.isEmpty())
        {
            giftUrl = imageObject.value("src").toString();
        }
        if (giftUrl.isEmpty())
        {
            giftUrl = findStringByName(renderer, "giftUrl");
        }
        if (giftThumbnails.isEmpty() && !giftUrl.isEmpty())
        {
            giftThumbnails.append(QJsonObject{{"url", giftUrl},
                                              {"width", 128},
                                              {"height", 128}});
        }
        if (message.isEmpty() && !giftName.isEmpty())
        {
            message = "sent " + giftName;
        }
    }
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
    QJsonArray normalizedAuthorBadges;
    for (const auto &badgeValue : authorBadges)
    {
        const auto badge =
            badgeValue.toObject().value("liveChatAuthorBadgeRenderer").toObject();
        const auto tooltip = badge.value("tooltip").toString().toLower();
        const auto iconType = badge.value("icon")
                                  .toObject()
                                  .value("iconType")
                                  .toString()
                                  .toUpper();
        const bool isMemberBadge =
            iconType == "SPONSOR" || tooltip.contains("member") ||
            tooltip.contains("sponsor");
        normalizedAuthorBadges.append(QJsonObject{
            {"tooltip", badge.value("tooltip")},
            {"iconType", iconType},
            {"isMember", isMemberBadge},
            {"thumbnails", badge.value("customThumbnail")
                                .toObject()
                                .value("thumbnails")
                                .toArray()},
        });
        isOwner = isOwner || iconType == "OWNER" ||
                  tooltip.contains("owner") || tooltip.contains("broadcaster");
        isModerator = isModerator || iconType == "MODERATOR" ||
                      tooltip.contains("moderator");
        isMember = isMember || isMemberBadge;
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
    if (!messageRuns.isEmpty())
    {
        snippet.insert("messageRuns", messageRuns);
    }
    if (type == "superStickerEvent")
    {
        const auto sticker = renderer.value("sticker").toObject();
        snippet.insert(
            "superStickerDetails",
            QJsonObject{
                {"superStickerMetadata",
                 QJsonObject{
                     {"altText", message},
                     {"stickerId", sticker.value("accessibility")
                                        .toObject()
                                        .value("accessibilityData")
                                        .toObject()
                                        .value("label")},
                     {"thumbnails", sticker.value("thumbnails")},
                     {"backgroundColor", renderer.value("backgroundColor")},
                     {"authorNameTextColor",
                      renderer.value("authorNameTextColor")},
                 }},
                {"amountDisplayString", amount},
            });
    }
    auto replyTargetId = findStringByName(renderer, "replyToMessageId");
    if (replyTargetId.isEmpty())
    {
        replyTargetId = findStringByName(renderer, "replyParentMessageId");
    }
    if (!replyTargetId.isEmpty())
    {
        snippet.insert("replyTargetId", replyTargetId);
    }

    QJsonObject replyButtonData;
    QJsonArray beforeContentButtons =
        renderer.value("beforeContentButtons").toArray();
    for (const auto &buttonValue : beforeContentButtons)
    {
        const auto button = buttonValue.toObject();
        const auto viewModel = button.value("buttonViewModel").toObject();
        const auto buttonData = viewModel.isEmpty() ? button : viewModel;
        auto title = textValue(buttonData.value("title")).trimmed();
        if (title.isEmpty())
        {
            title = textValue(buttonData.value("titleFormatted")).trimmed();
        }
        const auto iconName = buttonData.value("iconName").toString();
        const auto accessibilityText =
            buttonData.value("accessibilityText").toString();
        const auto tooltip = buttonData.value("tooltip").toString();
        const auto buttonDescription =
            (iconName + ' ' + accessibilityText + ' ' + tooltip).toLower();
        const bool isReplyButton =
            iconName.compare("MESSAGE", Qt::CaseInsensitive) == 0 ||
            title.startsWith('@') || buttonDescription.contains("reply");
        if (isReplyButton)
        {
            replyButtonData = buttonData;
            if (!title.isEmpty() &&
                !title.contains("reply", Qt::CaseInsensitive))
            {
                static const QRegularExpression handlePattern(
                    R"(@[^\s:]+)");
                const auto handleMatch = handlePattern.match(title);
                if (handleMatch.hasMatch())
                {
                    title = handleMatch.captured(0);
                }
                else if (!title.startsWith('@'))
                {
                    title.prepend('@');
                }
                snippet.insert("replyTargetName", title);
            }
            const auto buttonReplyId =
                findStringByName(buttonData, "replyToMessageId");
            if (!buttonReplyId.isEmpty())
            {
                replyTargetId = buttonReplyId;
                snippet.insert("replyTargetId", buttonReplyId);
            }
            break;
        }
    }
    auto replyDetails = findObjectByName(renderer, "replyToMessage");
    if (replyDetails.isEmpty())
    {
        replyDetails = findObjectByName(renderer, "replyToMessageRenderer");
    }
    if (replyDetails.isEmpty())
    {
        replyDetails = findObjectByName(renderer, "replyToMessageDetails");
    }
    if (replyDetails.isEmpty())
    {
        replyDetails = findObjectByName(renderer, "replyMessage");
    }
    if (replyDetails.isEmpty())
    {
        replyDetails = findObjectByName(renderer, "parentMessage");
    }
    if (replyDetails.isEmpty() && !replyButtonData.isEmpty())
    {
        replyDetails = findObjectByName(replyButtonData, "replyToMessage");
    }
    QString replyRendererType;
    for (const auto &replyType :
         {"liveChatTextMessageRenderer", "liveChatPaidMessageRenderer",
          "liveChatDonationAnnouncementRenderer",
          "liveChatPaidStickerRenderer"})
    {
        const auto nestedReply = findObjectByName(replyDetails, replyType);
        if (!nestedReply.isEmpty())
        {
            replyDetails = nestedReply;
            replyRendererType = QLatin1String(replyType);
            break;
        }
    }
    if (!replyDetails.isEmpty())
    {
        auto replyName = textValue(replyDetails.value("authorName"));
        if (replyName.isEmpty())
        {
            replyName = textValue(replyDetails.value("authorNameV2"));
        }
        if (replyName.isEmpty())
        {
            replyName = textValue(replyDetails.value("displayName"));
        }
        auto replyMessage = textValue(replyDetails.value("message"));
        if (replyMessage.isEmpty())
        {
            replyMessage = textValue(replyDetails.value("displayMessage"));
        }
        if (replyMessage.isEmpty())
        {
            replyMessage = textValue(replyDetails.value("messageText"));
        }
        if (replyMessage.isEmpty())
        {
            replyMessage = textValue(replyDetails.value("bodyText"));
        }
        if (replyMessage.isEmpty())
        {
            const auto textFallback = textValue(replyDetails.value("text"));
            const auto timestampText =
                textValue(replyDetails.value("timestampText"));
            static const QRegularExpression timestampOnlyPattern(
                R"(^\d{1,2}:\d{2}(?::\d{2})?$)");
            if (!textFallback.isEmpty() && textFallback != timestampText &&
                !timestampOnlyPattern.match(textFallback).hasMatch())
            {
                replyMessage = textFallback;
            }
        }
        static const QRegularExpression timestampOnlyPattern(
            R"(^\d{1,2}:\d{2}(?::\d{2})?$)");
        if (replyMessage.isEmpty() ||
            timestampOnlyPattern.match(replyMessage.trimmed()).hasMatch())
        {
            for (const auto *field : {"messageText", "displayMessage",
                                      "bodyText", "message", "content",
                                      "text"})
            {
                const auto candidate =
                    findStringByName(replyDetails,
                                     QString::fromLatin1(field))
                        .trimmed();
                if (!candidate.isEmpty() &&
                    !timestampOnlyPattern.match(candidate).hasMatch())
                {
                    replyMessage = candidate;
                    break;
                }
            }
        }
        const bool isSuperChatReply =
            replyRendererType == "liveChatPaidMessageRenderer" ||
            replyDetails.contains("purchaseAmountText") ||
            replyDetails.contains("amountDisplayString");
        if (isSuperChatReply)
        {
            snippet.insert("replyTargetIsSuperChat", true);
            if (timestampOnlyPattern.match(replyMessage.trimmed()).hasMatch())
            {
                replyMessage.clear();
            }
            auto replyAmount =
                textValue(replyDetails.value("purchaseAmountText"));
            if (replyAmount.isEmpty())
            {
                replyAmount =
                    textValue(replyDetails.value("amountDisplayString"));
            }
            if (!replyAmount.isEmpty())
            {
                snippet.insert("replyTargetAmount", replyAmount);
            }
            if (!replyAmount.isEmpty() &&
                !replyMessage.contains(replyAmount, Qt::CaseInsensitive))
            {
                replyMessage = replyMessage.isEmpty()
                                   ? replyAmount
                                   : replyAmount + ' ' + replyMessage;
            }
            else if (replyMessage.isEmpty())
            {
                replyMessage = "[Super Chat]";
            }
        }
        if (timestampOnlyPattern.match(replyMessage.trimmed()).hasMatch())
        {
            replyMessage.clear();
        }
        const auto parentMessageId =
            textValue(replyDetails.value("id")).isEmpty()
                ? textValue(replyDetails.value("messageId"))
                : textValue(replyDetails.value("id"));
        if (replyTargetId.isEmpty() && !parentMessageId.isEmpty())
        {
            snippet.insert("replyTargetId", parentMessageId);
        }
        if (!replyName.isEmpty())
        {
            snippet.insert("replyTargetName", replyName);
        }
        auto replyUserId =
            textValue(replyDetails.value("authorExternalChannelId"));
        if (replyUserId.isEmpty())
        {
            replyUserId = textValue(replyDetails.value("authorChannelId"));
        }
        if (!replyUserId.isEmpty())
        {
            snippet.insert("replyTargetUserId", replyUserId);
        }
        if (!replyMessage.isEmpty())
        {
            snippet.insert("replyTargetMessage", replyMessage);
        }
        auto replyNameColor =
            replyDetails.value("authorNameTextColor");
        if (replyNameColor.isUndefined())
        {
            replyNameColor = replyDetails.value("nameTextColor");
        }
        if (!replyNameColor.isUndefined())
        {
            snippet.insert("replyTargetNameColor", replyNameColor);
        }
    }
    if ((!replyTargetId.isEmpty() || !replyButtonData.isEmpty() ||
         !replyDetails.isEmpty()) &&
        !snippet.contains("replyTargetName"))
    {
        static const QRegularExpression leadingMentionPattern(
            R"(^\s*(@[^\s:]+))");
        const auto match = leadingMentionPattern.match(message);
        if (match.hasMatch())
        {
            snippet.insert("replyTargetName", match.captured(1));
        }
    }
    if (type == "textMessageEvent")
    {
        snippet.insert("textMessageDetails",
                       QJsonObject{{"messageText", message}});
    }
    else if (type == "superChatEvent")
    {
        snippet.insert(
            "superChatDetails",
            QJsonObject{
                {"amountDisplayString", amount},
                {"bodyBackgroundColor", renderer.value("bodyBackgroundColor")},
                {"authorNameTextColor", renderer.value("authorNameTextColor")},
            });
    }
    else if (type == "superStickerEvent")
    {
        // The sticker's original thumbnail set is preserved above so the
        // message renderer can show the artwork and its hover preview.
    }
    else if (type == "newSponsorEvent")
    {
        snippet.insert("newSponsorDetails",
                       QJsonObject{{"memberLevelName", memberLevel}});
    }
    else if (type == "memberMilestoneChatEvent")
    {
        snippet.insert(
            "memberMilestoneChatDetails",
            QJsonObject{{"memberLevelName", memberLevel},
                        {"memberMonth", memberMonth},
                        {"userComment", message}});
    }
    else if (type == "giftEvent")
    {
        snippet.insert(
            "giftDetails",
            QJsonObject{{"giftName", giftName},
                        {"giftUrl", giftUrl},
                        {"thumbnails", giftThumbnails},
                        {"jewelsAmount",
                         giftDetails.contains("jewelsAmount")
                             ? giftDetails.value("jewelsAmount")
                             : giftMetadata.value("jewelsAmount")},
                        {"comboCount",
                         giftDetails.contains("comboCount")
                             ? giftDetails.value("comboCount")
                             : giftMetadata.value("comboCount")},
                        {"altText",
                         giftDetails.contains("altText")
                             ? giftDetails.value("altText")
                             : giftMetadata.value("altText")}});
    }
    else if (type == "membershipGiftingEvent")
    {
        static const QRegularExpression giftCountPattern(
            R"((\d+)\s+(?:memberships?|gifts?))",
            QRegularExpression::CaseInsensitiveOption);
        const auto match = giftCountPattern.match(message);
        const auto giftedCount = match.hasMatch()
                                     ? match.captured(1).toInt()
                                     : renderer.value("giftMembershipsCount")
                                           .toInt();
        snippet.insert("membershipGiftingDetails",
                       QJsonObject{{"giftMembershipsCount", giftedCount},
                                   {"gifterName", authorName}});
    }
    else if (type == "pollEvent")
    {
        QJsonArray options;
        auto optionValues = renderer.value("options").toArray();
        if (optionValues.isEmpty())
        {
            optionValues = renderer.value("choices").toArray();
        }
        for (const auto &optionValue : optionValues)
        {
            const auto option = optionValue.toObject();
            const auto optionText = textValue(
                option.contains("text") ? option.value("text")
                                         : option.value("optionText"));
            if (optionText.isEmpty())
            {
                continue;
            }
            options.append(QJsonObject{
                {"optionText", optionText},
                {"tally", textValue(option.value("tally"))},
            });
        }
        snippet.insert("pollDetails",
                       QJsonObject{{"metadata",
                                    QJsonObject{{"questionText", message},
                                                {"options", options}}}});
    }

    QJsonObject author{
        {"channelId", authorChannelId},
        {"displayName", authorName},
        {"isChatOwner", isOwner},
        {"isChatModerator", isModerator},
        {"isChatSponsor", isMember},
        {"isVerified", isVerified},
        {"badges", normalizedAuthorBadges},
        {"nameTextColor", renderer.value("authorNameTextColor")},
        {"channelUrl", renderer.value("authorChannelUrl")},
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
    QJsonArray events;
    auto appendItem = [&events](const QJsonObject &item, QString action,
                                QString targetItemId = {}) {
        const auto normalized = normalizeChatItem(item);
        if (normalized.isEmpty())
        {
            return;
        }
        events.append(QJsonObject{
            {"action", std::move(action)},
            {"targetItemId", targetItemId},
            {"item", normalized},
        });
    };
    for (const auto &actionValue : liveChat.value("actions").toArray())
    {
        const auto action = actionValue.toObject();
        const auto addAction = action.value("addChatItemAction").toObject();
        if (!addAction.isEmpty())
        {
            appendItem(addAction.value("item").toObject(), "add");
            continue;
        }

        const auto replaceAction =
            action.value("replaceChatItemAction").toObject();
        if (!replaceAction.isEmpty())
        {
            appendItem(replaceAction.value("replacementItem").toObject(),
                       "replace",
                       replaceAction.value("targetItemId").toString());
            continue;
        }

        const auto removeAction = action.value("removeChatItemAction").toObject();
        if (!removeAction.isEmpty())
        {
            events.append(QJsonObject{
                {"action", "delete"},
                {"targetItemId", removeAction.value("targetItemId")},
            });
            continue;
        }

        if (action.contains("clearChatAction") ||
            action.contains("clearChatItemAction"))
        {
            events.append(QJsonObject{{"action", "clear"}});
            continue;
        }

        const auto deletedAction =
            action.value("markChatItemAsDeletedAction").toObject();
        if (!deletedAction.isEmpty())
        {
            events.append(QJsonObject{
                {"action", "delete"},
                {"targetItemId", deletedAction.value("targetItemId")},
                {"deletedStateMessage",
                 textValue(deletedAction.value("deletedStateMessage"))},
            });
            continue;
        }

        const auto authorDeletedAction =
            action.value("markChatItemsByAuthorAsDeletedAction").toObject();
        if (!authorDeletedAction.isEmpty())
        {
            auto authorChannelId =
                authorDeletedAction.value("externalChannelId");
            if (authorChannelId.isUndefined())
            {
                authorChannelId =
                    authorDeletedAction.value("targetUserChannelId");
            }
            events.append(QJsonObject{
                {"action", "deleteByAuthor"},
                {"authorChannelId", authorChannelId},
                {"deletedStateMessage",
                 textValue(authorDeletedAction.value("deletedStateMessage"))},
            });
            continue;
        }

        const auto removeByAuthorAction =
            action.value("removeChatItemByAuthorAction").toObject();
        if (!removeByAuthorAction.isEmpty())
        {
            auto authorChannelId =
                removeByAuthorAction.value("externalChannelId");
            if (authorChannelId.isUndefined())
            {
                authorChannelId =
                    removeByAuthorAction.value("targetUserChannelId");
            }
            events.append(QJsonObject{
                {"action", "deleteByAuthor"},
                {"authorChannelId", authorChannelId},
            });
            continue;
        }

        const auto bannerAction =
            action.value("addBannerToLiveChatCommand").toObject();
        if (!bannerAction.isEmpty())
        {
        const auto banner = bannerAction.value("bannerRenderer").toObject();
            auto bannerRenderer =
                banner.value("liveChatBannerRenderer").toObject();
            if (bannerRenderer.isEmpty())
            {
                bannerRenderer = banner;
            }
        const auto bannerContents = bannerRenderer.value("contents").toObject();
            if (!bannerContents.isEmpty())
            {
                appendItem(bannerContents, "banner",
                           bannerAction.value("actionId").toString());
            }
            continue;
        }

        const auto removeBannerAction =
            action.value("removeBannerForLiveChatCommand").toObject();
        if (!removeBannerAction.isEmpty())
        {
            events.append(QJsonObject{
                {"action", "delete"},
                {"targetItemId", removeBannerAction.value("targetActionId")},
            });
            continue;
        }

        if (action.contains("markChatEndedEvent"))
        {
            const QJsonObject endedMessage{
                {"id", "youtube-chat-ended"},
                {"snippet", QJsonObject{
                                {"type", "chatEndedEvent"},
                                {"displayMessage", "YouTube live chat ended."},
                            }},
                {"authorDetails", QJsonObject{
                                        {"displayName", "YouTube"},
                                    }},
            };
            events.append(QJsonObject{
                {"action", "add"},
                {"targetItemId", "youtube-chat-ended"},
                {"item", endedMessage},
            });
            continue;
        }

        const auto pollAction = action.value("updateLiveChatPollAction").toObject();
        if (!pollAction.isEmpty())
        {
            const auto poll = pollAction.value("pollRenderer").toObject();
            if (!poll.isEmpty())
            {
                appendItem(QJsonObject{{"liveChatPollRenderer", poll}},
                           "replace", "youtube-active-poll");
            }
            continue;
        }
    }
    return QJsonObject{{"events", events}};
}

}  // namespace

YouTubeInnertubeReader::VideoInfo YouTubeInnertubeReader::parseWatchPage(
    const QByteArray &html)
{
    const auto player = embeddedJsonObject(html, "ytInitialPlayerResponse");
    const auto initialData = embeddedJsonObject(html, "ytInitialData");
    const auto videoDetails = player.value("videoDetails").toObject();
    const auto metadata =
        findObjectByName(initialData, "channelMetadataRenderer");
    const auto microformat = player.value("microformat")
                                 .toObject()
                                 .value("playerMicroformatRenderer")
                                 .toObject();
    const auto liveDetails =
        microformat.value("liveBroadcastDetails").toObject();

    VideoInfo info;
    info.pageParsed = !videoDetails.isEmpty() || !metadata.isEmpty();
    info.videoId = videoDetails.value("videoId").toString();
    info.channelId = videoDetails.value("channelId").toString();
    if (info.channelId.isEmpty())
    {
        info.channelId = metadata.value("externalId").toString();
    }
    info.title = videoDetails.value("title").toString();
    info.channelTitle = videoDetails.value("author").toString();
    if (info.channelTitle.isEmpty())
    {
        info.channelTitle = metadata.value("title").toString();
    }
    const auto thumbnails = videoDetails.value("thumbnail")
                                .toObject()
                                .value("thumbnails")
                                .toArray();
    if (!thumbnails.isEmpty())
    {
        info.thumbnailUrl =
            thumbnails.last().toObject().value("url").toString();
    }
    info.actualStartTime = QDateTime::fromString(
        liveDetails.value("startTimestamp").toString(), Qt::ISODate);
    info.concurrentViewers = liveViewerCount(initialData);
    if (liveDetails.contains("isLiveNow"))
    {
        info.liveStateKnown = true;
        info.isLive = liveDetails.value("isLiveNow").toBool();
    }
    else
    {
        // Some YouTube /live pages embed player metadata in a script layout
        // that the general JSON extractor cannot associate with the player
        // response. Only accept an explicit liveBroadcastDetails flag here;
        // absence of the flag must remain "unknown", not "offline".
        static const QRegularExpression liveStateExpression(
            R"("liveBroadcastDetails"\s*:\s*\{\s*"isLiveNow"\s*:\s*(true|false))");
        const auto liveStateMatch = liveStateExpression.match(
            QString::fromUtf8(html));
        if (liveStateMatch.hasMatch())
        {
            info.liveStateKnown = true;
            info.isLive = liveStateMatch.captured(1) == "true";
        }
        else if (videoDetails.contains("isLive"))
        {
            info.liveStateKnown = true;
            info.isLive = videoDetails.value("isLive").toBool();
        }
        else
        {
            // A valid channel page with metadata but no current video player
            // is the normal response from the channel's /live route when it
            // has no active stream. Pages with a player but no live flag stay
            // unknown rather than being reported offline.
            info.liveStateKnown =
                videoDetails.isEmpty() && !metadata.isEmpty();
            info.isLive = false;
        }
    }
    return info;
}

QList<YouTubeInnertubeReader::ChannelSearchResult>
YouTubeInnertubeReader::parseChannelSearchPage(const QByteArray &html)
{
    const auto initialData = embeddedJsonObject(html, "ytInitialData");
    QList<ChannelSearchResult> results;
    QSet<QString> seenChannelIds;
    collectChannelSearchResults(initialData, results, seenChannelIds);
    return results;
}

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
    this->diagnosticsTimer_.setInterval(60000);
    QObject::connect(&this->diagnosticsTimer_, &QTimer::timeout, this,
                     [this] { this->logHeartbeat(); });
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
    this->diagnosticsTimer_.start();
    logYouTubeDiagnostic("reader_start",
                         QString("video_id=%1").arg(this->videoId_));
    this->loadWatchPage();
}

void YouTubeInnertubeReader::stop()
{
    if (this->stopped_)
    {
        return;
    }
    this->stopped_ = true;
    this->requestTimer_.stop();
    this->diagnosticsTimer_.stop();
    this->requestInFlight_ = false;
    logYouTubeDiagnostic(
        "reader_stop",
        QString("video_id=%1 requests=%2 successes=%3 failures=%4 events=%5")
            .arg(this->videoId_)
            .arg(this->diagnosticRequests_)
            .arg(this->diagnosticSuccesses_)
            .arg(this->diagnosticFailures_)
            .arg(this->diagnosticEvents_));
}

void YouTubeInnertubeReader::loadWatchPage()
{
    if (this->stopped_ || this->videoId_.isEmpty())
    {
        return;
    }

    ++this->diagnosticRequests_;
    this->requestInFlight_ = true;
    this->activeRequestElapsed_.start();

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
            this->requestInFlight_ = false;
            if (this->stopped_)
            {
                return;
            }
            ++this->diagnosticSuccesses_;
            this->lastRequestDurationMs_ =
                static_cast<int>(this->activeRequestElapsed_.elapsed());
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
            const auto info = parseWatchPage(html);
            const auto player =
                embeddedJsonObject(html, "ytInitialPlayerResponse");
            const auto initialData = embeddedJsonObject(html, "ytInitialData");

            int timeoutMs = 1000;
            this->continuation_ = findContinuation(initialData, &timeoutMs);
            const bool hasClientConfig = !this->apiKey_.isEmpty() &&
                                         !this->clientVersion_.isEmpty();
            logYouTubeDiagnostic(
                "watch_page_received",
                QString("video_id=%1 bytes=%2 config=%3 continuation=%4 "
                        "duration_ms=%5")
                    .arg(this->videoId_)
                    .arg(html.size())
                    .arg(hasClientConfig)
                    .arg(!this->continuation_.isEmpty())
                    .arg(this->lastRequestDurationMs_));
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
            this->requestInFlight_ = false;
            if (this->stopped_)
            {
                return;
            }
            ++this->diagnosticFailures_;
            this->lastRequestDurationMs_ =
                static_cast<int>(this->activeRequestElapsed_.elapsed());
            logYouTubeDiagnostic(
                "watch_page_failed",
                QString("video_id=%1 status=%2 duration_ms=%3")
                    .arg(this->videoId_)
                    .arg(result.status().value_or(0))
                    .arg(this->lastRequestDurationMs_));
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

    ++this->diagnosticRequests_;
    this->requestInFlight_ = true;
    this->activeRequestElapsed_.start();

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
            this->requestInFlight_ = false;
            if (this->stopped_)
            {
                return;
            }
            ++this->diagnosticSuccesses_;
            this->lastRequestDurationMs_ =
                static_cast<int>(this->activeRequestElapsed_.elapsed());
            const auto response = result.parseJson();
            const auto error = response.value("error").toObject();
            if (!error.isEmpty())
            {
                ++this->diagnosticFailures_;
                logYouTubeDiagnostic(
                    "chat_api_error",
                    QString("video_id=%1 status=%2 code=%3")
                        .arg(this->videoId_)
                        .arg(error.value("code").toInt())
                        .arg(error.value("status").toString()));
                this->reportError(error.value("message").toString());
                this->scheduleRetry(false, 5000);
                return;
            }

            int timeoutMs = 1000;
            const auto nextContinuation =
                findContinuation(response, &timeoutMs);
            if (nextContinuation.isEmpty())
            {
                ++this->diagnosticFailures_;
                logYouTubeDiagnostic(
                    "continuation_missing",
                    QString("video_id=%1 response_bytes=%2 duration_ms=%3")
                        .arg(this->videoId_)
                        .arg(result.getData().size())
                        .arg(this->lastRequestDurationMs_));
                this->reportError(
                    "YouTube did not return a continuation for live chat.");
                this->scheduleRetry(true, 5000);
                return;
            }

            const auto normalized = normalizeChatResponse(response);
            const auto events = normalized.value("events").toArray();
            this->diagnosticEvents_ +=
                static_cast<quint64>(events.size());
            const bool initialBatch = !this->receivedChatResponse_;
            if (!events.isEmpty() && this->messageCallback_)
            {
                logYouTubeDiagnostic(
                    "chat_batch",
                    QString("video_id=%1 event_count=%2 initial=%3")
                        .arg(this->videoId_)
                        .arg(events.size())
                        .arg(initialBatch));
                this->messageCallback_(normalized, initialBatch);
            }
            this->receivedChatResponse_ = true;
            this->continuation_ = nextContinuation;
            this->retryAttempt_ = 0;
            this->errorReported_ = false;
            // Bound the website's suggested wait so busy chats do not sit
            // idle for several seconds between continuation requests.
            this->requestTimer_.start(std::clamp(timeoutMs, 500, 1500));
        })
        .onError([this](const NetworkResult &result) {
            this->requestInFlight_ = false;
            if (this->stopped_)
            {
                return;
            }
            ++this->diagnosticFailures_;
            this->lastRequestDurationMs_ =
                static_cast<int>(this->activeRequestElapsed_.elapsed());
            const auto status = result.status().value_or(0);
            logYouTubeDiagnostic(
                "chat_request_failed",
                QString("video_id=%1 status=%2 duration_ms=%3")
                    .arg(this->videoId_)
                    .arg(status)
                    .arg(this->lastRequestDurationMs_));
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
    logYouTubeDiagnostic(
        "retry_scheduled",
        QString("video_id=%1 attempt=%2 delay_ms=%3 reload_page=%4")
            .arg(this->videoId_)
            .arg(this->retryAttempt_)
            .arg(delay)
            .arg(reloadPage));
    this->requestTimer_.start(delay);
}

void YouTubeInnertubeReader::logHeartbeat()
{
    logYouTubeDiagnostic(
        "reader_heartbeat",
        QString("video_id=%1 requests=%2 successes=%3 failures=%4 events=%5 "
                "retry_attempt=%6 in_flight=%7 continuation=%8 last_ms=%9")
            .arg(this->videoId_)
            .arg(this->diagnosticRequests_)
            .arg(this->diagnosticSuccesses_)
            .arg(this->diagnosticFailures_)
            .arg(this->diagnosticEvents_)
            .arg(this->retryAttempt_)
            .arg(this->requestInFlight_)
            .arg(!this->continuation_.isEmpty())
            .arg(this->lastRequestDurationMs_));
    this->diagnosticRequests_ = 0;
    this->diagnosticSuccesses_ = 0;
    this->diagnosticFailures_ = 0;
    this->diagnosticEvents_ = 0;
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
