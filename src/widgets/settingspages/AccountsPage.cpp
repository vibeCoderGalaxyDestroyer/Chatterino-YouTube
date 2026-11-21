// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/AccountsPage.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/accounts/AccountModel.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/youtube/YouTubeProvider.hpp"
#include "singletons/Settings.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/dialogs/LoginDialog.hpp"
#include "widgets/helper/EditableModelView.hpp"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

AccountsPage::AccountsPage()
{
    auto *app = getApp();

    LayoutCreator<AccountsPage> layoutCreator(this);
    auto layout = layoutCreator.emplace<QVBoxLayout>().withoutMargin();

    layout.emplace<QLabel>(
        "YouTube API key (for channel lookup; optional for video links)");
    auto *youtubeApiKey = layout.emplace<QLineEdit>().getElement();
    youtubeApiKey->setEchoMode(QLineEdit::Password);
    youtubeApiKey->setText(getSettings()->youtubeApiKey.getValue());
    QObject::connect(youtubeApiKey, &QLineEdit::textChanged,
                     [](const QString &value) {
                         getSettings()->youtubeApiKey.setValue(value);
                     });

    layout.emplace<QLabel>("Google Desktop OAuth client ID (for sign-in)");
    auto *youtubeClientId = layout.emplace<QLineEdit>().getElement();
    youtubeClientId->setText(getSettings()->youtubeOAuthClientId.getValue());
    QObject::connect(youtubeClientId, &QLineEdit::textChanged,
                     [](const QString &value) {
                         getSettings()->youtubeOAuthClientId.setValue(value);
                     });

    layout.emplace<QLabel>("Google OAuth client secret (if required)");
    auto *youtubeClientSecret = layout.emplace<QLineEdit>().getElement();
    youtubeClientSecret->setEchoMode(QLineEdit::Password);
    youtubeClientSecret->setText(
        getSettings()->youtubeOAuthClientSecret.getValue());
    QObject::connect(youtubeClientSecret, &QLineEdit::textChanged,
                     [](const QString &value) {
                         getSettings()->youtubeOAuthClientSecret.setValue(value);
                     });

    auto *youtubeStatus = layout.emplace<QLabel>().getElement();
    youtubeStatus->setWordWrap(true);
    const auto updateYouTubeStatus = [status = QPointer<QLabel>(youtubeStatus),
                                      provider = app->getYouTube()] {
        if (status)
        {
            if (provider->isLoggedIn())
            {
                status->setText("YouTube: signed in");
            }
            else if (!provider->authError().isEmpty())
            {
                status->setText("YouTube sign-in failed: " +
                                provider->authError());
            }
            else
            {
                status->setText("YouTube: signed out");
            }
        }
    };
    updateYouTubeStatus();
    std::ignore = app->getYouTube()->authChanged.connect(updateYouTubeStatus);

    auto *youtubeSignIn =
        layout.emplace<QPushButton>("Sign in to YouTube").getElement();
    QObject::connect(youtubeSignIn, &QPushButton::clicked, this, [this, app] {
        QString error;
        if (!app->getYouTube()->startLogin(&error))
        {
            QMessageBox::warning(this, "YouTube sign-in", error);
        }
    });
    auto *youtubeSignOut =
        layout.emplace<QPushButton>("Sign out of YouTube").getElement();
    QObject::connect(youtubeSignOut, &QPushButton::clicked, this, [app] {
        app->getYouTube()->logout();
    });

    EditableModelView *view =
        layout
            .emplace<EditableModelView>(
                app->getAccounts()->createModel(nullptr), false)
            .getElement();

    view->getTableView()->horizontalHeader()->setVisible(false);
    view->getTableView()->horizontalHeader()->setStretchLastSection(true);

    // We can safely ignore this signal connection since we own the view
    std::ignore = view->addButtonPressed.connect([this] {
        LoginDialog d(this);
        d.exec();
    });

    view->getTableView()->setStyleSheet("background: #333");

    //    auto buttons = layout.emplace<QDialogButtonBox>();
    //    {
    //        this->addButton = buttons->addButton("Add",
    //        QDialogButtonBox::YesRole); this->removeButton =
    //        buttons->addButton("Remove", QDialogButtonBox::NoRole);
    //    }

    //    layout.emplace<AccountSwitchWidget>(this).assign(&this->accSwitchWidget);

    // ----
    //    QObject::connect(this->addButton, &QPushButton::clicked, []() {
    //        static auto loginWidget = new LoginWidget();
    //        loginWidget->show();
    //    });

    //    QObject::connect(this->removeButton, &QPushButton::clicked, [this] {
    //        auto selectedUser = this->accSwitchWidget->currentItem()->text();
    //        if (selectedUser == ANONYMOUS_USERNAME_LABEL) {
    //            // Do nothing
    //            return;
    //        }

    //        getApp()->getAccounts()->Twitch.removeUser(selectedUser);
    //    });
}

}  // namespace chatterino
