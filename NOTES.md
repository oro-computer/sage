# `sage` Notes (performance + constraints)

## Snapshot (last verified)

- Date: **2026-02-19**
- `sage`: `f20d95d` (base commit; notes reflect subsequent local edits)
- `silk`: `440f569` (`silk (ABI) 0.2.0`)
- Baseline: linux/glibc x86_64 (hosted POSIX runtime)
- Verified:
  - `silk test --package .` (20 tests)
  - `silk build --package .` (build module enabled)
  - `./build/bin/sage --index-only http://example.com`
  - `./build/bin/sage --index-only https://example.com`
  - interactive `./build/bin/sage -v --no-alt-screen --no-plugins https://example.com` (quit with `q`)
  - interactive `SAGE_CONSOLE_LEVEL=debug SAGE_PLUGIN_LOG=/tmp/sage-plugin-smoke.log ./build/bin/sage --no-alt-screen --plugins-dir ./examples/plugins ./NOTES.md` (ran `:fs-demo`, `:url-demo`, `:fetch https://example.com/`, `:fetch-abort https://example.com/`, `:crypto-demo`, quit with `q`)

## Architecture

- **I/O**: files are read via `std::runtime::fs::mmap_readonly` (POSIX `mmap(PROT_READ|MAP_PRIVATE)` on hosted) for zero-copy paging.
- **Network open (HTTP/HTTPS)**:
  - `sage` accepts `http://...` / `https://...` paths as **read-only** inputs (e.g. `sage https://example.com/file.txt`).
  - `ssh://...` is currently treated as a “network path” but is **not implemented yet** (future: `std::ssh2`).
  - Implementation: fetches over TCP/TLS, spools bytes to an **unlinked temp file** (`mkstemp` + `unlink`), then `mmap`s it so the rest of the pager pipeline stays zero-copy.
  - DNS: includes a tiny IPv4 `A` resolver (UDP) that reads the first `nameserver` from `/etc/resolv.conf` and sets a best-effort `SO_RCVTIMEO` (2s). Fallbacks: `1.1.1.1`, then `8.8.8.8`.
  - Limitations (current):
    - **IPv4-only** (no IPv6 sockets; `std::https` only exposes `connect(SocketAddrV4)` today).
    - **No `Transfer-Encoding: chunked`** support (blocked by `std::http` / `std::https` response parser rejecting non-`identity` transfer encodings).
    - **No redirects**, auth, or caching; switching away from a URL tab and back will re-fetch.
    - HTTPS is currently **unauthenticated** in the stdlib snapshot (`mbedTLS` is configured with `VERIFY_NONE`), so this is transport encryption only.
    - Ctrl-K find (external grep) **skips network tabs** because the paths are not local files.
  - SNI note (upstream-relevant): many modern HTTPS servers require **SNI** to complete the handshake. The shipped `std::https::Connection.connect(SocketAddrV4)` API has no way to set a hostname, so `sage` currently calls `mbedtls_ssl_set_hostname()` directly via an `ext` binding (using `std::tls::Session.ssl`) before handshaking. Ideal upstream fix: expose a safe `std::tls::Session.set_hostname()` and plumb hostname through `std::https`.
  - Packaging note (upstream-relevant): `std::tls` introduces a runtime DT_NEEDED on `libmbedtls.so.14`. `sage/build.slk` bundles a local `build/lib/libmbedtls.so.14` and sets RUNPATH `$ORIGIN/../lib` so `sage` runs without a system mbedTLS install **when** the legacy runtime’s pinned static archives exist (currently under `../legacy-runtime/build/x86_64-desktop/mbedtls/build/library/`). Also observed: `silk/vendor/deps/mbedtls` is mbedTLS 4.x and does not export the 2.x-era symbols currently bound by `std::tls` (soname 14).
  - Note (Silk quirk observed): importing `std::url` currently collided with `sage`’s `VecU64` (the compiler resolved `VecU64` as `std::vector::Vector(u64)`), so URL parsing in `sage` is currently a minimal in-module parser instead of using `std::url`.
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
	    - `sage:fetch` is backed by libcurl in the native host, so the `sage` executable declares DT_NEEDED on `libcurl.so.4` (and `libpthread.so.0`).
	    - Note (current subset limitation): the generated bootstrap exports a `fn plugins_bootstrap_js() -> string` instead of an exported module-level `string` binding.
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
      - Built-in modules: `sage:fs`, `sage:path`, `sage:process`, `sage:env`, `sage:navigator`, `sage:performance`, `sage:crypto`, `sage:url`, `sage:core/dom`, `sage:core/web`, `sage:fetch`
      - Relative imports are allowed for filesystem modules under the plugin’s directory tree.
        Bare imports are rejected. Top-level await is not supported.
	    - The embedded bootstrap extends `globalThis` (instead of a `sage` object):
	      - `EventTarget`, `Event`, `CustomEvent`, `MessageEvent` are global.
	      - `isSageRuntime` is a stable runtime check getter.
        - `queueMicrotask(fn)` is available.
	      - Event helpers:
	        - `addEventListener/removeEventListener/dispatchEvent` receive Event objects (host events are `CustomEvent`s; payload is in `ev.detail`).
	        - `on/once/off` call `fn(payload)` where payload is `CustomEvent.detail` or `MessageEvent.data`.
      - `console` has `log`, `debug`, `verbose`, `info`, `warn`, `error` filtered by `SAGE_CONSOLE_LEVEL`
        (`silent|off` disables; default is `warn` unless `--verbose` → `debug`).
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
      - `sage:crypto`: `crypto.getRandomValues(...)` + `crypto.randomUUID()`
	      - `sage:url`: WHATWG-style `URL` + `URLSearchParams` + `URL.parse`/`URL.canParse`
	        - Implementation note (upstream-relevant): `sage:url` is a pure-JS URL Standard implementation focused on correctness for common cases and Node/WHATWG compatibility. It supports special-scheme parsing (including IPv4 canonicalization). Known gaps: no full TR46/IDNA mapping (punycode encode only) and no IPv6 parsing/normalization beyond bracketed literals.
	      - `sage:core/dom`: DOM-ish host-free primitives (`DOMException`, `structuredClone`)
	      - `sage:core/web`: host-free WHATWG-ish web primitives (`Headers`/`Request`/`Response`/`FormData`/`Blob`/`ReadableStream`/`AbortController`/`AbortSignal` + `TextEncoder`/`TextDecoder`)
	      - `sage:fetch`: WHATWG-ish `fetch(...)` backed by the native host (plus `timeoutMs`/`maxBytes`/`followRedirects` options)
	    - Known JS runtime gaps (current): no timers (`setTimeout`/`setInterval`), no `crypto.subtle` yet.
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

- `task fn` runs on OS threads (pthread-based on hosted `linux/x86_64`; not a work-stealing pool); keep worker counts low.
- `sage` uses one background indexer thread plus the UI thread.
- `std::sync::ChannelBorrow(T)` is a non-owning view and does not close on drop;
  use a sentinel, explicitly `close()`, or prefer `std::sync::ChannelSender(T)` for auto-close.
- Cancellation uses `std::sync::CancellationToken` plus a closed channel to unblock senders.

## What’s missing in Silk (today) for “ideal pager” ergonomics

These are the main language/runtime/stdlib gaps that shaped `sage`’s design:

- **Async runtime is hosted + partial**: on hosted `linux/x86_64`, Silk supports
  `async fn main`, `await`, and basic timers + fd readiness (`std::runtime::event_loop`)
  via the bundled runtime (`libsilk_rt`). However, the explicit event loop
  `Handle`/`init`/`poll` surface is still a stub, and there’s no stdlib primitive
  that can wait on a TTY fd *and* a `std::sync::Channel` in one call. `sage`
  therefore mixes “render”, “input”, and “index progress” with `poll()` + timeouts
  rather than a clean runtime-backed `select` loop.
- **Task runtime is thread-based**: `task fn` uses OS threads in the hosted
  subset (not a work-stealing pool), so spawning many workers can be expensive.
- **Move-only task handles don’t fit `std::vector` yet**: `Task(T)` / `Promise(T)`
  are single-use handles consumed by `yield`/`await` and are **not copyable**.
  Today, `std::vector::Vector(T)` stores elements via assignment (copy), so
  `Vector(Task(T))` fails to compile with `E2034: cannot copy a Task/Promise handle`.
  This is why highly-parallel programs sometimes use fixed slot locals (e.g.
  `t0..t31`) instead of a growable vector. Upstream fix: add move-aware
  `Vector(T)` operations (`push_move`/`set_move`/`take`), or allow move-into-memory
  assignment for move-only types.
- **FFI ergonomics / safety**:
  - `sage` currently ships its own libc surface in `src/sage/os.slk`:
    - `memchr(3)`, `memrchr(3)`, `memmem(3)` — hot-path scanning (newlines + literal search)
    - `mkstemp(3)` — stdin spooling to an unlinked temp file
  - Note: `isatty(3)` / TTY size / raw mode are used via `std::runtime::io` shims.
  - Note (stdlib mmap): as of `silk` `ea09e87`, `std::runtime::fs::{mmap_readonly, munmap}` and
    `std::fs::File.mmap_readonly*` work in native backend subset outputs; `sage` uses
    `std::runtime::fs::mmap_readonly` + `std::runtime::fs::munmap` now (so it no longer binds
    `mmap(2)` directly).
  - Note (stdlib tty): as of `silk` `ea09e87`, `std::io::{isatty, tty_size, tty_open, tty_raw_mode}`
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
  - Expectation: the Silk stdlib should provide a stable, cross-platform way to do the above
    so applications don’t need custom libc bindings:
    - canonical C types (`c_int`, `size_t`, `ssize_t`, etc) and checked wrappers
    - file mapping + temp-file spooling APIs usable from native outputs
    - terminal raw mode + window size
    - fast byte-search primitives (or a blessed `memchr`/`memmem` module)
    - signals (ignore/handle/wait) with safe, portable abstractions
    - ideally a bindgen-style workflow and ABI validation hooks
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
- **Regex value ergonomics (backend subset)**: the built-in `regexp` type is an
  opaque `{ ptr, len }` view with no stdlib accessors, making custom wrappers
  awkward. `sage` represents compiled regex bytecode as plain `{ ptr, len }`
  (`sage::re::RegExp`) so it can be stored in structs safely and efficiently.
- **Borrow/assignment ergonomics**: this subset does not currently allow taking a
  mutable borrow of a struct field (e.g. `mut cfg.theme`) or assigning to nested
  fields through a borrowed parent (e.g. `cfg.theme.status_bg = ...`); `sage`
  works around this by copy-modify-replace for theme overrides.
- **Unicode width / grapheme handling**: no standard “terminal cell width”
  library yet; correct rendering of wide characters is still future work.
- **Backend subset constraints (observed)**:
  References to scalar types (e.g. `&bool`) are currently rejected (`E2006: invalid borrow`);
  prefer passing scalars by value and returning results.
  `std::runtime::posix::fs::opendir` is observed to be more reliable when directory
  paths end with `/` in this snapshot (compile-cache recursion probes `path + "/"`).
  Exported module-level `string` bindings are not usable across modules in the
  current backend subset (`E4001`: unsupported expression `Name`); prefer exporting
  a `fn` that returns the string literal instead.
  `match` works well as an expression, but some statement forms are limited to
  typed-error handling.
  `if let` / `else let` is supported in the current subset; prefer it over
  `match` for simple optional/result destructuring. (Rust-style `let ... else`
  is still not available.)

## Minimal repros (backend subset)

- `E4001` (cross-module exported `string`):

```slk
// a.slk
module a;
export let S: string = "hello";

// b.slk
module b;
import { S } from "./a.slk";

export fn use () -> string {
  return S; // error[E4001] unsupported expression: `Name`
}
```

Note: `silk check` may accept this; the error is observed at native codegen time
(e.g. `silk build`).

Workaround: export a `fn` that returns the string literal instead.

- `E2006` (borrowing scalars like `&bool`):

```slk
fn main () -> int {
  var b: bool = true;
  let _y: &bool = &b; // error[E2006] invalid borrow
  return 0;
}
```

Note: borrowing non-scalar structs (e.g. `&MyStruct`) works in this snapshot; the rejection appears specific to scalar borrows.

- `E2056` (function expressions + `&T` params):

```slk
fn main () -> int {
  let f = fn (x: &int) -> int {
    return 0;
  };
  return 0;
}
```

- `E2005` (nested field assignment):

```slk
struct A {
  x: int,
}

struct B {
  a: A,
}

fn main () -> int {
  var b: B = B{ a: A{ x: 0 } };
  b.a.x = 1; // error[E2005] invalid assignment
  return 0;
}
```

- `E2034` (`Vector(Task(T))`):

```slk
import std::vector;

type VecTask = std::vector::Vector(Task(int));

task fn work () -> int {
  return 0;
}

fn main () -> int {
  let v_r = VecTask.init(1);
  let mut v: VecTask = match (v_r) { Ok(x) => x, Err(_) => VecTask.empty() };
  let t: Task(int) = work();
  let _ = v.push(t); // error[E2034] cannot copy a Task/Promise handle
  return 0;
}
```

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

## Current limitations (WIP)

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
