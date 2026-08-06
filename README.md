# foo_lastfm_scrobbler

A native Last.fm scrobbler component for foobar2000 2.x on 64-bit Windows.

## Features

- Browser-based Last.fm account authorization with a persistent session.
- Now-playing updates when a tagged track starts.
- Scrobbling after half the track or four minutes, whichever comes first.
- Active listening-time tracking that excludes pauses, stops, and seeks.
- Structured tag handling for artist, title, album, album artist, track number,
  MusicBrainz track ID, and duration.
- A Windows-DPAPI-encrypted queue that survives restarts and preserves ordering.
- Automatic retry with bounded backoff for temporary network and Last.fm errors.
- Account-bound queues that cannot be submitted to a different Last.fm user.
- Queue and submission diagnostics in foobar2000 preferences.

## Installation

Open `foo_lastfm_scrobbler.fb2k-component` and confirm the installation in
foobar2000. The component is installed for 64-bit foobar2000.

Connect an account from:

`Preferences > Tools > Last.fm Scrobbler`

Authorization opens Last.fm in the default browser. After approval, return to
foobar2000 and select **Finish connection**. The resulting session is encrypted
for the current Windows user and reused automatically.

## Scrobbling rules

A track must have artist and title tags and be longer than 30 seconds. It is
submitted after half its duration or four minutes of active playback,
whichever occurs first. Each playback session is submitted at most once.

Known placeholder values such as unknown artist or unknown track are rejected.
Metadata is read from foobar2000 tags and is never guessed from filenames.

## Building

Requirements:

- Visual Studio or Build Tools with Desktop development with C++
- MSVC v143 toolset
- Windows 10/11 SDK
- PowerShell 5.1 or later

Download the pinned foobar2000 SDK and build the Release x64 component:

```powershell
.\scripts\bootstrap-sdk.ps1
.\scripts\build.ps1
```

The build script compiles the component and runs the core test suite. Output is
written to `out\Release`.

Create the installable package:

```powershell
.\scripts\package.ps1
```

Build the Debug configuration with:

```powershell
.\scripts\build.ps1 -Configuration Debug
```

## Last.fm API credentials

The component requires a Last.fm API account that identifies the application.
Copy `src\auth\api_credentials.local.example.h` to
`src\auth\api_credentials.local.h`, then add the application API key and shared
secret before building.

The local credentials header is ignored by Git and must not be committed.
Without it, the component still builds but account connection is disabled.

## Data storage

The Last.fm session and pending scrobbles are encrypted with Windows DPAPI in
the current-user scope before being stored in foobar2000 configuration.
Disconnecting the account removes the saved session and its pending queue.

The foobar2000 SDK is not included in this repository. See
[`THIRD_PARTY.md`](THIRD_PARTY.md) for the pinned version and checksum.

## License

The original source code in this repository is available under the
[MIT License](LICENSE). Third-party software remains subject to the licenses
listed in [`THIRD_PARTY.md`](THIRD_PARTY.md).
