// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeProvider.hpp"

#include <gtest/gtest.h>

namespace chatterino {

TEST(YouTubeProvider, acceptsVideoAndChannelInputs)
{
    EXPECT_TRUE(YouTubeProvider::isValidInput("dQw4w9WgXcQ"));
    EXPECT_TRUE(YouTubeProvider::isValidInput(
        "https://www.youtube.com/live/dQw4w9WgXcQ"));
    EXPECT_TRUE(YouTubeProvider::isValidInput("Mr Beast"));
    EXPECT_TRUE(YouTubeProvider::isValidInput("@MrBeast"));
    EXPECT_TRUE(
        YouTubeProvider::isValidInput("https://www.youtube.com/@MrBeast"));
    EXPECT_TRUE(YouTubeProvider::isValidInput(
        "https://youtube.com/channel/UC1234567890123456789012"));
}

TEST(YouTubeProvider, rejectsNonYouTubeUrlsAsChannelInputs)
{
    EXPECT_FALSE(YouTubeProvider::isValidInput("https://example.com/channel"));
    EXPECT_FALSE(YouTubeProvider::isValidInput(""));
}

}  // namespace chatterino
