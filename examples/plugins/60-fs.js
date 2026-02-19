'use strict'

// Demonstrates `sage:fs` (ESM builtin module):
// - reading local files currently open in `sage` tabs
// - reading/writing per-plugin data files (persistent state)
//
// Console output is filtered by `SAGE_CONSOLE_LEVEL`:
// - default: `warn` (or `debug` when `sage` runs with `--verbose`)
//
// Install:
//   mkdir -p ~/.config/sage/plugins
//   cp examples/plugins/60-fs.js ~/.config/sage/plugins/
//
// Commands:
//   :fs-demo
//   :fs-peek <path>

import fs from 'sage:fs'

function nowIso() {
  try {
    return new Date().toISOString()
  } catch (_) {
    return ''
  }
}

function firstLine(s) {
  const i = s.indexOf('\n')
  return (i >= 0 ? s.slice(0, i) : s).slice(0, 160)
}

on('open', async (p) => {
  // Safe, bounded peek at the currently open file.
  try {
    const txt = await fs.readFile(p.path, { encoding: 'utf8', maxBytes: 64 * 1024 })
    console.info('fs(open)', p.path, 'firstLine=', JSON.stringify(firstLine(txt)))
  } catch (e) {
    // This can fail for very large files (bounded reads) or non-local tabs.
    console.warn('fs(open)', p.path, 'peek failed:', String(e))
  }
})

command('fs-demo', async () => {
  const dir = fs.dataDir()
  console.info('fs-demo', 'dataDir=', dir)

  // Text state file.
  const text = `updated=${nowIso()}\n`
  await fs.appendFile('demo.txt', text)
  const cur = await fs.readDataFile('demo.txt', { encoding: 'utf8', maxBytes: 64 * 1024 })
  console.info('fs-demo', 'demo.txt=', JSON.stringify(cur))

  // Binary state file.
  const bytes = new Uint8Array([0x73, 0x61, 0x67, 0x65, 0x0a]) // "sage\n"
  await fs.writeFile('demo.bin', bytes)
  const back = await fs.readDataFile('demo.bin', { maxBytes: 1024 })
  console.info('fs-demo', 'demo.bin bytes=', back.length)

  // List data dir contents (flat names).
  const names = await fs.readdir()
  console.info('fs-demo', 'readdir=', JSON.stringify(names))
})

command('fs-peek', async (args) => {
  const p = String(args || '').trim()
  if (!p) return
  try {
    const txt = await fs.readFile(p, { encoding: 'utf8', maxBytes: 64 * 1024 })
    console.info('fs-peek', p, 'firstLine=', JSON.stringify(firstLine(txt)))
  } catch (e) {
    console.warn('fs-peek', p, 'failed:', String(e))
  }
})
