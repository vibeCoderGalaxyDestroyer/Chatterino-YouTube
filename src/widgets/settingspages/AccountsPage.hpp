// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

class QHideEvent;
class QStackedWidget;

namespace chatterino {

class AccountSwitchWidget;

class AccountsPage : public SettingsPage
{
public:
    AccountsPage();

    void hideYouTubeCredentials();

protected:
    void hideEvent(QHideEvent *event) override;

private:
    QStackedWidget *youtubeCredentialsStack_{};
};

}  // namespace chatterino
