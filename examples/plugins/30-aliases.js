'use strict'

// Demonstrates defining short alias commands that call built-in `:` commands.
//
// Install:
//   mkdir -p ~/.config/sage/plugins
//   cp examples/plugins/30-aliases.js ~/.config/sage/plugins/
//
// Commands:
//   :n         (alias for :bn)
//   :p         (alias for :bp)
//   :top       (alias for :0)
//   :tfirst    (alias for :t 1)
//   :tlast     (alias for :t 0)
//   :go <n>    (alias for :<n> line jump; supports 0 and negatives)

function alias(name, cmd) {
  command(name, () => {
    exec(cmd)
  })
}

alias('n', 'bn')
alias('p', 'bp')
alias('top', '0')
alias('tfirst', 't 1')
alias('tlast', 't 0')

command('go', (args) => {
  const a = String(args || '').trim()
  if (!a) return

  // Forward to numeric jump: `:0`, `:12`, `:-2`, etc.
  exec(a)
})
