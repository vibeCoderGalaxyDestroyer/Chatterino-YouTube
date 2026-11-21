# YouTube live chat support

This fork adds YouTube as a provider inside the existing Chatterino app. Twitch
and YouTube splits can coexist in one tab, and each can also be placed in its
own tab. Add a YouTube split with a live video URL or video ID, or enter a
channel name, `@handle`, or channel URL to find that channel's active livestream.

## Included in this fork

- Read active YouTube live chat through `liveChatMessages.streamList`, resume
  from YouTube's page token after a disconnect, and reconnect with backoff.
- Sign in through Google OAuth in the system browser and send messages as the
  signed-in account.
- Show paid chat, sticker descriptions, membership events, and author role
  labels when the API provides their metadata.
- Add context menu actions to delete a message, timeout a user, or permanently
  ban a user. YouTube enforces owner/moderator permissions.
- Open the video or its YouTube Studio livestream page. Signed-in users can
  also create a live poll from the split menu.

Channel-name lookup chooses an exact channel-name match; if more than one
channel has that name, use its `@handle` or channel URL. The selected channel
must have an active livestream with chat enabled. Name lookup uses YouTube
search requests, which consume additional API quota.

The app's local Accounts settings store the API key, OAuth client ID, optional
OAuth client secret, and OAuth tokens. A Google Cloud project with YouTube Data
API v3 enabled is needed. Desktop OAuth client secrets are optional in Google's
standard flow, but this setting is available for projects whose token endpoint
requires one. Do not share the local Settings folder or embed a user's
credentials in a distribution build.
When signed out, enter an API key under **Settings > Accounts** to view public
chat. When signed in, OAuth is used for reading and posting, so the API key is
optional. The Desktop OAuth client ID is still needed to sign in. Chatterino
itself does not request a YouTube password.

This preview does not include a shared API key. Signed-out users must enter
their own key. A distribution build can remove that per-user step only if its
publisher supplies an app-owned key; all signed-out viewers would then share
that project's quota.

## API limitations

The public chat API provides message text and event metadata, but not a reusable
catalog of channel custom emote images or all of YouTube's native sticker art.
This fork can show the text and sticker description it receives; it cannot
promise the same emote rendering as youtube.com. Use **Open in YouTube** for
native rendering and controls outside the API.

Chat reading uses Google's gRPC `liveChatMessages.streamList` endpoint. It
pushes messages over a long-lived connection and returns a page token that the
app uses to resume after a disconnect. Each stream connection costs 5 quota
units; messages do not each trigger a separate REST poll. Reconnections cost
another call. Resolving a channel or video, sending chat, and moderation still
use YouTube Data API requests and consume their documented quota. If Google
reports quota exhaustion, the app pauses all YouTube chat streams and shows a
notice in each affected chat. Reconnect after the project's quota is available
again to resume.
Chat rows show the YouTube display name and message timestamp in local time.
Tabs use the channel title rather than the livestream title.
The API exposes owner, moderator, member, and verified status; these are shown
as compact badges. Membership months and level names are shown when the API
includes them in a milestone or membership event. The public API does not expose
Top Fan rank numbers or custom membership badge artwork.

Moderator actions and poll creation are shown for a signed-in account. The API
rejects them if that account lacks permission for the live chat. Studio remains
the full YouTube broadcaster control room; this fork does not implement all
broadcast metadata, status, or chat-mode controls.

## References

- [YouTube live chat messages](https://developers.google.com/youtube/data/live/reference/rest/v3/liveChatMessages)
- [YouTube live chat streaming guide](https://developers.google.com/youtube/data/live/streaming-live-chat)
- [YouTube liveChatMessages.streamList reference](https://developers.google.com/youtube/data/live/reference/rest/v3/liveChatMessages/streamList)
- [Read and poll live chat messages](https://developers.google.com/youtube/data/live/reference/rest/v3/liveChatMessages/list)
- [YouTube live chat bans](https://developers.google.com/youtube/data/live/reference/rest/v3/liveChatBans)
- [YouTube live chat moderators](https://developers.google.com/youtube/data/live/reference/rest/v3/liveChatModerators)
- [YouTube broadcast API](https://developers.google.com/youtube/data/live/reference/rest/v3/liveBroadcasts)
- [Google desktop OAuth](https://developers.google.com/identity/protocols/oauth2/native-app)
