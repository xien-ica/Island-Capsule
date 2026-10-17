# Changelog

All notable changes to Island Capsule are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

## [1.0.2] - 2026-10-17

### Changed
- Lyrics lookups now send Island Capsule's registered LRCLIB application ID (the `Lrclib-App-Id` header) with every request. This identifies the app to [LRCLIB](https://lrclib.net) and lets it use the higher rate limit for registered applications, so rapid track skipping should hit "429 Too Many Requests" less often.
- README privacy section now mentions the application ID header.

[1.0.2]: https://github.com/xien-ica/Island-Capsule/compare/v1.0.1...v1.0.2
