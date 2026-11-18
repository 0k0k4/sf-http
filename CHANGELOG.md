# Changelog

## 0.1.0 — 2026-11-18

First release.

- HTTP/HTTPS requests from addon scripts and microcontrollers: `http.get`, `http.post`,
  `http.request` (GET, POST, PUT, PATCH, DELETE, HEAD), polled with `http.poll`, plus
  `state`, `cancel`, `pending`.
- WinHTTP on a thread pool: TLS through Windows, redirects, gzip/deflate, overall timeout,
  automatic proxy with direct fallback.
- JSON: `json_decode`, `json_encode`, `field` (one value by path), and `urlencode`.
- Per-script isolation, quotas (`max_per_vm`, `rate_per_minute`, `max_inflight`), size
  limits, `allow_hosts`, and no local network for microcontrollers by default.
- Requests dropped when a script goes idle or the world closes; clean unload.
- Tested in game on Stormworks v1.15.23 (addon and microcontroller), plus offline tests.
