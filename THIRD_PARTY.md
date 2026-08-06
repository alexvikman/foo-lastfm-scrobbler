# Third-party notices

This project uses the following third-party software when building
`foo_lastfm_scrobbler`.

## foobar2000 SDK

- Version: 2025-03-07
- Copyright: Copyright (c) 2002-2025 Peter Pawlowski
- Source: <https://www.foobar2000.org/SDK>
- Download: <https://www.foobar2000.org/downloads/SDK-2025-03-07.7z>
- SHA-256: `CCDA3C5840E66E0E28A7E4FE36407C4E78581AA30C40C362A188FCBAAE799A3E`
- License: [foobar2000 SDK license](LICENSES/foobar2000-sdk.txt)

The SDK archive and source tree are downloaded into the untracked `.deps`
directory and are not committed to this repository. The compiled component is
built against and linked with libraries supplied by the SDK under the terms of
the license above.

## PFC

- Copyright: Copyright (C) 2002-2024 Peter Pawlowski
- Distribution: Included with the foobar2000 SDK
- License: [PFC license](LICENSES/pfc.txt)

PFC is compiled and linked into the component. Its license is separate from
the main foobar2000 SDK license.
