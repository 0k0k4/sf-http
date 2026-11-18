# sf_http — HTTP/HTTPS & JSON for Stormworks Lua

A [StormFabric](https://github.com/0k0k4/StormFabric) mod that gives Stormworks: Build and
Rescue scripts real HTTP and HTTPS requests to **any URL**, plus JSON helpers, for both
**addon/mission scripts** (`script.lua`) and **microcontrollers**.

The game's own `async.httpGet` can only reach `localhost`, so every web feature needed a
proxy program running next to the game. With sf_http, scripts call Discord webhooks, live
weather, leaderboards or any REST API directly. Requests run on a background thread pool
(WinHTTP): a Lua call never blocks the game.

```lua
local id = http.get("https://wttr.in/Paris?format=j1")
-- later, in onTick:
local id, kind, status, body = http.poll()
if kind == "response" then
    server.announce("weather", http.field(body, "current_condition.1.temp_C") .. " C")
end
```

## Requirements

| | |
|---|---|
| Game | Stormworks **v1.15.23** for addon scripts; microcontrollers also work on v1.15.18 |
| Loader | [StormFabric](https://github.com/0k0k4/StormFabric) v0.2.0 or later |
| Full JSON | `json_encode` and JSON arrays need a loader with `extras->pushValue` (StormFabric `main`, after v0.2.0). On v0.2.0 everything else works and `http.field` reads any value. |
| OS | Windows 8.1 or later (64-bit) |

## Installation

1. Install the StormFabric loader in your Stormworks folder.
2. **Close the game.**
3. Download `sf_http-<version>.zip` from the [releases](../../releases) and extract it.
4. Run `install.bat "C:\path\to\Stormworks"`. It copies `mod.json` and `sf_http.dll` to
   `<Stormworks>\mods\http\`, adds `http=1` to `mods\_modloader.ini`, and installs the
   example addon in `%APPDATA%\Stormworks\data\missions\sf_http_test`.
   (By hand: copy `mod.json` and `sf_http.dll` to `<Stormworks>\mods\http\` and enable
   **HTTP** in the in-game MODS menu.)
5. Start the game through `Launcher.exe`. `StormworksModLoader.log` should show
   `http 0.1.0 ready` and `libraries: addon=yes microcontroller=yes`.

To check that everything works: create a new world with the **StormFabric HTTP Test**
addon enabled and type `?http test` in the chat. It prints `TEST OK (11/11)`.

## Lua API

Always test the table, so the script still works without the mod: `if http then … end`.

| Function | Returns |
|---|---|
| `http.get(url [, options])` | `id`, or `nil, error` |
| `http.post(url, body [, options])` | `id`, or `nil, error` |
| `http.request(method, url [, body [, options]])` | `id`, or `nil, error` — `GET POST PUT PATCH DELETE HEAD` |
| `http.poll([id])` | one finished request (see below), or nothing |
| `http.state(id)` | `"queued"`, `"running"`, `"done"` or `"unknown"` |
| `http.cancel(id)` | `true` / `false` |
| `http.pending()` | requests of this script not returned by `poll` yet |
| `http.json_decode(text)` | Lua value (`null` → `nil`), or `nil, error` |
| `http.json_encode(value)` | JSON text, or `nil, error` |
| `http.field(json_text, "a.b.1.c")` | one value from the JSON without decoding it all (keys, 1-based indexes), or nothing |
| `http.urlencode(text)` | text encoded for a URL (`a b` → `a%20b`) |
| `http.version()` | `"0.1.0"` |

**Results of `poll`** (a request is released once returned):

| | values |
|---|---|
| response | `id, "response", status, body, headers` — `headers`: table with lower-case names |
| failure | `id, "error", message, code` — network, DNS, TLS, timeout, cancelled, size limit |

A 404 or 500 is a **response** with its `status`, not an error. A `post` body is sent as
`application/json` when it starts with `{` or `[`, otherwise as `text/plain`.

**Options** are a string `"key=value;key=value"`:

| Key | Value | Effect |
|---|---|---|
| `header` | `Name: value` | extra request header (repeatable; no `;` in the value) |
| `content_type` | text | type of the body sent |
| `timeout` | 1–120 s | limit for the whole request (15 s by default) |
| `insecure` | `0` / `1` | `https`: accept an invalid or self-signed certificate |
| `redirects` | `0` / `1` | follow redirects (yes by default; never from https to http) |
| `max_bytes` | bytes | limit of the body received (never above `max_response_bytes`) |
| `headers` | `0` / `1` | `0`: do not build the `headers` table (saves work in microcontrollers) |
| `proxy` | `auto` / `direct` / `system` | `auto`: automatic proxy, then direct |

### JSON

- `json_decode`: objects → tables with string keys, arrays → tables indexed from 1,
  `null` → `nil` (a `null` inside an array leaves a hole), integers → Lua integers.
  At most 64 levels of nesting and 100,000 values. Arrays are filled with `table.pack` +
  `table.move`, because Stormworks removes `rawset` from script VMs; in a VM without
  `rawset`, an empty object key or a key longer than 255 bytes is skipped.
- `json_encode`: a table whose keys are exactly `1..n` becomes an array (an empty table
  gives `[]`), anything else an object (numeric keys written as strings). Refused:
  functions, NaN/infinity, cycles (more than 64 levels). Object key order is not
  guaranteed.
- `field` works on every game build and loader version, without decoding the whole
  document: handy in microcontrollers. An object or array found comes back as JSON text.

### Example — Discord webhook

```lua
http.post(WEBHOOK_URL, http.json_encode({content = "Hello from Stormworks!"}))
-- Discord answers 204 on success
```

Keep the webhook URL in your script, not in the chat: the game chat cuts long messages.
`examples/script.lua` has a `DISCORD_WEBHOOK` variable and a `?http discord <text>` command.

### Example — microcontroller

`examples/mc_test.lua` (Lua block with video out to a monitor) requests
`https://httpbin.org/uuid` every 5 seconds and shows the status and the uuid.

## Isolation and lifetime

- Each VM (one addon, one microcontroller) only sees **its own** requests: on its first
  call the mod stores a numeric token in `http.__owner` in that VM. Do not change it.
- A VM that does not call the mod for `idle_timeout` seconds loses its requests: this
  cleans up after a despawned vehicle or `?reload_scripts`. Call `poll` every tick while a
  request is in flight.
- When the world closes (no addon VM left), every request is dropped.
- Unloading the mod closes everything and waits for its threads.

## Configuration

Created on first launch: `<Stormworks>\mods\http\config\http.cfg.bin` (plain
`key=value` text; edit it with the game closed; an invalid value makes the whole file
ignored, with the reason in the log).

| Key | Default | Effect |
|---|---|---|
| `allow_hosts` | `*` | allowed hosts, comma-separated: `*`, `host`, `*.domain` |
| `max_inflight` | 16 | requests not returned yet, all scripts together |
| `max_per_vm` | 8 | requests not returned yet, per script |
| `rate_per_minute` | 60 | requests started per script and per minute |
| `max_response_bytes` | 1048576 | largest body received |
| `max_body_bytes` | 1048576 | largest body sent |
| `threads` | 4 | requests running at the same time |
| `idle_timeout` | 30 | seconds without a call before a script's requests are dropped |
| `local_network` | `addons` | who may reach `localhost` and the local network: `none`, `addons`, `all` |
| `log_requests` | 1 | write every request to the log (method + host) |
| `microcontrollers` | 1 | `0`: library for addon scripts only |

## Security

Any script that gets the table can send requests, including the microcontroller of a
vehicle downloaded from the Workshop. Protections:

- **disabled by default** in the loader (`enabled_by_default: false`);
- **no local network for microcontrollers** (`local_network=addons`): `localhost`, `127.x`,
  `10.x`, `172.16–31.x`, `192.168.x`, `169.254.x`, `100.64–127.x`, local IPv6, and exotic
  numeric forms (`2130706433`, `0x7f.1`, `0177.0.0.1`). Limit: a public domain name that
  *resolves* to a local address is not detected;
- every request is logged (scheme + host + port only, never the path, query, headers or
  body, which may hold tokens);
- `allow_hosts`, `rate_per_minute`, `max_per_vm`, `microcontrollers=0`.

## Building from source

Requires MSVC x64 (Visual Studio 2022 or Build Tools) and the StormFabric SDK headers.

```bat
git clone https://github.com/0k0k4/StormFabric
git clone https://github.com/0k0k4/sf-http
cd sf-http
build.bat                                 :: uses ..\StormFabric\include
build.bat "C:\path\to\StormFabric\include"
```

Output: `build\sf_http.dll`, plus the offline tests.

### Tests (no game needed)

```bat
pip install cryptography                 :: once, for the self-signed test certificate
python tests\test_server.py              :: http://127.0.0.1:8780 and https://localhost:8781
build\http_selftest.exe [--online]       :: WinHTTP client: 13 groups (+ httpbin.org, example.com)
build\mod_test.exe                       :: whole mod through a fake StormFabric host: 11 groups
```

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `http` is `nil` in the addon | mod disabled, game build other than 1.15.23, or world loaded before the mod: reload it |
| `local network address not allowed` | microcontroller to localhost/LAN: set `local_network=all` |
| `host not allowed by http.cfg` | `allow_hosts` |
| `too many pending requests` | the script never calls `http.poll()` |
| `JSON arrays need game build 1.15.23…` | build 1.15.18 or loader without `pushValue`: use `http.field` |
| `security error (12175)` | invalid certificate: `insecure=1` for a test server |
| Discord `HTTP 401` | wrong or truncated webhook URL (do not type it in the chat) |

## License

MIT — see [LICENSE](LICENSE).
