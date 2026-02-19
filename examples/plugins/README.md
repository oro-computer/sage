# Plugin examples (JavaScript / QuickJS)

These scripts demonstrate the `sage` plugin surface area:

- events via `on(type, fn)` / `addEventListener(type, fn)`
- custom `:` commands via `command(name, fn)`
- executing built-in or plugin commands via `exec(cmd)`
- logging via `console.*` (filtered by `SAGE_CONSOLE_LEVEL`)
- builtin ESM modules via `import ... from 'sage:...'`

## Install

```sh
mkdir -p ~/.config/sage/plugins
rsync -a examples/plugins/ ~/.config/sage/plugins/
```

Logs go to `$SAGE_PLUGIN_LOG` (if set), else `$XDG_CACHE_HOME/sage/plugins.log`, else `~/.cache/sage/plugins.log`.

## Events

Currently emitted events and payload shapes:

- `on(type, fn)` / `once(type, fn)` call `fn(payload)` where payload is:
  - `CustomEvent.detail` (host-emitted events), or
  - `MessageEvent.data`
- `addEventListener(type, fn)` receives an Event object (host events are `CustomEvent`s; payload is in `ev.detail`).
- Events:
  - `open`: `{ path, tab, tab_count }`
  - `tab_change`: `{ from, to, tab_count }`
  - `search`: `{ query, regex, ignore_case }`
  - `copy`: `{ bytes }`
  - `quit`: no payload

## Commands

- Built-in `:` commands (like `:q`, `:bn`, `:t 2`, `:42`, `:-2`) are handled first.
- Unknown `:` commands are offered to plugins.
- Command names are normalized case-insensitively by the JS bootstrap.
- `exec(cmd)` enqueues commands; the host runs them on UI ticks (don’t enqueue unbounded loops).

## ESM modules

Plugins are evaluated as ES modules and can import:

- `sage:fs`: minimal `node:fs/promises`-ish helpers.
  - `readFile(path, { encoding?, maxBytes? })` reads from local open tabs (and the plugin data dir).
  - `writeFile(name, data)`, `appendFile(name, data)`, `readdir()` operate on the plugin’s data dir.
- `sage:path`: minimal `node:path`-ish helpers (POSIX-y).
- `sage:process`: `pid`, `ppid`, `cwd()`, `exec(cmd, { timeoutMs?, maxBytes? })`.
- `sage:env`: `get(name)`, `set(name, value, { overwrite? })`, `unset(name)`.
- `sage:navigator`: browser-like `navigator` instance (`userAgent`, versions).
- `sage:performance`: `performance.now()` + `performance.timeOrigin`.
  - The bootstrap also installs `performance` on `globalThis`.
- `sage:crypto`: `crypto.getRandomValues(...)` + `crypto.randomUUID()`.
  - The bootstrap also installs `crypto` on `globalThis`.
- `sage:url`: WHATWG-style `URL` + `URLSearchParams` + `URL.parse`/`URL.canParse`.
  - The bootstrap also installs `URL` and `URLSearchParams` on `globalThis`.
- `sage:core/dom`: `DOMException` + `structuredClone`.
  - The bootstrap also installs `DOMException` and `structuredClone` on `globalThis`.
- `sage:core/web`: host-free WHATWG-ish web primitives:
  - `Headers`, `Request`, `Response`, `FormData`, `Blob`, `ReadableStream`, `AbortController`, `AbortSignal`, `TextEncoder`, `TextDecoder`
  - The bootstrap also installs these on `globalThis`.
- `sage:fetch`: WHATWG-ish `fetch` backed by the native host.
  - Extra options: `timeoutMs`, `maxBytes`, `followRedirects`
  - The bootstrap installs `fetch` on `globalThis`, so plugins can typically just call `fetch(...)`.

## Example scripts

- `00-log-events.js`: logs all events.
- `10-commands.js`: registers custom commands and uses `exec(...)` to run built-ins.
- `20-session-stats.js`: stateful plugin; counts events and exposes `:stats` / `:stats-reset`.
- `30-aliases.js`: short alias commands (macros for built-in `:` commands).
- `40-macros.js`: queues multiple `:` commands (`:rotate`, `:tour`) to demonstrate exec chaining.
- `50-provider.js` + `51-consumer.js`: cross-plugin composition (one plugin calls another via `exec(...)`).
- `60-fs.js`: reads/writes plugin data files and reads the currently open file.
- `70-process.js`: uses `sage:process` + `sage:env` (runs simple commands).
- `72-imports.js` + `72-imports_util.mjs`: demonstrates relative ESM imports inside a plugin.
- `80-fetch.js`: demonstrates global `fetch(...)` (GET, abort, FormData POST).
- `81-url.js`: demonstrates WHATWG-style `URL` + `URLSearchParams` (`sage:url`) (parse + relative resolution + query editing).
- `82-crypto.js`: demonstrates `crypto.getRandomValues` + `crypto.randomUUID` and `performance.now` (`sage:crypto` / `sage:performance`).
- `83-dom.js`: demonstrates `DOMException` + `structuredClone` (`sage:core/dom`).
