# `sage` Notes (performance + constraints)

## Snapshot (last verified)

- Date: **2026-02-27**
- Notes updated: **2026-02-27** (refreshed Silk-gap notes + cleaned formatting; plugin fetch now verifies system CAs by default and tolerates TLS 1.3 `NewSessionTicket` reads)
- `sage`: `d489d5f` (notes reflect subsequent local edits)
- `silk`: `ebef419` (`silk (ABI) 0.2.0`)
- Baseline: linux/glibc x86_64 (hosted POSIX runtime)
- Verified:
  - `silk test --package .` (20 tests)
  - `silk build --package .` (build module enabled)
  - `ldd ./build/bin/sage` (no libcurl or libmbedtls DT_NEEDED)
  - `./build/bin/sage --help`
  - `./build/bin/sage --index-only http://example.com`
  - `./build/bin/sage --index-only https://example.com`
  - `./build/bin/sage --index-only https://httpbin.org/stream/5`
  - `./build/bin/sage --index-only https://httpbin.org/redirect/2`
  - interactive `./build/bin/sage --no-alt-screen --no-plugins https://httpbin.org/json` (quit with `q`)
  - interactive `./build/bin/sage -v --no-alt-screen --no-plugins https://example.com` (quit with `q`)
  - interactive `SAGE_CONSOLE_LEVEL=debug SAGE_PLUGIN_LOG=/tmp/sage-plugin-smoke.log ./build/bin/sage --no-alt-screen --plugins-dir ./examples/plugins ./NOTES.md` (ran `:fetch https://example.com/`, `:fetch-abort https://example.com/`, quit with `q`)

## Architecture

- **I/O**: files are read via `std::runtime::fs::mmap_readonly` (POSIX `mmap(PROT_READ|MAP_PRIVATE)` on hosted) for zero-copy paging.
- **Network open (HTTP/HTTPS)**:
  - `sage` accepts `http://...` / `https://...` paths as **read-only** inputs (e.g. `sage https://example.com/file.txt`).
  - `ssh://...` is currently treated as a “network path” but is **not implemented yet** (future: `std::ssh2`; the stdlib module exists but `sage` doesn’t integrate it yet).
  - Implementation: fetches over TCP/TLS, spools bytes to an **unlinked temp file** (`mkstemp` + `unlink`), then `mmap`s it so the rest of the pager pipeline stays zero-copy.
  - DNS: includes a tiny IPv4 `A` resolver (UDP) that reads the first `nameserver` from `/etc/resolv.conf` and sets a best-effort `SO_RCVTIMEO` (2s). Fallbacks: `1.1.1.1`, then `8.8.8.8`.
  - Limitations (current):
    - **IPv4-only** for `sage::netfile` today (it resolves `A` records and connects via `SocketAddrV4`; `std::https` also supports `SocketAddrV6`).
    - `std::http` / `std::https` support `Content-Length`, `Transfer-Encoding: chunked`, and body-to-EOF responses. `sage::netfile` still sends `HTTP/1.0` + `Connection: close`.
    - Redirects: follows `Location` for common 3xx responses (up to 10). Still no auth/caching; switching away from a URL tab and back will re-fetch.
    - Syntax selection for URL tabs:
      - Query strings (`?...`) and fragments (`#...`) are ignored for syntax selection.
      - If the URL path has no extension (e.g. `https://example.com`), `sage` uses `Content-Type` as a best-effort syntax hint (e.g. `text/html` → `html`, `application/json` → `json`).
    - TLS verification:
      - `std::https::Connection.connect(...)` uses `std::tls::Session.client()` (no CA verification).
      - `std::https::Connection.connect_host(...)` uses `std::tls::Session.client_verified_system()` (system CA bundle + hostname via `set_hostname`).
      - `sage::netfile` uses `connect_host(...)`, so HTTPS opens are verified (and fail if no CA bundle can be loaded).
  - Ctrl-K find (external grep) **skips network tabs** because the paths are not local files.
  - TLS hostname (SNI): `sage` uses `std::https::Connection.connect_host(SocketAddrV4, hostname)` (which calls `std::tls::Session.set_hostname(...)`) so modern HTTPS servers handshake correctly without local mbedTLS bindings.
  - Packaging note (upstream-relevant): `sage` statically links the vendored mbedTLS 4.x archives via `sage/build.slk` (`../silk/vendor/lib/x64-linux/{libmbedtls.a,libmbedx509.a,libmbedcrypto.a}`) and uses the vendored headers (`../silk/vendor/include`). There is no runtime `DT_NEEDED` on `libmbedtls.so.*` and no RUNPATH requirement for TLS.
  - TLS 1.3 note (upstream-relevant): mbedTLS surfaces post-handshake `NewSessionTicket` as `MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET`; `std::tls` maps this to `WouldBlockRead` so higher-level callers can continue reading application data (unblocks `std::https` response reads on modern servers).
  - URL parsing: `sage::netfile` uses the stdlib WHATWG parser via a named import (`from "std/url"`) so we don't pull `std::url`’s internal helper aliases into the local namespace.
    - Upstream note (Silk `ebef419`): `std::url` uses module-specific helper alias names (e.g. `UrlVecU64`) so it no longer collides with `sage`’s internal `VecU64`.
      - Guidance: prefer named imports from `"std/url"` (only import what you need) to keep downstream namespaces small.
- **Viewport model**: scrolling is **byte-offset based** (`top_off`), so `sage`
  can page through extremely large files without building a full line table.
  Scrolling and rendering operate in **visual lines** (wrap-to-viewport), so
  `sage` remains fast even on extremely long single-line inputs (minified JSON,
  huge stack traces, etc).
- **Indexing (bounded memory)**: a background `task fn` scans for `\n` using
  `memchr` (in fixed-size byte chunks) and emits:
  - periodic progress (`scan_off`, `lines`), and
  - **checkpoint line-start offsets** every `INDEX_STRIDE` lines.
  These checkpoints are used to compute line numbers without storing every line start.
- **Rendering**: full redraw on input/resize; while indexing, `sage` updates the
  **status line only** on a timer so the indexer can keep draining without
  repainting the whole screen.
  For robustness across terminals/muxers, the renderer uses absolute cursor
  positioning (`CSI <row>;<col>H`) per line rather than relying on newline
  translation. To avoid worst-case “scan to EOF” behavior, `sage` bounds newline
  detection to ~`cols` bytes per visual row and wraps locally.
- **UX/Presentation**: colorized status bar + prompts (ANSI 256-color), in-app
  help overlay (`?`/`h`), and match highlighting for the current search hit.
  Highlighting is disabled in `--raw` mode to avoid mutating output bytes.
  - Colors are controlled by `--color <auto|always|never>` (auto respects `NO_COLOR`).
  - User defaults are loaded from `.sagerc` (see README) and can select a `--theme`
    plus palette overrides; CLI flags always win.
  - `--no-alt-screen` disables the alternate screen for terminals/muxers that behave oddly.
  - Mouse wheel scrolling uses xterm mouse reporting; this typically captures
    mouse clicks. `sage` uses left mouse drag for **in-app selection** (content-only;
    gutter excluded) and `Ctrl-C` copies via OSC 52. For terminal-native selection,
    use `Shift+drag` (or disable with `--no-mouse` / `mouse = false` in `.sagerc`).
- **Pager integration**: when stdin is a pipe (e.g. `man`), `sage` reads keys from
  `/dev/tty` (opened read+write for raw mode + window size) and ignores `SIGTERM`
  to avoid noisy `man: <pager>: Terminated` messages in some setups.
- **Search**:
  - default: literal substring search (chunked `memmem`)
  - `-i/--ignore-case`: ASCII case-insensitive literal search
  - `-R/--regex`: regex search via bundled runtime regex (ABI-safe wrapper in `sage::re`)
  - regex is compiled once per query and cached for repeated `n`
  - searching is incremental (chunked) so huge searches don’t freeze the UI;
    status shows `[searching]`, `Esc` cancels, and `[not found]` is surfaced
  - the `/` (or `Ctrl-F`) prompt keeps **in-session history** (Up/Down)
- **Find (Ctrl-K)**:
  - Opens a modal to search across the **currently open tab paths** (excludes stdin `"-"`).
  - Search is delegated to an external tool:
    - `.sagerc`: `find_cmd = <program and args...>` (aliases: `find`, `find-cmd`)
      - value is whitespace-split (no shell quoting); `find_cmd` runs as `<cmd...> <query> <files...>`
    - default (smart): `rg --vimgrep --` → `ag --vimgrep --` → `slg --` → `grep -inH --`
  - Output parsing expects a `grep`-like `path:line:` format (and `--vimgrep`’s `path:line:col:`); Enter/click jumps by file+line (column is ignored).
  - For safety, results are capped (`FIND_MAX_RESULTS = 5000`) and the modal shows “(truncated)” when hit.
  - Results rendering: the `path:line:col:` prefix is dimmed and the snippet is syntax-highlighted (when `--syntax` is enabled and color is on). The selected row keeps its background while still showing syntax colors.
    - Performance: syntax highlighters are cached in a small ring buffer while the modal is open to avoid reloading/compiling grammars on every redraw.
  - Modal controls: Up/Down/Tab/Shift-Tab (or `j/k`), mouse wheel scroll, Enter/click to jump, Esc to close.
- **Plugins (QuickJS / JavaScript)**:
  - **Build**:
    - `silk.toml` opts in to a build module (`build.slk`) which:
      - generates `build/gen/plugins_bootstrap.slk` from `src/sage/plugins/bootstrap.js` (embedded JS bootstrap), and
      - generates `build/gen/plugins_api_modules.slk` from `src/sage/plugins/api/**` (embedded `sage:*` ESM sources; no JS sources live in C), and
      - emits a manifest that compiles QuickJS into the `sage` executable via `[[target]].inputs` (`quickjs/*.c` + `src/native/sage_qjs.c`).
    - `sage:fetch` is backed by the native host HTTP/HTTPS client (sockets + mbedTLS), so the `sage` executable does **not** depend on libcurl (TLS is statically linked via the vendored mbedTLS archives).
    - Note: the generated bootstrap currently exports a `fn plugins_bootstrap_js() -> string`. Exported module-level `string` bindings also work across modules in native outputs, so this could be simplified if we want.
    - Note (build-module subset quirks observed):
      - `match` on `Result` should use unqualified `Ok(...)` / `Err(...)` patterns (qualified variant paths were rejected in this snapshot).
      - Calling `.drop()` requires a `let mut` local binding (since `drop` takes a mutable receiver).
  - **Runtime plugin discovery**:
    - Plugins are initialized only in the interactive TUI path (when stdout is a TTY). In pass-through mode (`stdout_tty=false`), `sage` returns early and does not run plugins.
    - Disable plugins (safe mode): `SAGE_NO_PLUGINS=1`, `--no-plugins`, or `.sagerc` `plugins = false`.
    - Loads `*.js` plugins in lexicographic order from:
      - `--plugins-dir <path>` / `.sagerc` `plugins_dir = <path>` (if set and `$SAGE_PLUGINS_DIR` is not set), else
      - `$SAGE_PLUGINS_DIR`, else
      - `$XDG_CONFIG_HOME/sage/plugins`, else
      - `$HOME/.config/sage/plugins`
    - Implementation safety: `sage` reserves plugin slots up-front so `SageQjsPlugin` pointers stay stable (QuickJS stores opaque pointers for interrupts and module loading).
  - **Execution limits (robustness)**:
    - Plugin execution is bounded to keep the UI responsive:
      - load/bootstrap budget: `SAGE_PLUGIN_LOAD_TIMEOUT_MS` (default `500`)
      - per-event budget: `SAGE_PLUGIN_EVENT_TIMEOUT_MS` (default `50`)
      - memory limit: `SAGE_PLUGIN_MEM_LIMIT_MB` (default `64`)
      - stack limit: `SAGE_PLUGIN_STACK_LIMIT_KB` (default `1024`)
    - These can also be set in `.sagerc` (unless the corresponding env var is set):
      - `plugin_load_timeout_ms`, `plugin_event_timeout_ms`, `plugin_mem_limit_mb`, `plugin_stack_limit_kb`
    - Each plugin runs in its own QuickJS runtime/context. If a plugin hits a timeout, `sage` disables **only that plugin**; other plugins keep running.
    - Listener exceptions are caught by the JS bootstrap and reported via `__sage_report_exception`; uncaught exceptions during bootstrap/load disable the offending plugin.
  - **Logging / diagnostics**:
    - Plugin errors and `console.*` output go to a log file (to avoid corrupting the TUI):
      - `$SAGE_PLUGIN_LOG` if set, else
      - `$XDG_CACHE_HOME/sage/plugins.log`, else
      - `$HOME/.cache/sage/plugins.log`
    - If the log file cannot be opened, plugin logs are suppressed by default (written to `/dev/null`).
    - `SAGE_PLUGIN_LOG_STDERR=1` forces plugin logs to stderr (debug only; may corrupt the TUI).
  - **JS API surface**:
    - Plugins are evaluated as ES modules (ESM) with a custom module resolver:
      - Built-in modules: `sage:fs`, `sage:path`, `sage:process`, `sage:env`, `sage:navigator`, `sage:performance`, `sage:crypto`, `sage:uuid`, `sage:url`, `sage:core/dom`, `sage:core/web`, `sage:fetch`
      - Relative imports are allowed for filesystem modules under the plugin’s directory tree.
      - Bare imports are rejected. Top-level await is not supported.
    - The embedded bootstrap extends `globalThis` (instead of a `sage` object):
      - `EventTarget`, `Event`, `CustomEvent`, `MessageEvent` are global.
      - `isSageRuntime` is a stable runtime check getter.
      - `queueMicrotask(fn)` is available.
      - `setTimeout/clearTimeout/setInterval/clearInterval` and `sleep(ms)` are available.
      - Event helpers:
        - `addEventListener/removeEventListener/dispatchEvent` receive Event objects (host events are `CustomEvent`s; payload is in `ev.detail`).
        - `on/once/off` call `fn(payload)` where payload is `CustomEvent.detail` or `MessageEvent.data`.
      - `console` has `log`, `debug`, `verbose`, `info`, `warn`, `error` filtered by `SAGE_CONSOLE_LEVEL` (`silent|off` disables; default is `warn` unless `--verbose` → `debug`).
      - `command(name, fn)` registers a custom `:<name>` command (async ok).
      - `exec(cmd)` enqueues a built-in or plugin `:` command to run on the next UI tick.
      - `navigator` is global (also available as `sage:navigator`).
      - `performance` is global (also available as `sage:performance`).
      - `crypto` is global (also available as `sage:crypto`).
      - `URL`, `URLSearchParams` are global (also available as `sage:url`).
      - `DOMException` and `structuredClone` are global (also available as `sage:core/dom`).
      - `fetch` + `Headers`/`Request`/`Response`/`FormData`/`Blob`/`ReadableStream`/`AbortController`/`AbortSignal` + `TextEncoder`/`TextDecoder`
    - File/process access is via built-in ESM modules:
      - `sage:fs`
        - `readFile(path, { encoding?, maxBytes? })` is restricted to local open tabs (and the plugin’s data dir).
        - `writeFile(name, data)`, `appendFile(name, data)`, `readdir()` operate on the plugin’s data dir only.
      - `sage:process`: `pid`, `ppid`, `cwd()`, `exec(cmd, { timeoutMs?, maxBytes? })` (Promise; resolves on UI idle ticks)
      - `sage:env`: `get/set/unset` for env vars in the current process
      - `sage:path`: minimal POSIX-y path helpers
      - `sage:navigator`: browser-like `navigator` (`userAgent`, versions)
      - `sage:performance`: `performance.now()` (monotonic) + `performance.timeOrigin`
      - `sage:crypto`: `crypto.getRandomValues(...)` + `crypto.randomUUID()` (libsodium-backed randomness)
      - `sage:uuid`: `v4()` + `v7([unixMs])` (v7 is time-ordered; matches RFC 9562 layout)
      - `sage:url`: WHATWG-style `URL` + `URLSearchParams` + `URL.parse`/`URL.canParse`
        - Implementation note (upstream-relevant): `sage:url` is a pure-JS URL Standard implementation focused on correctness for common cases and Node/WHATWG compatibility. It supports special-scheme parsing (including IPv4 canonicalization). Known gaps: no full TR46/IDNA mapping (punycode encode only) and no IPv6 parsing/normalization beyond bracketed literals.
      - `sage:core/dom`: DOM-ish host-free primitives (`DOMException`, `structuredClone`)
      - `sage:core/web`: host-free WHATWG-ish web primitives (`Headers`/`Request`/`Response`/`FormData`/`Blob`/`ReadableStream`/`AbortController`/`AbortSignal` + `TextEncoder`/`TextDecoder`)
      - `sage:fetch`: WHATWG-ish `fetch(...)` backed by the native host (plus `timeoutMs`/`maxBytes`/`followRedirects` options)
        - Native host transport supports `Content-Length`, `Transfer-Encoding: chunked`, and body-to-EOF responses; `maxBytes` is enforced for all paths.
        - TLS verification uses the system CA bundle by default (supports `SSL_CERT_FILE`, else tries common Linux paths); set `SAGE_FETCH_INSECURE=1` to disable verification (encryption only).
    - Known JS runtime gaps (current): no `crypto.subtle` yet.
    - Plugins run without QuickJS `std`/`os` modules linked in the current host.
    - Events currently emitted by the host:
      - `open`: `{ path, tab, tab_count }` (tabs are 1-indexed in events)
      - `tab_change`: `{ from, to, tab_count }`
      - `search`: `{ query, regex, ignore_case }`
      - `copy`: `{ bytes }` (OSC 52 success only)
      - `quit`
    - Plugin exceptions are reported to the plugin log with a `sage[plugin:<path>]` prefix (stack printed in `--verbose` mode). Runtime UI may also show `[plugin err]` on the status line.
- **Syntax highlighting (optional)**:
  - Syntax source files live in `XDG_CONFIG_HOME/sage/syntax/` (`.sublime-syntax`, `.tmLanguage`, `.tmLanguage.json`, `.cson`).
    - `sage --compile-cache` parses a **subset** of those formats and writes a compact
      binary cache to `XDG_CACHE_HOME/sage/syntax/` plus an `index.bin` for fast lookup.
      - Sources are scanned **recursively** (e.g. `syntax/textmate/*.tmLanguage`, `syntax/atom/*.cson`).
      - Cache file names are flattened from relative paths to avoid collisions
        (e.g. `textmate/c.tmLanguage` → `textmate_c.sagec`).
    - At runtime, `sage` loads the matching cache by basename/extension and compiles a
      small fixed set of regexes (one per token kind). Highlighting is done per
      viewport segment, so it stays bounded even on huge files.
      - The compiled cache also stores **range flags** extracted from `begin`/`end` rules
        for common comment/string delimiters (`//…`, `/*…*/`, `'…"…"`, `` `…` ``).
        When present, `sage` uses a lightweight state machine to paint these ranges
        across wrapped segments before applying regex rules, preventing false
        keyword matches inside strings/comments.
    - **Capture blocks are intentionally ignored** during compilation:
      patterns under `captures` / `beginCaptures` / `endCaptures` / `matchCaptures`
      are *context-bound* in real TextMate engines. Treating them as global regex
      rules causes severe false positives (e.g. “match any identifier” patterns
      that were meant only inside a specific `begin`/`end` rule). `sage` skips
      these blocks to keep highlighting stable and predictable in the current subset.
    - **TextMate `include` is not fully resolved yet**:
      many `.tmLanguage` grammars are *deltas* that `include` a base scope (e.g.
      `source.c++` includes `source.c`) to inherit common rules like comments,
      strings, preprocessor directives, and core keywords. The current compiler
      subset does not follow `include: <scope>` across files, so those inherited
      rules would otherwise be missing.
      - Pragmatic workaround (runtime): when loading a `*c++.sagec` cache, `sage`
        also loads the `c` cache (when available) and merges its rules + range flags.
      - Future direction: index `scopeName` and resolve `include` properly at
        compile time so merged caches are explicit and stable.
    - **Extension key collisions**: multiple syntaxes may claim the same extension
      (e.g. C vs C++). `sage`’s index lookup is “last match wins”, so later entries
      in the compiled `index.bin` override earlier ones. `sage --list-syntax` will
      list unique keys only; use `sage --verbose --list-syntax` to see collisions
      and the selected key→cache mapping.
    - **Case-sensitive keys (with fallback)**: `index.bin` keys are stored exactly
      as declared by the source grammars (so `.C` can be distinct from `.c`).
      At runtime, lookup tries the exact key first, then falls back to ASCII-lowercased
      matching for convenience.

## Concurrency model (current Silk subset)

- `task fn` runs on OS threads (pthread-based on hosted `linux/x86_64`). Default is thread-per-call; `attr(task=pool)` / `attr(task_pool)` schedules on the global task pool (queue-based + simple work stealing); keep worker counts low.
- `sage` uses one background indexer thread plus the UI thread.
- `std::sync::ChannelBorrow(T)` is a non-owning view and does not close on drop;
  use a sentinel, explicitly `close()`, or prefer `std::sync::ChannelSender(T)` for auto-close.
  `ChannelBorrow(T).wait_fd()` exposes a pollable fd for integrating channel wakeups into `poll(2)` / `std::runtime::event_loop`.
- Cancellation uses `std::sync::CancellationToken` plus a closed channel to unblock senders.

## What’s missing in Silk (today) for “ideal pager” ergonomics

These are the main language/runtime/stdlib gaps that shaped `sage`’s design:

- **Async runtime is hosted + partial**: on hosted `linux/x86_64`, Silk supports
  `async fn main`, `await`, and basic timers + fd readiness (`std::runtime::event_loop`)
  via the bundled runtime (`libsilk_rt`). The explicit event loop
  `Handle`/`init`/`wake`/`poll` surface exists (single global instance).
  - Waiting on multiple sources is still limited, but you *can* wait on a TTY fd
    and a channel in one await via `std::sync::ChannelBorrow(T).wait_fd()` +
    `std::runtime::event_loop::fd_wait_readable2(tty_fd, chan_fd)`.
  - Still missing: a general `select`-style primitive for waiting on an arbitrary
    set of fds/channels/timers in one call (beyond the `*_readable2` helper), so
    apps still build small polling loops when they need N-way waits.
- **Task runtime is OS-thread-based**: by default, calling a `task fn` spawns a dedicated OS thread in the hosted subset.
  - `attr(task=pool)` / `attr(task_pool)` schedules calls on the global task pool (queue-based + simple work stealing), but tasks are still OS-thread-backed so unbounded fan-out is expensive.
- **FFI ergonomics / safety**:
  - `sage` currently ships its own libc surface in `src/sage/os.slk`:
    - `memchr(3)`, `memrchr(3)`, `memmem(3)` — hot-path scanning (newlines + literal search)
  - Note: `isatty(3)` / TTY size / raw mode are used via `std::runtime::io` shims.
  - Note (stdlib temp files): as of this snapshot (**2026-02-27**),
    `std::runtime::fs::mkstemp(template_ptr)` is available for hosted native outputs; `sage` uses
    it for stdin + network spooling and unlinks the temp file after creation.
  - Note (stdlib mmap): as of `silk` `ebef419`, `std::runtime::fs::{mmap_readonly, munmap}` and
    `std::fs::File.mmap_readonly*` work in native backend subset outputs; `sage` uses
    `std::runtime::fs::mmap_readonly` + `std::runtime::fs::munmap` now (so it no longer binds
    `mmap(2)` directly).
  - Note (stdlib tty): as of `silk` `ebef419`, `std::io::{isatty, tty_size, tty_open, tty_raw_mode}`
    work in native backend subset outputs; `sage` uses the `std::runtime::io` layer directly today.
  - Note: `memmem`/`memrchr` are GNU/libc extensions. For portability, `std::arrays::ByteSlice`
    already provides pure-Silk `find_u8`, `rfind_u8`, and `find_bytes` (memmem-like semantics);
    a dedicated, optimized stdlib byte-search module would still be valuable.
  - `src/main.slk` binds `signal(2)` to `SIG_IGN` `SIGTERM` when acting as a pager,
    purely as a UX workaround for noisy `man: <pager>: Terminated` messages in some setups.
  - Concern: `ext` bindings are powerful but very low-level; today you must hand-write
    signatures, constants, ioctl numbers, and (sometimes) struct layouts/offsets.
    A small typo (wrong type width, wrong struct size, wrong offset) can silently
    produce memory unsafety.
  - `std::xml` currently depends on an external shim archive (libxml2-backed) that
    may not be present in minimal builds without running an extra vendor build step
    (e.g. `zig build deps`). For `sage`, we kept `.tmLanguage` parsing in pure Silk
    to avoid adding a hard dependency on that shim at pager build time.
  - `std::fs` / `std::path` / `std::strings` provide higher-level APIs, but many of
    them return `Drop` types and may allocate; for `sage`’s hot-path + toolchain
    sharp-edge avoidance, we currently prefer the lower-level
    `std::runtime::posix::*` primitives plus a small, explicit byte-buffer helper
    surface (`src/sage/buf.slk`).
  - Stdlib coverage (helps avoid custom libc bindings):
    - `std::ffi::c` provides canonical C scalar aliases (`c_int`, `size_t`, `ssize_t`, etc) plus C-string helpers.
    - `std::runtime::fs` / `std::fs::File.mmap_readonly*` cover file mapping.
    - `std::io` covers `/dev/tty` open, raw mode, and window size.
  - Still desirable for apps like `sage`:
    - fast byte-search primitives (or a blessed `memchr`/`memmem` module)
    - signals (ignore/handle/wait) with safe, portable abstractions
    - a bindgen-style workflow and ABI validation hooks
- **Signal ergonomics**:
  - Silk `ext` supports passing **non-capturing** function pointers (use `c_fn`
    for FFI-safe callback pointers), but **capturing closures** are not supported.
    For `sage`, we currently avoid custom handlers anyway and only use `SIG_IGN`
    as a UX workaround when acting as a pager.
  - There’s no stdlib SIGWINCH hook, so resize handling is polling-based today
    (a `sigwait`-style watcher thread is possible, but requires manual `sigset_t`
    bindings/layout).
- **Regex scalability**: the bundled runtime regex validates `input_len <= INT32_MAX`,
  so true “whole file” regex search on `>2GiB` inputs requires chunking/workarounds.
- **Regex value ergonomics**: `regexp` is still a non-owning `{ ptr, len }` view,
  but `std::runtime::regex` + `std::regex` provide compile/exec/search helpers so
  downstream code rarely needs bespoke wrappers just to run matches. `sage` still
  keeps a small wrapper (`sage::re::RegExp`) for caching + UI integration.
- **Unicode width / grapheme handling**: no standard “terminal cell width”
  library yet; correct rendering of wide characters is still future work.
- **Backend subset constraints (observed)**:
  `std::runtime::posix::fs::opendir` is observed to be more reliable when directory
  paths end with `/` in this snapshot (compile-cache recursion probes `path + "/"`).
  `match` works well as an expression, but some statement forms are limited to
  typed-error handling.
  `if let` / `else let` is supported in the current subset; prefer it over
  `match` for simple optional/result destructuring. (Rust-style `let ... else`
  is still not available.)

## Minimal repros (backend subset)

- No known `sage`-driven minimal repros are outstanding in this snapshot.

- Previously observed backend-subset constraints that are now **covered by passing fixtures** in
  `silk/tests/silk/` (see `pass_sage_min_repro_*.slk`):
  - scalar borrows (e.g. `&bool`, `&int`)
  - function expressions with scalar `&T` params
  - nested field assignment for scalar leaf fields
  - `Vector(Task(T))` push patterns (move-only handles)

## Security / terminal safety

- Default rendering **sanitizes control bytes** to avoid terminal injection, but
  passes through ANSI SGR sequences (`ESC [ ... m`) so colored output (like
  `man` and `git diff`) renders correctly. Use `--no-ansi` to fully sanitize.
- `--raw` disables sanitization and can execute arbitrary terminal escape
  sequences from content; use only when you trust the input.
- By default, `sage` treats input as binary when it sees a NUL (`\0`) byte
  (first 1KiB for mapped files; anywhere for stdin) and refuses to open it
  (“binary input”); pass `--binary` (or set `.sagerc`: `binary = true`) to allow.
- When `sage` is used as a pager but cannot enable raw mode (no usable TTY),
  it falls back to a non-interactive safe stream (same sanitization rules).

## Current limitations

- Display is ASCII-cell oriented (no full Unicode width handling yet).
- Wrap-to-viewport is always enabled (no horizontal scrolling yet, and no wrap toggle).
- Regex search is chunked to support `>2GiB`, but pathological patterns that
  require matches spanning *very* large chunk boundaries may still be missed
  without a true streaming regex engine.
- Resize handling is polling-based (no SIGWINCH hook).

## Quick perf probe

```sh
cd sage
silk build --package .
./build/bin/sage --index-only <path>
```
