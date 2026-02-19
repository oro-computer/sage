'use strict'

// Demonstrates `sage:process` + `sage:env` (ESM builtin modules).
//
// Console output is filtered by `SAGE_CONSOLE_LEVEL`:
// - default: `warn` (or `debug` when `sage` runs with `--verbose`)
//
// Install:
//   mkdir -p ~/.config/sage/plugins
//   rsync -a examples/plugins/ ~/.config/sage/plugins/
//
// Commands:
//   :cwd
//   :env-get <NAME>
//   :proc-demo
//   :sh <cmd...>

import process from 'sage:process'
import env from 'sage:env'

on('open', async (p) => {
  console.debug('process(open)', 'pid=', process.pid, 'ppid=', process.ppid)
  try {
    console.debug('process(open)', 'cwd=', await process.cwd(), 'path=', p && p.path ? p.path : '')
  } catch (e) {
    console.warn('process(open)', 'cwd failed:', String(e))
  }
})

command('cwd', async () => {
  console.info('cwd', await process.cwd())
})

command('env-get', async (args) => {
  const key = String(args || '').trim()
  if (!key) return
  const v = await env.get(key)
  console.info('env-get', key, '=', v === undefined ? '(undefined)' : JSON.stringify(v))
})

command('proc-demo', async () => {
  const res = await process.exec("printf 'hello from sage:process\\n'")
  console.info('proc-demo', 'code=', res.code, 'stdout=', JSON.stringify(res.stdout))
  if (res.stderr) {
    console.warn('proc-demo', 'stderr=', JSON.stringify(res.stderr))
  }
})

command('sh', async (args) => {
  const cmd = String(args || '').trim()
  if (!cmd) return
  const res = await process.exec(cmd, { timeoutMs: 5000, maxBytes: 512 * 1024 })
  console.info('sh', 'code=', res.code, 'stdout=', JSON.stringify(res.stdout))
  if (res.stderr) {
    console.warn('sh', 'stderr=', JSON.stringify(res.stderr))
  }
})
