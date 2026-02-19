'use strict'

// Demonstrates relative ESM imports in plugins (filesystem modules) + `sage:path`.
//
// Install:
//   mkdir -p ~/.config/sage/plugins
//   rsync -a examples/plugins/ ~/.config/sage/plugins/
//
// Commands:
//   :imports-demo

import path from 'sage:path'
import { banner, shout } from './72-imports_util.mjs'

on('open', (p) => {
  if (!p || !p.path) return
  console.debug('imports(open)', banner, 'basename=', path.basename(p.path))
})

command('imports-demo', async (args) => {
  const s = String(args || '').trim()
  console.info('imports-demo', shout(s || 'hello'), banner)
})
