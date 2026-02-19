import { requireHostFunction } from 'sage:internal/host'

const pidHost = requireHostFunction('__sage_process_pid')
const ppidHost = requireHostFunction('__sage_process_ppid')
const cwdHost = requireHostFunction('__sage_process_cwd')
const execHost = requireHostFunction('__sage_process_exec')

export const pid = pidHost()
export const ppid = ppidHost()

export async function cwd() {
  return String(cwdHost())
}

export async function exec(cmd, opts) {
  const o = opts && typeof opts === 'object' ? opts : null
  const timeoutMs = o && typeof o.timeoutMs === 'number' ? o.timeoutMs : 30_000
  const maxBytes = o && typeof o.maxBytes === 'number' ? o.maxBytes : 1024 * 1024
  return await execHost(String(cmd), timeoutMs, maxBytes)
}

export default Object.freeze({ pid, ppid, cwd, exec })
