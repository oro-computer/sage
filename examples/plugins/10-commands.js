'use strict'

// Demonstrates plugin-defined `:` commands and invoking built-in `:` commands
// from JavaScript via `exec(...)`.
//
// Console output is filtered by `SAGE_CONSOLE_LEVEL`:
// - default: `warn` (or `debug` when `sage` runs with `--verbose`)
//
// Install:
//   mkdir -p ~/.config/sage/plugins
//   cp examples/plugins/10-commands.js ~/.config/sage/plugins/
//
// Commands:
//   :hello [top|next|prev]
//   :cycle <n>    (enqueues :bn / :bp up to 50 times)

function toInt(s, def) {
  const n = parseInt(String(s || '').trim(), 10)
  return Number.isFinite(n) ? n : def
}

command('hello', (args) => {
  const a = String(args || '').trim()
  console.info('hello', a)

  if (a === 'top') {
    // Jump to top of the file (same as typing `:0`).
    exec('0')
  } else if (a === 'next') {
    // Next tab/buffer.
    exec('bn')
  } else if (a === 'prev') {
    // Previous tab/buffer.
    exec('bp')
  }
})

command('cycle', (args) => {
  const n = toInt(args, 1)
  const cmd = n < 0 ? 'bp' : 'bn'
  const times = Math.min(Math.abs(n), 50)

  for (let i = 0; i < times; i++) {
    exec(cmd)
  }
})
