'use strict'

// Demonstrates small command "macros" by queueing multiple built-in `:` commands
// via `exec(...)`.
//
// Install:
//   mkdir -p ~/.config/sage/plugins
//   cp examples/plugins/40-macros.js ~/.config/sage/plugins/
//
// Commands:
//   :rotate <n>  (positive -> :bn, negative -> :bp; capped to 200)
//   :tour        (cycles through all tabs and returns to the start)

let tabCount = 0

function toInt(s, def) {
  const n = parseInt(String(s || '').trim(), 10)
  return Number.isFinite(n) ? n : def
}

on('open', (p) => {
  tabCount = Number(p && p.tab_count ? p.tab_count : 0) || tabCount
})

on('tab_change', (p) => {
  tabCount = Number(p && p.tab_count ? p.tab_count : 0) || tabCount
})

command('rotate', (args) => {
  const n = toInt(args, 0)
  if (!n) return

  const cmd = n < 0 ? 'bp' : 'bn'
  const times = Math.min(Math.abs(n), 200)
  for (let i = 0; i < times; i++) {
    exec(cmd)
  }
})

command('tour', () => {
  const n = tabCount | 0
  if (n < 2) return

  // `:bn` is cyclic; N steps returns to the original tab after visiting all.
  for (let i = 0; i < n; i++) {
    exec('bn')
  }
})
