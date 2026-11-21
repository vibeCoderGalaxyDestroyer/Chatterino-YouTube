// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeChannel.hpp"

#include "providers/youtube/YouTubeLiveChatStream.hpp"
#include "providers/youtube/YouTubeProvider.hpp"

#include <utility>

namespace chatterino {

YouTubeChannel::YouTubeChannel(QString input, bool channelLookup,
                               YouTubeProvider *provider)
    : Channel(input, Type::YouTube)
    , lookupInput_(input)
    , provider_(provider)
    , channelLookup_(channelLookup)
{
    if (!channelLookup)
    {
        this->videoId_ = std::move(input);
    }
    this->reconnectTimer_.setSingleShot(true);
    QObject::connect(&this->reconnectTimer_, &QTimer::timeout, [this] {
        this->provider_->loadMessages(this);
    });
}

YouTubeChannel::~YouTubeChannel()
{
    this->stopStream();
}

bool YouTubeChannel::canSendMessage() const
{
    return this->provider_->isLoggedIn();
}

void YouTubeChannel::sendMessage(const QString &message)
{
    this->provider_->sendMessage(this, message);
}

bool YouTubeChannel::isLive() const
{
    return !this->liveChatId_.isEmpty();
}

bool YouTubeChannel::canReconnect() const
{
    return true;
}

void YouTubeChannel::reconnect()
{
    this->start();
}

const QString &YouTubeChannel::getDisplayName() const
{
    if (!this->channelTitle_.isEmpty())
    {
        return this->channelTitle_;
    }
    static const QString pendingDisplayName = "YouTube live chat";
    return pendingDisplayName;
}

const QString &YouTubeChannel::getLocalizedName() const
{
    return this->getDisplayName();
}

const QString &YouTubeChannel::videoId() const
{
    return this->videoId_;
}

const QString &YouTubeChannel::liveChatId() const
{
    return this->liveChatId_;
}

const QString &YouTubeChannel::videoTitle() const
{
    return this->videoTitle_;
}

const QString &YouTubeChannel::thumbnailUrl() const
{
    return this->thumbnailUrl_;
}

const QDateTime &YouTubeChannel::actualStartTime() const
{
    return this->actualStartTime_;
}

qint64 YouTubeChannel::concurrentViewers() const
{
    return this->concurrentViewers_;
}

bool YouTubeChannel::isChannelLookup() const
{
    return this->channelLookup_;
}

const QString &YouTubeChannel::lookupInput() const
{
    return this->lookupInput_;
}

void YouTubeChannel::deleteMessage(const QString &messageId)
{
    this->provider_->deleteMessage(this, messageId);
}

void YouTubeChannel::banUser(const QString &userId, int durationSeconds)
{
    this->provider_->banUser(this, userId, durationSeconds);
}

void YouTubeChannel::createPoll(const QString &question,
                                const QStringList &options)
{
    this->provider_->createPoll(this, question, options);
}

void YouTubeChannel::start()
{
    this->stopStream();
    this->quotaPaused_ = false;
    this->reconnectAttempt_ = 0;
    this->liveChatId_.clear();
    if (this->channelLookup_)
    {
        this->videoId_.clear();
    }
    this->provider_->loadVideo(this);
}

void YouTubeChannel::stopStream()
{
    ++this->streamGeneration_;
    this->reconnectTimer_.stop();
    if (this->stream_)
    {
        auto stream = std::move(this->stream_);
        stream->stop();
    }
}

}  // namespace chatterino
