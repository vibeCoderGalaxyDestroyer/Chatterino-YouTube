# YouTube live chat support

This fork adds YouTube as a provider inside the existing Chatterino app. Twitch
and YouTube splits can coexist in one tab, and each can also be placed in its
own tab. Add a YouTube split with a live video URL or video ID, or enter a
channel name, channel ID, `@handle`, legacy username, or channel URL to follow
that channel. Channel tabs show offline/live status and check again every five
minutes. When a stream starts, the tab switches to its chat and updates the
header with the stream details.

## Included in this fork

- Read public live chat from YouTube's website Innertube endpoint, using the
  continuation and wait interval supplied by the page.
- Sign in through Google OAuth in the system browser and send messages as the
  signed-in account.
- Render text chat, Super Chats with the supplied currency amount and highlight
  color, Super Stickers with their supplied artwork, new members, membership
  milestones, gifted memberships, polls, and YouTube system notices when those
  events appear in the live chat response.
- Show YouTube's supplied author name colors, badge artwork, member emotes,
  reply context, and hover previews when the response includes image variants.
- Apply Chatterino's similarity filters and highlight rules to incoming YouTube
  messages. The initial backlog is shown without replaying notification sounds.
- Reflect message removals, user message removals, chat clears, banner changes,
  and poll updates when Innertube sends those actions.
- Add context menu actions to delete a message, timeout a user, or permanently
  ban a user. YouTube enforces owner/moderator permissions.
- Open the video or its YouTube Studio livestream page. Signed-in users can
  also create a live poll from the split menu.

Plain channel-name lookup searches YouTube's public results page and chooses an
exact channel-title or handle match; if more than one channel has that title,
use its `@handle` or channel URL. This lookup works without sign-in or an API
key. If the public results page does not return a usable exact match, an
optional official Data API fallback is used when OAuth or an API key is
configured. Channel IDs, handles, legacy usernames, and channel URLs are
monitored through their public `/live` pages. A resolved name is converted to
its channel ID and monitored the same way, so the tab can find the next live
video after the current stream ends. Open video-ID splits also start following
the owning channel after its ID is read from the watch page.

The app's local Accounts settings store the API key, OAuth client ID, optional
OAuth client secret, and OAuth tokens. Public video links and public channel
chat can be read without sign-in or a YouTube Data API key. Google OAuth is
used for sending messages and moderation; a Desktop OAuth client ID is
required to sign in. Chatterino itself does not request a YouTube password.
These values are currently saved as readable strings in
`Settings/settings.json`; this fork does not encrypt them with Windows DPAPI or
the Windows Credential Manager. Other programs running as the same Windows
user can read that file. The Settings reveal panel only hides the values on
screen and does not encrypt them. Do not share the local Settings folder or
embed a user's credentials in a distribution build.

## API limitations

Innertube and YouTube's public search page are undocumented interfaces used by
YouTube's website. YouTube may change them or throttle requests, so name
resolution, status detection, or chat reading can stop until the client is
updated. Live-status checks run once every five minutes while a YouTube split
is open; a new live event can therefore take up to five minutes to appear.
These checks do not use the project's YouTube Data API quota. Normal
channel-name resolution uses public search and does not use that quota; the
optional API fallback and signed-in send/moderation metadata do. Each new
stream may trigger one `videos.list` lookup for signed-in metadata and chat
moderation access. Direct video links need no API key for reading.

Innertube is an undocumented endpoint. The app renders the event and image
metadata that YouTube sends to this client; it cannot guarantee parity with
every badge, emote, membership event, or control shown in YouTube's website.
Some built-in badges do not include image URLs and use a compact local icon.
Use **Open in YouTube** for native rendering and controls outside the chat
view. Chat rows show the YouTube display name and message timestamp in local
time. The stream header labels YouTube chats with `(YT)`.

Moderator actions and poll creation are shown for a signed-in account. The API
rejects them if that account lacks permission for the live chat. Reading uses
Innertube and does not make YouTube Data API chat-list requests. Sending,
moderation, and the optional channel-name search fallback use official Data
API requests and consume their documented quota. Studio remains the full
YouTube broadcaster control room; this fork does not implement every
broadcast or chat setting.

For crash tracing, the app writes a local, rotating diagnostic file to
`<Chatterino data directory>/Logs/youtube-diagnostics.log`. It records startup
and shutdown, reader heartbeats, request/retry counts, and event batch sizes;
it does not record chat text or credentials. The active file rotates at 2 MiB
and keeps three previous files.

## References

- [YouTube live chat messages](https://developers.google.com/youtube/data/live/reference/rest/v3/liveChatMessages)
- [Remahy's Chatterino YouTube chat plugin](https://github.com/Remahy/Chatterino-Plugins/tree/main/yt-chat)
- [YouTube live chat bans](https://developers.google.com/youtube/data/live/reference/rest/v3/liveChatBans)
- [YouTube live chat moderators](https://developers.google.com/youtube/data/live/reference/rest/v3/liveChatModerators)
- [YouTube broadcast API](https://developers.google.com/youtube/data/live/reference/rest/v3/liveBroadcasts)
- [Google desktop OAuth](https://developers.google.com/identity/protocols/oauth2/native-app)
