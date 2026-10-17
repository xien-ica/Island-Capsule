# Changelog

All notable changes to Island Capsule are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

## [1.0.2] - 2026-10-17

### Changed
- Lyrics lookups now send Island Capsule's registered LRCLIB application ID (the `Lrclib-App-Id` header) with every request. This identifies the app to [LRCLIB](https://lrclib.net) and lets it use the higher rate limit for registered applications, so rapid track skipping should hit "429 Too Many Requests" less often.
- README privacy section now mentions the application ID header.

### Fixed
- Crash when skipping tracks quickly with the next / previous buttons: while a media session was closing, Windows returned an empty playback-info object and the app read from it, which killed the process. Every use of those objects is now null-checked and the last known track info is kept instead.
- Crashes left no `crash.log` / `crash.dmp` in `%APPDATA%\IslandCapsule`: the crash reporter existed but was never built into the app or started. It is now installed at startup.

[1.0.2]: https://github.com/xien-ica/Island-Capsule/compare/v1.0.1...v1.0.2
