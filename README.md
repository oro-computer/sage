# `sage` — a fast, ergonomic terminal pager (WIP)

`sage` is a terminal pager inspired by `most`, written in Silk.

![](assets/screenshot.png)

## Build

```sh
cd sage
silk build --package .
```

Binary: `./build/bin/sage`

## Usage

```sh
./build/bin/sage <path>
cat <path> | ./build/bin/sage
./build/bin/sage --index-only <path>
./build/bin/sage -i <path>        # case-insensitive search
./build/bin/sage -R <path>        # regex search (std::regex)
./build/bin/sage --color never <path>
./build/bin/sage --no-alt-screen <path>
```

## Keys

- `q` / `Ctrl-C` — quit
- `j` / `Down` — down one visual line (wrap-to-viewport)
- `k` / `Up` — up one visual line
- `Space` / `PageDown` — down one page
- `b` / `PageUp` — up one page
- `→` / `Right` — down one page
- `←` / `Left` — up one page
- `g` / `Home` — top
- `G` / `End` — bottom
- `/` — search (starts searching; jumps to first match when found)
- `n` — next match
- `Esc` — cancel search
- `?` / `h` — help

## Notes

- By default, `sage` sanitizes control bytes to avoid terminal injection, but
  passes through ANSI SGR (`ESC [ ... m`) so colored output (like `man`) renders
  (use `--no-ansi` to fully sanitize).
- When stdout is not a TTY, `sage` falls back to streaming bytes to stdout.
- Line numbers are computed using a background indexer; on very large inputs they
  may briefly show `?` until the indexer scans past the current viewport.
- The status bar, prompts, and match highlighting use ANSI colors (256-color SGR).
  Use `--color never` or set `NO_COLOR=1` to disable styling.
