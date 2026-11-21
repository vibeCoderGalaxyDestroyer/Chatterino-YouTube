// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeLiveChatStream.hpp"

#include "stream_list.grpc.pb.h"

#include <google/protobuf/util/json_util.h>
#include <grpcpp/grpcpp.h>
#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>

#include <chrono>
#include <utility>

namespace chatterino {

namespace {

void traceYouTubeStream(const QString &message)
{
    if (!qEnvironmentVariableIsSet("CHATTERINO_YOUTUBE_TRACE"))
    {
        return;
    }

    qInfo().noquote() << QDateTime::currentDateTime().toString(
                             Qt::ISODateWithMs)
                      << "[YouTube gRPC]" << message;
}

QString eventType(
    youtube::api::v3::LiveChatMessageSnippet::TypeWrapper::Type type)
{
    using TypeWrapper = youtube::api::v3::LiveChatMessageSnippet::TypeWrapper;
    switch (type)
    {
        case TypeWrapper::TEXT_MESSAGE_EVENT:
            return "textMessageEvent";
        case TypeWrapper::FAN_FUNDING_EVENT:
            return "fanFundingEvent";
        case TypeWrapper::CHAT_ENDED_EVENT:
            return "chatEndedEvent";
        case TypeWrapper::SPONSOR_ONLY_MODE_STARTED_EVENT:
            return "sponsorOnlyModeStartedEvent";
        case TypeWrapper::SPONSOR_ONLY_MODE_ENDED_EVENT:
            return "sponsorOnlyModeEndedEvent";
        case TypeWrapper::NEW_SPONSOR_EVENT:
            return "newSponsorEvent";
        case TypeWrapper::USER_BANNED_EVENT:
            return "userBannedEvent";
        case TypeWrapper::MEMBER_MILESTONE_CHAT_EVENT:
            return "memberMilestoneChatEvent";
        case TypeWrapper::MEMBERSHIP_GIFTING_EVENT:
            return "membershipGiftingEvent";
        case TypeWrapper::GIFT_MEMBERSHIP_RECEIVED_EVENT:
            return "giftMembershipReceivedEvent";
        case TypeWrapper::SUPER_CHAT_EVENT:
            return "superChatEvent";
        case TypeWrapper::SUPER_STICKER_EVENT:
            return "superStickerEvent";
        case TypeWrapper::POLL_EVENT:
            return "pollEvent";
        case TypeWrapper::GIFT_EVENT:
            return "giftEvent";
        case TypeWrapper::INVALID_TYPE:
            return {};
    }
    return {};
}

}  // namespace

namespace {

std::shared_ptr<grpc::Channel> youtubeGrpcChannel()
{
    static const auto channel = grpc::CreateChannel(
        "dns:///youtube.googleapis.com:443", grpc::SslCredentials({}));
    return channel;
}

}  // namespace

YouTubeLiveChatStream::YouTubeLiveChatStream(QString liveChatId,
                                             QString pageToken, QString apiKey,
                                             QString accessToken,
                                             MessageCallback messageCallback,
                                             FinishedCallback finishedCallback)
    : liveChatId_(std::move(liveChatId))
    , pageToken_(std::move(pageToken))
    , apiKey_(std::move(apiKey))
    , accessToken_(std::move(accessToken))
    , messageCallback_(std::move(messageCallback))
    , finishedCallback_(std::move(finishedCallback))
{
}

YouTubeLiveChatStream::~YouTubeLiveChatStream()
{
    this->stop();
}

void YouTubeLiveChatStream::start()
{
    if (this->thread_.joinable())
    {
        return;
    }
    this->stopRequested_.store(false);
    this->thread_ = std::thread([this] {
        this->run();
    });
}

void YouTubeLiveChatStream::stop()
{
    this->stopRequested_.store(true);
    {
        std::lock_guard lock(this->contextMutex_);
        if (this->context_)
        {
            this->context_->TryCancel();
        }
    }
    if (this->thread_.joinable())
    {
        this->thread_.join();
    }
}

void YouTubeLiveChatStream::run()
{
    const auto channel = youtubeGrpcChannel();
    auto stub =
        youtube::api::v3::V3DataLiveChatMessageService::NewStub(channel);
    auto context = std::make_shared<grpc::ClientContext>();
    {
        std::lock_guard lock(this->contextMutex_);
        this->context_ = context;
        if (this->stopRequested_.load())
        {
            context->TryCancel();
        }
    }

    if (!this->apiKey_.isEmpty())
    {
        context->AddMetadata("x-goog-api-key", this->apiKey_.toStdString());
    }
    if (!this->accessToken_.isEmpty())
    {
        context->AddMetadata("authorization",
                             "Bearer " + this->accessToken_.toStdString());
    }

    youtube::api::v3::LiveChatMessageListRequest request;
    request.set_live_chat_id(this->liveChatId_.toStdString());
    request.add_part("snippet");
    request.add_part("authorDetails");
    if (!this->pageToken_.isEmpty())
    {
        request.set_page_token(this->pageToken_.toStdString());
    }

    QString lastPageToken = this->pageToken_;
    QString offlineAt;
    const auto streamStartedAt = std::chrono::steady_clock::now();
    auto lastResponseAt = streamStartedAt;
    int responseCount = 0;
    int totalItems = 0;
    auto reader = stub->StreamList(context.get(), request);
    traceYouTubeStream(
        QString("RPC started; liveChatId=%1; resumed=%2")
            .arg(this->liveChatId_, this->pageToken_.isEmpty() ? "no" : "yes"));
    youtube::api::v3::LiveChatMessageListResponse response;
    while (reader->Read(&response))
    {
        const auto responseAt = std::chrono::steady_clock::now();
        const auto responseGapMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                responseAt - lastResponseAt)
                .count();
        lastResponseAt = responseAt;
        ++responseCount;
        totalItems += response.items_size();
        traceYouTubeStream(
            QString("response=%1; items=%2; "
                    "has_page_token=%3; gap_ms=%4")
                .arg(responseCount)
                .arg(response.items_size())
                .arg(response.has_next_page_token() ? "yes" : "no")
                .arg(responseGapMs));

        if (response.has_next_page_token())
        {
            lastPageToken = QString::fromStdString(response.next_page_token());
        }
        if (response.has_offline_at())
        {
            offlineAt = QString::fromStdString(response.offline_at());
        }

        std::string json;
        const auto jsonStatus =
            google::protobuf::util::MessageToJsonString(response, &json);
        if (!jsonStatus.ok())
        {
            continue;
        }

        auto root =
            QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
        auto items = root.value("items").toArray();
        for (int index = 0;
             index < items.size() && index < response.items_size(); ++index)
        {
            auto item = items.at(index).toObject();
            auto snippet = item.value("snippet").toObject();
            const auto &protoSnippet = response.items(index).snippet();
            if (protoSnippet.has_type())
            {
                snippet.insert("type", eventType(protoSnippet.type()));
            }
            item.insert("snippet", snippet);
            items.replace(index, item);
        }
        root.insert("items", items);

        if (this->messageCallback_)
        {
            this->messageCallback_(
                root, lastPageToken,
                QString::fromStdString(response.offline_at()));
        }
    }

    const auto status = reader->Finish();
    const auto connectionDurationMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - streamStartedAt)
            .count();
    traceYouTubeStream(
        QString("RPC finished; code=%1; duration_ms=%2; responses=%3; "
                "items=%4; canceled=%5; error=%6")
            .arg(static_cast<int>(status.error_code()))
            .arg(connectionDurationMs)
            .arg(responseCount)
            .arg(totalItems)
            .arg(this->stopRequested_.load() ? "yes" : "no")
            .arg(QString::fromStdString(status.error_message())));
    if (this->finishedCallback_)
    {
        this->finishedCallback_(
            static_cast<int>(status.error_code()),
            QString::fromStdString(status.error_message()), lastPageToken,
            offlineAt, this->stopRequested_.load(), connectionDurationMs);
    }
    {
        std::lock_guard lock(this->contextMutex_);
        this->context_.reset();
    }
}

}  // namespace chatterino
