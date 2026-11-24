// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeChannel.hpp"

#include "providers/youtube/YouTubeInnertubeReader.hpp"
#include "providers/youtube/YouTubeProvider.hpp"

#include <QObject>
#include <utility>

namespace chatterino {

YouTubeChannel::YouTubeChannel(QString input, bool channelLookup,
                               YouTubeProvider *provider)
    : Channel(input, Type::YouTube)
    , lookupInput_(input)
    , provider_(provider)
    , channelLookup_(channelLookup)
{
    this->liveStatusTimer_.setInterval(5 * 60 * 1000);
    QObject::connect(&this->liveStatusTimer_, &QTimer::timeout,
                     [this] { this->provider_->checkLiveStatus(this); });
    if (!channelLookup)
    {
        this->videoId_ = std::move(input);
    }
}

YouTubeChannel::~YouTubeChannel()
{
    this->stopStream();
}

bool YouTubeChannel::canSendMessage() const
{
    return this->provider_->isLoggedIn() && !this->liveChatId_.isEmpty();
}

void YouTubeChannel::sendMessage(const QString &message)
{
    this->provider_->sendMessage(this, message);
}

bool YouTubeChannel::isLive() const
{
    return this->channelIsLive_ || this->chatAvailable_ ||
           !this->liveChatId_.isEmpty();
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

bool YouTubeChannel::hasKnownLiveStatus() const
{
    return this->channelStatusKnown_;
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
    this->replyTargets_.clear();
    this->chatAvailable_ = false;
    this->liveChatId_.clear();
    this->channelStatusKnown_ = false;
    this->channelIsLive_ = false;
    this->offlineNoticeSent_ = false;
    this->statusRequestInFlight_ = false;
    ++this->liveStatusRequestId_;
    if (this->channelLookup_)
    {
        this->videoId_.clear();
    }
    this->provider_->loadVideo(this);
}

void YouTubeChannel::stopStream()
{
    this->liveStatusTimer_.stop();
    this->statusRequestInFlight_ = false;
    ++this->liveStatusRequestId_;
    if (this->reader_)
    {
        auto reader = std::move(this->reader_);
        reader->stop();
    }
    this->chatAvailable_ = false;
}

}  // namespace chatterino
