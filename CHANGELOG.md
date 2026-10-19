# Changelog

All notable changes to Island Capsule are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

## [1.0.3] - 2026-10-19

### Added
- **Settings > Behavior > Volume > Only the playing app** (`volume_app_only` in `settings.ini`, on by default).
- **Settings > Capsule > Visualizer > Only the playing app** (`visualizer_app_only`, on by default).
- The log now reports what the visualizer capture is doing, as `[visualizer] ...` lines (including the failing step and error code if per-app capture cannot start).

### Changed
- Scrolling over the capsule now changes only the volume of the app that owns the current media (its entry in Windows' Volume Mixer) instead of the system volume. The app is matched to its audio session by the media session's app id; if it has not played any sound yet, nothing changes. Turn the option off to get the old system-wide behaviour back.
- The visualizer now reacts only to the audio of the app that owns the current media, not to everything the PC plays (per-process loopback capture, Windows 11 / Windows 10 build 20348+). The bars follow the app's own audio before its volume is applied, so they keep moving even when you turn that app's volume down. The system mix is still used when Windows is too old for per-app capture, or when the player's app id cannot be matched to a running process. If per-app capture fails on a supported Windows, the system mix is NOT used (other apps would move the bars). Turn the option off for the old behaviour.

## [1.0.2] - 2026-10-17

### Changed
- Lyrics lookups now send Island Capsule's registered LRCLIB application ID (the `Lrclib-App-Id` header) with every request. This identifies the app to [LRCLIB](https://lrclib.net) and lets it use the higher rate limit for registered applications, so rapid track skipping should hit "429 Too Many Requests" less often.
- README privacy section now mentions the application ID header.

### Fixed
- Crash when skipping tracks quickly with the next / previous buttons: while a media session was closing, Windows returned an empty playback-info object and the app read from it, which killed the process. Every use of those objects is now null-checked and the last known track info is kept instead.
- Crashes left no `crash.log` / `crash.dmp` in `%APPDATA%\IslandCapsule`: the crash reporter existed but was never built into the app or started. It is now installed at startup.

[1.0.3]: https://github.com/xien-ica/Island-Capsule/compare/v1.0.2...v1.0.3
[1.0.2]: https://github.com/xien-ica/Island-Capsule/compare/v1.0.1...v1.0.2
