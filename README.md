![chatterinoLogo](https://user-images.githubusercontent.com/41973452/272541622-52457e89-5f16-4c83-93e7-91866c25b606.png)
Chatterino 2 [![GitHub Actions Build (Windows, Ubuntu, MacOS)](https://github.com/Chatterino/chatterino2/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/Chatterino/chatterino2/actions?query=workflow%3ABuild+branch%3Amaster) [![Build FreeBSD](https://github.com/Chatterino/chatterino2/actions/workflows/build-freebsd.yml/badge.svg)](https://github.com/Chatterino/chatterino2/actions/workflows/build-freebsd.yml) [![Chocolatey Package](https://img.shields.io/chocolatey/v/chatterino?include_prereleases)](https://chocolatey.org/packages/chatterino) [![Flatpak Package](https://img.shields.io/flathub/v/com.chatterino.chatterino)](https://flathub.org/apps/details/com.chatterino.chatterino)
============

Chatterino 2 is a chat client for [Twitch.tv](https://twitch.tv).
The Chatterino 2 wiki can be found [here](https://wiki.chatterino.com).
Contribution guidelines can be found [here](https://wiki.chatterino.com/Contributing%20for%20Developers).

## YouTube live chat in this fork

The `youtube-innertube-preview` branch adds experimental YouTube live-chat support to this Chatterino fork. The upstream Chatterino downloads and the upstream repository do not include these changes.

### Features

- Read public live chat from a YouTube video URL or video ID using YouTube's Innertube live-chat endpoint. Chat reading does not poll the YouTube Data API, so it avoids spending Data API quota on each chat refresh.
- Show YouTube chat badges, custom and member-only emotes, Super Chats and Super Stickers with their amounts, membership and gift announcements, and reply markers when YouTube provides reply metadata.
- Show stream details in the chat header, including the title and live thumbnail on hover when available.
- Sign in with Google OAuth to send messages and use supported moderation actions.

Innertube is an undocumented YouTube interface and may change. OAuth-backed actions and API-based channel lookups still use the YouTube Data API and count toward its quota. A Data API key is optional when opening a direct public video link; it can be used for channel-name lookup while signed out.

### YouTube setup

1. In Google Cloud, enable YouTube Data API v3 for the project used by your OAuth client.
2. Create a Google OAuth client with the **Desktop app** application type. If the OAuth consent screen is in testing mode, add the Google account you plan to use as a test user.
3. In Chatterino, open **Settings → Accounts**, enter the OAuth client ID (and client secret if your client configuration requires it), then choose **Sign in to YouTube**.
4. Paste a YouTube live-video URL or video ID into Chatterino to open its chat. You do not need a Data API key just to read chat from a direct video link.

This repository is private. Anyone cloning it must first be granted access to the GitHub repository.

### Use this fork with an existing Chatterino install

You cannot add the YouTube features to an existing upstream Chatterino installation with a setting or plugin. They are compiled into this fork, so you need to build and run a version from the `youtube-innertube-preview` branch. You do not need to uninstall the official app: keep the fork in a separate folder and run its build there.

On Windows, follow the build steps below and the [standalone packaging instructions](BUILDING_ON_WINDOWS.md#producing-standalone-builds). Copy the complete packaged output, including its Qt runtime files, into a separate folder; copying only `chatterino.exe` is not enough. Then launch `chatterino.exe` from that folder.

By default, the fork uses the same Chatterino settings profile as the official install. To keep the fork's settings and cache separate, create a file named `modes` beside its executable and put this single line in it:

```text
portable
```

Replacing the official installation is possible, but back up the installation and your settings first, and replace the complete packaged output rather than only the executable. With the updater enabled (the default), this fork still checks the upstream Chatterino update service, so an upstream update may replace the fork's YouTube build. Running it in a separate portable folder is the safer option.

## Download

Current releases are available at [https://chatterino.com](https://chatterino.com).
Windows users can also install Chatterino [from Chocolatey](https://chocolatey.org/packages/chatterino).
These upstream downloads do not include this fork's YouTube changes.

## Nightly build

You can download the latest Chatterino 2 build over [here](https://github.com/Chatterino/chatterino2/releases/tag/nightly-build)

You might also need to install the [VC++ Redistributables](https://aka.ms/vs/17/release/vc_redist.x64.exe) from Microsoft if you do not have it installed already.  
If you still receive an error about `MSVCR120.dll missing`, then you should install the [VC++ 2013 Restributable](https://download.microsoft.com/download/2/E/6/2E61CFA4-993B-4DD4-91DA-3737CD5CD6E3/vcredist_x64.exe).

## Building

To build this fork with its required submodules, clone the `youtube-innertube-preview` branch:

```shell
git clone --branch youtube-innertube-preview --recurse-submodules https://github.com/vibeCoderGalaxyDestroyer/Chatterino-YouTube.git
```

or

```shell
git clone --branch youtube-innertube-preview https://github.com/vibeCoderGalaxyDestroyer/Chatterino-YouTube.git
cd Chatterino-YouTube
git submodule update --init --recursive
```

- [Building on Windows](../master/BUILDING_ON_WINDOWS.md)
- [Building on Windows with vcpkg](../master/BUILDING_ON_WINDOWS_WITH_VCPKG.md)
- [Building on Linux](../master/BUILDING_ON_LINUX.md)
- [Building on macOS](../master/BUILDING_ON_MAC.md)
- [Building on FreeBSD](../master/BUILDING_ON_FREEBSD.md)

## Git blame

This project has big commits in the history which touch most files while only doing stylistic changes. To improve the output of git-blame, consider setting:

```shell
git config blame.ignoreRevsFile .git-blame-ignore-revs
```

This will ignore all revisions mentioned in the [`.git-blame-ignore-revs`
file](./.git-blame-ignore-revs). GitHub does this by default.

## Code style

The code is formatted using [clang-format](https://clang.llvm.org/docs/ClangFormat.html). Our configuration is found in the [.clang-format](.clang-format) file in the repository root directory.

For more contribution guidelines, take a look at [the wiki](https://wiki.chatterino.com/Contributing%20for%20Developers/).

## Doxygen

Doxygen is used to generate project information daily and is available [here](https://doxygen.chatterino.com).
