'use strict'

// Demonstrates providing a command that other plugins can call via `exec(...)`.
//
// Console output is filtered by `SAGE_CONSOLE_LEVEL` (default: `warn`).
//
// Install:
//   mkdir -p ~/.config/sage/plugins
//   cp examples/plugins/50-provider.js ~/.config/sage/plugins/
//   cp examples/plugins/51-consumer.js ~/.config/sage/plugins/
//
// Commands:
//   :provider [message]
//   :provider-goto <n>   (executes numeric line jump)

function toInt(s) {
  const n = parseInt(String(s || '').trim(), 10)
  return Number.isFinite(n) ? n : null
}

command('provider', (args) => {
  const msg = String(args || '').trim()
  console.info('provider', msg || '(no message)')
})

command('provider-goto', (args) => {
  const n = toInt(args)
  if (n === null) {
    console.warn('provider-goto', 'expected an integer')
    return
  }
  exec(String(n))
})
