import { requireHostFunction } from 'sage:internal/host'

const v4Host = requireHostFunction('__sage_uuid_v4')
const v7Host = requireHostFunction('__sage_uuid_v7')

export function v4() {
  return String(v4Host())
}

export function v7(unixMs) {
  if (unixMs === undefined || unixMs === null) {
    return String(v7Host())
  }
  return String(v7Host(Number(unixMs)))
}

export default Object.freeze({ v4, v7 })
