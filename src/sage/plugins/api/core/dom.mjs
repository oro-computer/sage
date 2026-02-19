// DOM-ish primitives for `sage` plugins (host-free).
//
// - DOMException (subset, Error-based)
// - structuredClone (useful subset; no transfer list support)

import { defineGlobal } from 'sage:core/global'

const domExceptionNameToCode = Object.freeze({
  IndexSizeError: 1,
  DOMStringSizeError: 2,
  HierarchyRequestError: 3,
  WrongDocumentError: 4,
  InvalidCharacterError: 5,
  NoDataAllowedError: 6,
  NoModificationAllowedError: 7,
  NotFoundError: 8,
  NotSupportedError: 9,
  InUseAttributeError: 10,
  InvalidStateError: 11,
  SyntaxError: 12,
  InvalidModificationError: 13,
  NamespaceError: 14,
  InvalidAccessError: 15,
  ValidationError: 16,
  TypeMismatchError: 17,
  SecurityError: 18,
  NetworkError: 19,
  AbortError: 20,
  URLMismatchError: 21,
  QuotaExceededError: 22,
  TimeoutError: 23,
  InvalidNodeTypeError: 24,
  DataCloneError: 25,
})

function domExceptionCode(name) {
  const k = String(name || '')
  return Object.prototype.hasOwnProperty.call(domExceptionNameToCode, k) ? domExceptionNameToCode[k] : 0
}

export class DOMException extends Error {
  constructor(message, name) {
    super(message == null ? '' : String(message))
    this.name = name == null ? 'Error' : String(name)
    this.code = domExceptionCode(this.name)

    if (typeof Error.captureStackTrace === 'function') {
      Error.captureStackTrace(this, DOMException)
    }
  }
}

Object.defineProperties(DOMException, {
  INDEX_SIZE_ERR: { value: 1 },
  DOMSTRING_SIZE_ERR: { value: 2 },
  HIERARCHY_REQUEST_ERR: { value: 3 },
  WRONG_DOCUMENT_ERR: { value: 4 },
  INVALID_CHARACTER_ERR: { value: 5 },
  NO_DATA_ALLOWED_ERR: { value: 6 },
  NO_MODIFICATION_ALLOWED_ERR: { value: 7 },
  NOT_FOUND_ERR: { value: 8 },
  NOT_SUPPORTED_ERR: { value: 9 },
  INUSE_ATTRIBUTE_ERR: { value: 10 },
  INVALID_STATE_ERR: { value: 11 },
  SYNTAX_ERR: { value: 12 },
  INVALID_MODIFICATION_ERR: { value: 13 },
  NAMESPACE_ERR: { value: 14 },
  INVALID_ACCESS_ERR: { value: 15 },
  VALIDATION_ERR: { value: 16 },
  TYPE_MISMATCH_ERR: { value: 17 },
  SECURITY_ERR: { value: 18 },
  NETWORK_ERR: { value: 19 },
  ABORT_ERR: { value: 20 },
  URL_MISMATCH_ERR: { value: 21 },
  QUOTA_EXCEEDED_ERR: { value: 22 },
  TIMEOUT_ERR: { value: 23 },
  INVALID_NODE_TYPE_ERR: { value: 24 },
  DATA_CLONE_ERR: { value: 25 },
})

function dataCloneError(message) {
  return new DOMException(message == null ? 'DataCloneError' : String(message), 'DataCloneError')
}

function throwDataClone(message) {
  throw dataCloneError(message)
}

function cloneAny(value, seen) {
  if (value == null) return value

  const t = typeof value
  if (t === 'string' || t === 'number' || t === 'boolean' || t === 'bigint' || t === 'undefined') return value

  if (t === 'function' || t === 'symbol') {
    throwDataClone('structuredClone: cannot clone ' + t)
  }

  if (seen.has(value)) return seen.get(value)

  if (typeof ArrayBuffer !== 'undefined' && value instanceof ArrayBuffer) {
    const out = value.slice(0)
    seen.set(value, out)
    return out
  }

  if (typeof ArrayBuffer !== 'undefined' && typeof ArrayBuffer.isView === 'function' && ArrayBuffer.isView(value)) {
    if (value instanceof DataView) {
      const buf = value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength)
      const out = new DataView(buf)
      seen.set(value, out)
      return out
    }
    try {
      const C = value.constructor
      const out = new C(value)
      seen.set(value, out)
      return out
    } catch (_) {
      throwDataClone('structuredClone: cannot clone typed array')
    }
  }

  if (value instanceof Date) {
    const out = new Date(value.getTime())
    seen.set(value, out)
    return out
  }

  if (value instanceof RegExp) {
    const out = new RegExp(value.source, value.flags)
    out.lastIndex = value.lastIndex
    seen.set(value, out)
    return out
  }

  if (value instanceof Map) {
    const out = new Map()
    seen.set(value, out)
    value.forEach((v, k) => {
      out.set(cloneAny(k, seen), cloneAny(v, seen))
    })
    return out
  }

  if (value instanceof Set) {
    const out = new Set()
    seen.set(value, out)
    value.forEach((v) => {
      out.add(cloneAny(v, seen))
    })
    return out
  }

  // Try to preserve some common built-ins when present in this runtime.
  const URLCtor = globalThis.URL
  if (typeof URLCtor === 'function') {
    try {
      if (value instanceof URLCtor) {
        const out = new URLCtor(value.href)
        seen.set(value, out)
        return out
      }
    } catch (_) {
      // ignore
    }
  }

  const URLSPCtor = globalThis.URLSearchParams
  if (typeof URLSPCtor === 'function') {
    try {
      if (value instanceof URLSPCtor) {
        const out = new URLSPCtor(value.toString())
        seen.set(value, out)
        return out
      }
    } catch (_) {
      // ignore
    }
  }

  const HeadersCtor = globalThis.Headers
  if (typeof HeadersCtor === 'function') {
    try {
      if (value instanceof HeadersCtor) {
        const out = new HeadersCtor(value)
        seen.set(value, out)
        return out
      }
    } catch (_) {
      // ignore
    }
  }

  if (value instanceof Error) {
    const name = value.name || 'Error'
    const message = value.message || ''
    let out
    try {
      const C = value.constructor
      out = typeof C === 'function' ? new C(message) : new Error(message)
    } catch (_) {
      out = new Error(message)
    }
    out.name = String(name)
    seen.set(value, out)

    const keys = Reflect.ownKeys(value)
    for (let i = 0; i < keys.length; i++) {
      const k = keys[i]
      if (k === 'name' || k === 'message' || k === 'stack') continue
      const desc = Object.getOwnPropertyDescriptor(value, k)
      if (!desc) continue
      if (!Object.prototype.hasOwnProperty.call(desc, 'value')) {
        throwDataClone('structuredClone: cannot clone accessor property')
      }
      desc.value = cloneAny(desc.value, seen)
      try {
        Object.defineProperty(out, k, desc)
      } catch (_) {
        // ignore
      }
    }
    return out
  }

  const proto = Object.getPrototypeOf(value)
  const allow =
    proto === Object.prototype ||
    proto === null ||
    proto === Array.prototype ||
    proto === Boolean.prototype ||
    proto === Number.prototype ||
    proto === String.prototype

  if (!allow) {
    throwDataClone('structuredClone: unsupported object type')
  }

  let out
  if (proto === Boolean.prototype) {
    out = new Boolean(value.valueOf())
  } else if (proto === Number.prototype) {
    out = new Number(value.valueOf())
  } else if (proto === String.prototype) {
    out = new String(value.valueOf())
  } else {
    out = Array.isArray(value) ? [] : Object.create(proto)
  }

  seen.set(value, out)

  const keys = Reflect.ownKeys(value)
  for (let i = 0; i < keys.length; i++) {
    const k = keys[i]
    const desc = Object.getOwnPropertyDescriptor(value, k)
    if (!desc) continue
    if (!Object.prototype.hasOwnProperty.call(desc, 'value')) {
      throwDataClone('structuredClone: cannot clone accessor property')
    }
    desc.value = cloneAny(desc.value, seen)
    Object.defineProperty(out, k, desc)
  }

  return out
}

export function structuredClone(value, options) {
  const o = options && typeof options === 'object' ? options : null
  if (o && Object.prototype.hasOwnProperty.call(o, 'transfer')) {
    const t = o.transfer
    if (t && typeof t.length === 'number' && t.length > 0) {
      throw dataCloneError('structuredClone: transfer is not supported in sage')
    }
  }

  const seen = new Map()
  return cloneAny(value, seen)
}

export function installGlobals() {
  defineGlobal('DOMException', DOMException)
  defineGlobal('structuredClone', structuredClone)
}

export default Object.freeze({
  DOMException,
  structuredClone,
  installGlobals,
})
