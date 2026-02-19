import { requireHostFunction } from 'sage:internal/host'
import { defineGlobal } from 'sage:core/global'

const randomBytesHost = requireHostFunction('__sage_crypto_random_bytes')

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

function hex2(b) {
  return (b + 0x100).toString(16).slice(1)
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
    const b = new Uint8Array(16)
    this.getRandomValues(b)
    // RFC 4122 v4: set version and variant bits.
    b[6] = (b[6] & 0x0f) | 0x40
    b[8] = (b[8] & 0x3f) | 0x80
    return (
      hex2(b[0]) +
      hex2(b[1]) +
      hex2(b[2]) +
      hex2(b[3]) +
      '-' +
      hex2(b[4]) +
      hex2(b[5]) +
      '-' +
      hex2(b[6]) +
      hex2(b[7]) +
      '-' +
      hex2(b[8]) +
      hex2(b[9]) +
      '-' +
      hex2(b[10]) +
      hex2(b[11]) +
      hex2(b[12]) +
      hex2(b[13]) +
      hex2(b[14]) +
      hex2(b[15])
    )
  }
}

export const crypto = new Crypto()

export function installGlobals() {
  defineGlobal('Crypto', Crypto)
  defineGlobal('crypto', crypto)
}

export default crypto
