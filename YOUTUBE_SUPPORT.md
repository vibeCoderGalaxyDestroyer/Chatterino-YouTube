# YouTube live chat support

This fork adds YouTube as a provider inside the existing Chatterino app. Twitch
and YouTube splits can coexist in one tab, and each can also be placed in its
own tab. Add a YouTube split with a live video URL or video ID, or enter a
channel name, `@handle`, or channel URL to find that channel's active livestream.

## Included in this fork

- Read public live chat from YouTube's website Innertube endpoint, using the
  continuation and wait interval supplied by the page.
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
OAuth client secret, and OAuth tokens. Public video links can be read without
sign-in or a YouTube Data API key. Google OAuth is used for sending messages
and moderation; a Desktop OAuth client ID is required to sign in. Chatterino
itself does not request a YouTube password. Channel lookup still uses YouTube
Data API requests and needs OAuth or an API key. Do not share the local
Settings folder or embed a user's credentials in a distribution build.

## API limitations

Innertube is an undocumented endpoint used by YouTube's website. YouTube may
change it or throttle requests, so reading can stop until the client is
updated. These reads do not use the project's YouTube Data API quota. Channel
name resolution and OAuth write actions still use official Data API methods and
consume their documented quota. Direct video links need no API key for reading.

Innertube provides chat text, role badges, and some event metadata. This fork
does not yet reproduce every native YouTube badge, custom emoji, or sticker
image. Use **Open in YouTube** for native rendering and controls outside the
chat view.
Chat rows show the YouTube display name and message timestamp in local time.
Tabs use the channel title rather than the livestream title.
Owner, moderator, member, and verified status are shown as compact badges when
the website includes them in the chat response. Membership months and level
names are shown when available. Top Fan ranks and native badge artwork are not
currently reproduced.

Moderator actions and poll creation are shown for a signed-in account. The API
rejects them if that account lacks permission for the live chat. Studio remains
the full YouTube broadcaster control room; this fork does not implement all
broadcast metadata, status, or chat-mode controls.

## References

- [YouTube live chat messages](https://developers.google.com/youtube/data/live/reference/rest/v3/liveChatMessages)
- [Remahy's Chatterino YouTube chat plugin](https://github.com/Remahy/Chatterino-Plugins/tree/main/yt-chat)
- [YouTube live chat bans](https://developers.google.com/youtube/data/live/reference/rest/v3/liveChatBans)
- [YouTube live chat moderators](https://developers.google.com/youtube/data/live/reference/rest/v3/liveChatModerators)
- [YouTube broadcast API](https://developers.google.com/youtube/data/live/reference/rest/v3/liveBroadcasts)
- [Google desktop OAuth](https://developers.google.com/identity/protocols/oauth2/native-app)
