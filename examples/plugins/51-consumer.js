'use strict'

// Demonstrates calling built-in `:` commands and other plugin commands by
// enqueueing commands with `exec(...)`.
//
// Console output is filtered by `SAGE_CONSOLE_LEVEL` (default: `warn`).
//
// Install:
//   mkdir -p ~/.config/sage/plugins
//   cp examples/plugins/50-provider.js ~/.config/sage/plugins/
//   cp examples/plugins/51-consumer.js ~/.config/sage/plugins/
//
// Commands:
//   :consumer [message]   (calls :provider <message>)
//   :consumer-demo        (calls :provider, then :provider-goto 0)

command('consumer', (args) => {
  const msg = String(args || '').trim()
  exec(`provider ${msg}`.trim())
})

command('consumer-demo', () => {
  exec('provider hello-from-consumer')
  // `:0` jumps to the top of the file (line 1).
  exec('provider-goto 0')
})
