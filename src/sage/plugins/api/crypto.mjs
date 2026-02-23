import { requireHostFunction } from 'sage:internal/host'
import { defineGlobal } from 'sage:core/global'

const randomBytesHost = requireHostFunction('__sage_crypto_random_bytes')
const uuidV4Host = requireHostFunction('__sage_uuid_v4')

function isTypedArray(v) {
  return (
    typeof ArrayBuffer !== 'undefined' &&
    typeof ArrayBuffer.isView === 'function' &&
    ArrayBuffer.isView(v) &&
    !(v instanceof DataView)
  )
}

function isIntegerTypedArray(v) {
  return (
    v instanceof Int8Array ||
    v instanceof Uint8Array ||
    v instanceof Uint8ClampedArray ||
    v instanceof Int16Array ||
    v instanceof Uint16Array ||
    v instanceof Int32Array ||
    v instanceof Uint32Array
  )
}

function quotaExceededError(bytes) {
  const e = new Error('crypto.getRandomValues: requested ' + bytes + ' bytes (max 65536)')
  e.name = 'QuotaExceededError'
  return e
}

export class Crypto {
  constructor() {
    // No state.
  }

  getRandomValues(typedArray) {
    if (!isTypedArray(typedArray) || !isIntegerTypedArray(typedArray)) {
      throw new TypeError('crypto.getRandomValues(typedArray): typedArray must be an integer TypedArray')
    }

    const n = typedArray.byteLength >>> 0
    if (n > 65536) {
      throw quotaExceededError(n)
    }

    const ab = randomBytesHost(n)
    if (!(ab instanceof ArrayBuffer)) {
      throw new Error('crypto.getRandomValues: host returned non-ArrayBuffer')
    }
    const src = new Uint8Array(ab)
    const dst = new Uint8Array(typedArray.buffer, typedArray.byteOffset, typedArray.byteLength)
    dst.set(src)
    return typedArray
  }

  randomUUID() {
    return String(uuidV4Host())
  }
}

export const crypto = new Crypto()

export function installGlobals() {
  defineGlobal('Crypto', Crypto)
  defineGlobal('crypto', crypto)
}

export default crypto
