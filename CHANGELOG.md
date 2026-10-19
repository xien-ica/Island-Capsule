# Changelog

All notable changes to Island Capsule are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed
- Scrolling over the capsule now changes only the volume of the app that owns the current media (its entry in Windows' Volume Mixer) instead of the system volume. The app is matched to its audio session by the media session's app id; if it has not played any sound yet, nothing changes. Turn off *Settings > Volume > Only the playing app* (`volume_app_only=false` in `settings.ini`) to get the old system-wide behaviour back.
- The visualizer now reacts only to the audio of the app that owns the current media, not to everything the PC plays (per-process loopback capture, Windows 11 / Windows 10 build 20348+; older systems keep using the system mix). If the player's app id cannot be matched to a running process the system mix is used as well. Turn off *Settings > Visualizer > Only the playing app* (`visualizer_app_only=false`) for the old behaviour.

## [1.0.2] - 2026-10-17

### Changed
- Lyrics lookups now send Island Capsule's registered LRCLIB application ID (the `Lrclib-App-Id` header) with every request. This identifies the app to [LRCLIB](https://lrclib.net) and lets it use the higher rate limit for registered applications, so rapid track skipping should hit "429 Too Many Requests" less often.
- README privacy section now mentions the application ID header.

### Fixed
- Crash when skipping tracks quickly with the next / previous buttons: while a media session was closing, Windows returned an empty playback-info object and the app read from it, which killed the process. Every use of those objects is now null-checked and the last known track info is kept instead.
- Crashes left no `crash.log` / `crash.dmp` in `%APPDATA%\IslandCapsule`: the crash reporter existed but was never built into the app or started. It is now installed at startup.

[1.0.2]: https://github.com/xien-ica/Island-Capsule/compare/v1.0.1...v1.0.2
