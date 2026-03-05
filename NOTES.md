# Silk support expectations (from `sage`)

## Snapshot (last verified)

- Date: **2026-03-05**
- `sage`: `9eae523214b9`
- `silk`: `382cc9e11f3a` (`silk (ABI) 0.2.0`)
- Baseline: linux/glibc x86_64 (hosted POSIX runtime)
- Verified (sage build):
  - `silk test --package .` (20 tests)
  - `silk build --package .`
  - `ldd ./build/bin/sage` (no libcurl, libssh2, or libmbedtls DT_NEEDED)

## Silk should support (required by Sage)

- Filesystem mapping + temp files: `std::runtime::fs::{mmap_readonly, munmap, mkstemp, unlink}`
- TTY I/O: `std::io::{isatty, tty_size, tty_open, tty_raw_mode}`
- Concurrency: `task fn`, `attr(task=pool)` / `attr(task_pool)`, `SILK_TASK_POOL_THREADS=<n>`
- Sync primitives: `std::sync::{ChannelBorrow(T).wait_fd, ChannelSender(T), CancellationToken}`
- Async event loop surface: `std::runtime::event_loop::{sleep_ms, fd_wait_readable, fd_wait_readable2, fd_wait_readable_any, fd_wait_readable_any_timeout_ms, fd_wait_writable}`
- Vendored headers for C inputs: `silk build` adds `<prefix>/lib/silk/vendor/include` to the include path when compiling `.c` inputs
- URL parsing: `std::url::{URL.parse, parse_with_base}` + `URL.{href,scheme,host,port,path,query,fragment}`
- HTTP: `std::http` response parsing supports `Content-Length`, `Transfer-Encoding: chunked`, and body-to-EOF
- HTTPS: `std::https::Connection.connect_host(SocketAddrV4, hostname)` and `connect_host_v6(SocketAddrV6, hostname)` + `std::tls::Session.client_verified_system()` + `Session.set_hostname(...)`
- TLS 1.3: `std::tls` handles `MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET` as `WouldBlockRead`
- SSH/SFTP: `std::ssh2` supports SFTP reads and OpenSSH `known_hosts` verification helpers

## Silk should support (missing / needed)

- DNS / connect-by-hostname in `std::net` (A/AAAA; v4/v6 selection)
- Reliable async coroutine spawning in multi-threaded processes
- Portable signal wait/handling beyond Linux `signalfd`
- Regex engine support for input sizes > `INT32_MAX` (or a streaming/iterator API)
- Terminal cell width + grapheme segmentation for correct TUI rendering
- Bindgen-style tooling + ABI validation for `ext` declarations
