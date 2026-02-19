'use strict'

// Logs high-level `sage` events (ESM).
//
// Console output is filtered by `SAGE_CONSOLE_LEVEL`:
// - default: `warn` (or `debug` when `sage` runs with `--verbose`)
// - examples: `SAGE_CONSOLE_LEVEL=info`, `SAGE_CONSOLE_LEVEL=debug`
// Logs are written to `$SAGE_PLUGIN_LOG` (if set), else `$XDG_CACHE_HOME/sage/plugins.log`,
// else `~/.cache/sage/plugins.log`.
//
// Install:
//   mkdir -p ~/.config/sage/plugins
//   cp examples/plugins/00-log-events.js ~/.config/sage/plugins/

import navigator from 'sage:navigator'

console.debug('navigator.userAgent', navigator.userAgent)

globalThis.on('open', (p) => {
  console.info('open', p.path, `tab=${p.tab}/${p.tab_count}`)
})

globalThis.on('tab_change', (p) => {
  console.info('tab_change', `${p.from} -> ${p.to}`, `tabs=${p.tab_count}`)
})

globalThis.on('search', (p) => {
  console.debug('search', JSON.stringify({ q: p.query, regex: p.regex, ignore_case: p.ignore_case }))
})

globalThis.on('copy', (p) => {
  console.info('copy', `${p.bytes} bytes`)
})

globalThis.on('quit', () => {
  console.info('quit')
})
