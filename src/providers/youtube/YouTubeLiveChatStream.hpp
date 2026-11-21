// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace grpc {
class ClientContext;
}  // namespace grpc

namespace chatterino {

class YouTubeLiveChatStream final
{
public:
    using MessageCallback =
        std::function<void(QJsonObject, QString, QString)>;
    using FinishedCallback =
        std::function<void(int, QString, QString, QString, bool, qint64)>;

    YouTubeLiveChatStream(QString liveChatId, QString pageToken,
                          QString apiKey, QString accessToken,
                          MessageCallback messageCallback,
                          FinishedCallback finishedCallback);
    ~YouTubeLiveChatStream();

    YouTubeLiveChatStream(const YouTubeLiveChatStream &) = delete;
    YouTubeLiveChatStream &operator=(const YouTubeLiveChatStream &) = delete;

    void start();
    void stop();

private:
    QString liveChatId_;
    QString pageToken_;
    QString apiKey_;
    QString accessToken_;
    MessageCallback messageCallback_;
    FinishedCallback finishedCallback_;
    std::shared_ptr<grpc::ClientContext> context_;
    std::mutex contextMutex_;
    std::atomic_bool stopRequested_{false};
    std::thread thread_;

    void run();
};

}  // namespace chatterino
