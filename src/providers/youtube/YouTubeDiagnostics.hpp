// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino {

void initializeYouTubeDiagnostics(const QString &logDirectory);
void logYouTubeDiagnostic(const QString &event, const QString &details = {});

}  // namespace chatterino
