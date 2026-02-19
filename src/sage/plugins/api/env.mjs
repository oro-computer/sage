import { requireHostFunction } from 'sage:internal/host'

const getHost = requireHostFunction('__sage_env_get')
const setHost = requireHostFunction('__sage_env_set')
const unsetHost = requireHostFunction('__sage_env_unset')

export async function get(name) {
  const v = getHost(String(name))
  return v === undefined ? undefined : String(v)
}

export async function set(name, value, opts) {
  const o = opts && typeof opts === 'object' ? opts : null
  const overwrite = o && Object.prototype.hasOwnProperty.call(o, 'overwrite') ? !!o.overwrite : true
  return setHost(String(name), String(value), overwrite) === 0
}

export async function unset(name) {
  return unsetHost(String(name)) === 0
}

export default Object.freeze({ get, set, unset })
