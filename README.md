<p align="center">
  <img src="resources/icon.svg" alt="Chatterino logo" width="88" height="88">
  &nbsp;&nbsp;
  <img src="docs/resources/youtube.svg" alt="YouTube logo" width="88" height="88">
</p>

<h1 align="center">Chatterino-YouTube</h1>

<p align="center">
  <a href="https://github.com/vibeCoderGalaxyDestroyer/Chatterino-YouTube/actions?query=workflow%3ABuild+branch%3Ayoutube-innertube-preview"><img alt="Build status" src="https://github.com/vibeCoderGalaxyDestroyer/Chatterino-YouTube/actions/workflows/build.yml/badge.svg?branch=youtube-innertube-preview"></a>
  <a href="https://github.com/vibeCoderGalaxyDestroyer/Chatterino-YouTube/actions?query=workflow%3ABuild+branch%3Ayoutube-innertube-preview"><img alt="FreeBSD build status" src="https://github.com/vibeCoderGalaxyDestroyer/Chatterino-YouTube/actions/workflows/build-freebsd.yml/badge.svg?branch=youtube-innertube-preview"></a>
  <a href="https://github.com/vibeCoderGalaxyDestroyer/Chatterino-YouTube/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/vibeCoderGalaxyDestroyer/Chatterino-YouTube?include_prereleases"></a>
</p>

<p align="center">
  <a href="https://silkstack.ai"><img src="docs/resources/weaver.svg" alt="Weaver logo" width="24" height="24"></a>
  Made with <a href="https://silkstack.ai">Weaver</a>
</p>

Chatterino-YouTube is a fork of [Chatterino 2](https://github.com/Chatterino/chatterino2), a chat client for [Twitch.tv](https://twitch.tv), with YouTube live chat support.
The Chatterino 2 wiki can be found [here](https://wiki.chatterino.com).
Contribution guidelines can be found [here](https://wiki.chatterino.com/Contributing%20for%20Developers).

## YouTube live chat in this fork

The `youtube-innertube-preview` branch adds experimental YouTube live-chat support to this Chatterino fork. The upstream Chatterino downloads and the upstream repository do not include these changes.

### Features

- Read public live chat from a YouTube video URL or video ID using YouTube's Innertube live-chat endpoint. Chat reading does not poll the YouTube Data API, so it avoids spending Data API quota on each chat refresh.
- Follow a channel by its channel name, `@handle`, username URL, channel ID, or channel URL. Channel name search uses YouTube's public search page, so viewing and reconnecting to future streams does not require sign-in.
- Show YouTube chat badges, custom and member-only emotes, Super Chats and Super Stickers with their amounts, membership and gift announcements, and reply markers when YouTube provides reply metadata.
- Show stream details in the chat header, including the title and live thumbnail on hover when available.
- Sign in with Google OAuth to send messages and use supported moderation actions.

Innertube and YouTube's public search page are undocumented interfaces and may change. OAuth-backed actions and the optional Data API fallback for channel-name lookup use the YouTube Data API and count toward its quota. Reading public chat and normal channel-name lookup do not require sign-in or an API key.

### YouTube setup

1. Download the [Windows x64 preview ZIP](https://github.com/vibeCoderGalaxyDestroyer/Chatterino-YouTube/releases/latest) and extract the entire archive to a separate folder. Run `chatterino.exe` from that folder. Keep its DLLs and Qt plugin folders beside the executable.
2. Paste a YouTube live-video URL or video ID to open that stream, or add a channel name/`@handle`/channel URL to follow the channel. A channel split checks every five minutes and attaches to chat when the channel goes live, then follows its next live stream after the current one ends. Reading public chat needs no sign-in or API key.
3. To send messages or use supported moderation actions, enable YouTube Data API v3 in Google Cloud, create a **Desktop app** OAuth client, and enter its client ID (and secret if required) under **Settings → Accounts**. If the OAuth consent screen is in testing mode, add your Google account as a test user before choosing **Sign in to YouTube**.

### Use this fork with an existing Chatterino install

You cannot add the YouTube features to an existing upstream Chatterino installation with a setting or plugin. They are compiled into this fork. You do not need to uninstall the official app: extract this fork's release ZIP into a separate folder and run it there, or build it from the `youtube-innertube-preview` branch.

The release ZIP already includes the Qt runtime files. If building locally instead, follow the build steps below and the [standalone packaging instructions](BUILDING_ON_WINDOWS.md#producing-standalone-builds). Copying only `chatterino.exe` is not enough.

By default, the fork uses the same Chatterino settings profile as the official install. To keep the fork's settings and cache separate, create a file named `modes` beside its executable and put this single line in it:

```text
portable
```

Replacing the official installation is possible, but back up the installation and your settings first, and replace the complete packaged output rather than only the executable. With the updater enabled (the default), this fork still checks the upstream Chatterino update service, so an upstream update may replace the fork's YouTube build. Running it in a separate portable folder is the safer option.

## Download

Download this fork's [YouTube preview release](https://github.com/vibeCoderGalaxyDestroyer/Chatterino-YouTube/releases/latest) for Windows x64. Extract the entire ZIP before launching. The separate symbols ZIP is for crash debugging and is not needed to run the app.

Current releases are available at [https://chatterino.com](https://chatterino.com).
Windows users can also install Chatterino [from Chocolatey](https://chocolatey.org/packages/chatterino).
These upstream downloads do not include this fork's YouTube changes.

## Nightly build

You can download the latest upstream Chatterino 2 build over [here](https://github.com/Chatterino/chatterino2/releases/tag/nightly-build)

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

## Contributors

- [vibeCoderGalaxyDestroyer](https://github.com/vibeCoderGalaxyDestroyer) — project maintainer and YouTube fork.
- [Chatterino contributors](https://github.com/Chatterino/chatterino2/graphs/contributors) — the upstream chat client.
- [OpenAI Codex](https://openai.com/codex/) — AI coding assistant credited for this branding update.
