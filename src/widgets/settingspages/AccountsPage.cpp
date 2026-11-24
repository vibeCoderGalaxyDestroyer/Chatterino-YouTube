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
#include <QFrame>
#include <QGroupBox>
#include <QHideEvent>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

AccountsPage::AccountsPage()
{
    auto *app = getApp();

    LayoutCreator<AccountsPage> layoutCreator(this);
    auto layout = layoutCreator.emplace<QVBoxLayout>().withoutMargin();

    auto *youtubeCredentials =
        layout.emplace<QGroupBox>("YouTube credentials").getElement();
    auto *youtubeCredentialsLayout = new QVBoxLayout(youtubeCredentials);
    this->youtubeCredentialsStack_ =
        new QStackedWidget(youtubeCredentials);
    youtubeCredentialsLayout->addWidget(this->youtubeCredentialsStack_);

    auto *hiddenPanel = new QFrame(this->youtubeCredentialsStack_);
    hiddenPanel->setObjectName("youtubeCredentialsHiddenPanel");
    hiddenPanel->setStyleSheet(
        "QFrame#youtubeCredentialsHiddenPanel {"
        "background-color: palette(mid);"
        "border: 1px solid palette(midlight);"
        "border-radius: 4px;"
        "}");
    auto *hiddenPanelLayout = new QVBoxLayout(hiddenPanel);
    hiddenPanelLayout->addStretch();
    auto *hiddenLabel =
        new QLabel("YouTube credentials are hidden", hiddenPanel);
    hiddenLabel->setAlignment(Qt::AlignCenter);
    hiddenPanelLayout->addWidget(hiddenLabel);
    auto *revealButton =
        new QPushButton("Reveal credentials", hiddenPanel);
    hiddenPanelLayout->addWidget(revealButton, 0, Qt::AlignCenter);
    hiddenPanelLayout->addStretch();
    this->youtubeCredentialsStack_->addWidget(hiddenPanel);

    auto *credentialPanel = new QWidget(this->youtubeCredentialsStack_);
    auto *credentialPanelLayout = new QVBoxLayout(credentialPanel);

    credentialPanelLayout->addWidget(new QLabel(
        "YouTube API key (for channel lookup; optional for video links)",
        credentialPanel));
    auto *youtubeApiKey = new QLineEdit(credentialPanel);
    youtubeApiKey->setText(getSettings()->youtubeApiKey.getValue());
    credentialPanelLayout->addWidget(youtubeApiKey);
    QObject::connect(youtubeApiKey, &QLineEdit::textChanged,
                     [](const QString &value) {
                         getSettings()->youtubeApiKey.setValue(value);
                     });

    credentialPanelLayout->addWidget(
        new QLabel("Google Desktop OAuth client ID (for sign-in)",
                   credentialPanel));
    auto *youtubeClientId = new QLineEdit(credentialPanel);
    youtubeClientId->setText(getSettings()->youtubeOAuthClientId.getValue());
    credentialPanelLayout->addWidget(youtubeClientId);
    QObject::connect(youtubeClientId, &QLineEdit::textChanged,
                     [](const QString &value) {
                         getSettings()->youtubeOAuthClientId.setValue(value);
                     });

    credentialPanelLayout->addWidget(
        new QLabel("Google OAuth client secret (if required)",
                   credentialPanel));
    auto *youtubeClientSecret = new QLineEdit(credentialPanel);
    youtubeClientSecret->setText(
        getSettings()->youtubeOAuthClientSecret.getValue());
    credentialPanelLayout->addWidget(youtubeClientSecret);
    QObject::connect(youtubeClientSecret, &QLineEdit::textChanged,
                     [](const QString &value) {
                         getSettings()->youtubeOAuthClientSecret.setValue(value);
                     });

    auto *hideButton = new QPushButton("Hide credentials", credentialPanel);
    credentialPanelLayout->addWidget(hideButton, 0, Qt::AlignRight);
    this->youtubeCredentialsStack_->addWidget(credentialPanel);

    QObject::connect(revealButton, &QPushButton::clicked, this, [this] {
        this->youtubeCredentialsStack_->setCurrentIndex(1);
    });
    QObject::connect(hideButton, &QPushButton::clicked, this,
                     [this] { this->hideYouTubeCredentials(); });

    auto *youtubeStatus = layout.emplace<QLabel>().getElement();
    youtubeStatus->setWordWrap(true);
    const auto updateYouTubeStatus = [status = QPointer<QLabel>(youtubeStatus),
                                      provider = app->getYouTube()] {
        if (status)
        {
            if (provider->isLoggedIn())
            {
                const auto accountName = provider->accountName();
                status->setText(
                    accountName.isEmpty()
                        ? QString("YouTube: signed in")
                        : QString("YouTube: signed in as %1").arg(accountName));
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

void AccountsPage::hideYouTubeCredentials()
{
    if (this->youtubeCredentialsStack_)
    {
        this->youtubeCredentialsStack_->setCurrentIndex(0);
    }
}

void AccountsPage::hideEvent(QHideEvent *event)
{
    this->hideYouTubeCredentials();
    SettingsPage::hideEvent(event);
}

}  // namespace chatterino
