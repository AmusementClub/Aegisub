# Discord Rich Presence

The optional C++ integration connects to the local Discord desktop client over
IPC. It does not require the Discord Social SDK, .NET, a bot, an access token,
or an OAuth login.

## Configuration

Open **Preferences > General > Discord** and configure:

- **Application ID**: the public ID of an application registered in the
  [Discord Developer Portal](https://discord.com/developers/applications).
  Official builds use `1546931732245123245`; forks can override it.
- **Application display name**: the name displayed on Discord; defaults to
  `Aegisub`. An empty value also uses `Aegisub`.
- **Large image asset key**: the Discord Developer Portal asset key used for
  the large Presence image; defaults to `aegisub`.
- **Connect to Discord**: enables publishing. This is disabled by default.

Apply the changes to connect or update the presence without restarting.
Discord must be running and its activity sharing must be enabled. The settings
page reports connection status. Application IDs are public identifiers, not
credentials. A valid ID is still required when connecting without an SDK.

## Display And Timing

| Discord field | Value |
| --- | --- |
| Application name | Configured application display name |
| `details` | Subtitle filename, with all parent directories removed |
| `state` | Aegisub's elapsed time in this run, updated when the minute changes |
| `timestamps.start` | Start of the current subtitle session |
| `assets.large_image` | Configured Discord asset key |

Discord's `Playing` activity may still show a generic gamepad icon in compact
status surfaces. The uploaded large asset is shown in the expanded activity
card. Upload the Aegisub artwork in the application's Rich Presence assets and
use the matching key in the setting; the image is not read from the local
filesystem.

Discord advances the subtitle timer itself. Opening or creating another subtitle
starts a new session. Saving, saving under another name, automatic reloading,
renaming the application, and refreshing the elapsed-minute text preserve the
subtitle start time. Unsaved documents display `Untitled subtitles`.

Each project window keeps its own subtitle session. Selecting another project
switches the displayed session; closing it selects the most recently active
remaining project. Foreground/background state is not published, and both
durations continue while the application is in the background.

Discord exposes one current activity per Discord user. Separate Aegisub
processes using the same Application ID therefore compete: the most recent
successful IPC update wins, and independent instances can overwrite one
another's filename and session timer. The current implementation does not
coordinate multiple processes.

The client uses a background I/O thread, coalesces activity changes, reconnects
when Discord becomes available, and resends the latest activity after reconnect.
Disabling the feature or exiting clears the activity and closes the connection.
It is not started by headless commands or GUI automation hosts.

## Building

`WITH_DISCORD_PRESENCE` defaults to `ON`. Set it to `OFF` to omit the module and
its settings page. The implementation uses the existing Boost.Asio and JSON
dependencies; it does not add a runtime SDK library.

Windows uses named pipes. Unix platforms use local sockets in the runtime and
temporary directories, including common Flatpak and Snap locations.

The `discord_presence.*` C++ tests cover the state model. On Windows they also
exercise an isolated named-pipe server to verify the real RPC handshake,
activity fields, reconnect, ping/pong, clearing, invalid frames, and bounded
shutdown without a Discord account or desktop client.
