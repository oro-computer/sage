// Web-ish primitives used by `sage` plugins.
//
// This module intentionally contains no host bindings. Host-specific behavior
// (like performing the actual HTTP request) lives in `sage:fetch`.

import { defineGlobal } from 'sage:core/global'
import { DOMException } from 'sage:core/dom'

function isArrayBuffer(v) {
  return typeof ArrayBuffer !== 'undefined' && v instanceof ArrayBuffer
}

function isTypedArray(v) {
  return (
    typeof ArrayBuffer !== 'undefined' &&
    typeof ArrayBuffer.isView === 'function' &&
    ArrayBuffer.isView(v) &&
    !(v instanceof DataView)
  )
}

function utf8Encode(str) {
  str = String(str)
  const out = []
  for (let i = 0; i < str.length; i++) {
    let c = str.charCodeAt(i)
    if (c < 0x80) {
      out.push(c)
      continue
    }
    if (c < 0x800) {
      out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f))
      continue
    }
    if (c >= 0xd800 && c <= 0xdbff) {
      const next = str.charCodeAt(i + 1)
      if (next >= 0xdc00 && next <= 0xdfff) {
        const cp = 0x10000 + ((c - 0xd800) << 10) + (next - 0xdc00)
        out.push(
          0xf0 | (cp >> 18),
          0x80 | ((cp >> 12) & 0x3f),
          0x80 | ((cp >> 6) & 0x3f),
          0x80 | (cp & 0x3f)
        )
        i++
        continue
      }
      out.push(0xef, 0xbf, 0xbd)
      continue
    }
    if (c >= 0xdc00 && c <= 0xdfff) {
      out.push(0xef, 0xbf, 0xbd)
      continue
    }
    out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f))
  }
  return new Uint8Array(out)
}

function utf8Decode(bytes) {
  if (bytes == null) return ''
  const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes)
  let out = ''
  let i = 0
  while (i < b.length) {
    const b0 = b[i]
    if (b0 < 0x80) {
      out += String.fromCharCode(b0)
      i++
      continue
    }
    if (b0 >= 0xc2 && b0 <= 0xdf) {
      if (i + 1 >= b.length) {
        out += '\uFFFD'
        break
      }
      const b1 = b[i + 1]
      if ((b1 & 0xc0) !== 0x80) {
        out += '\uFFFD'
        i++
        continue
      }
      const cp = ((b0 & 0x1f) << 6) | (b1 & 0x3f)
      out += String.fromCharCode(cp)
      i += 2
      continue
    }
    if (b0 >= 0xe0 && b0 <= 0xef) {
      if (i + 2 >= b.length) {
        out += '\uFFFD'
        break
      }
      const b1 = b[i + 1]
      const b2 = b[i + 2]
      if ((b1 & 0xc0) !== 0x80 || (b2 & 0xc0) !== 0x80) {
        out += '\uFFFD'
        i++
        continue
      }
      const cp = ((b0 & 0x0f) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f)
      if (cp >= 0xd800 && cp <= 0xdfff) {
        out += '\uFFFD'
        i += 3
        continue
      }
      // Reject overlong forms.
      if (cp < 0x800) {
        out += '\uFFFD'
        i += 3
        continue
      }
      out += String.fromCharCode(cp)
      i += 3
      continue
    }
    if (b0 >= 0xf0 && b0 <= 0xf4) {
      if (i + 3 >= b.length) {
        out += '\uFFFD'
        break
      }
      const b1 = b[i + 1]
      const b2 = b[i + 2]
      const b3 = b[i + 3]
      if ((b1 & 0xc0) !== 0x80 || (b2 & 0xc0) !== 0x80 || (b3 & 0xc0) !== 0x80) {
        out += '\uFFFD'
        i++
        continue
      }
      const cp =
        ((b0 & 0x07) << 18) | ((b1 & 0x3f) << 12) | ((b2 & 0x3f) << 6) | (b3 & 0x3f)
      // Reject overlong forms and invalid ranges.
      if (cp < 0x10000 || cp > 0x10ffff) {
        out += '\uFFFD'
        i += 4
        continue
      }
      const u = cp - 0x10000
      const hi = 0xd800 | (u >> 10)
      const lo = 0xdc00 | (u & 0x3ff)
      out += String.fromCharCode(hi, lo)
      i += 4
      continue
    }
    out += '\uFFFD'
    i++
  }
  return out
}

export const TextEncoder =
  typeof globalThis.TextEncoder === 'function'
    ? globalThis.TextEncoder
    : class TextEncoder {
        encode(str) {
          return utf8Encode(str)
        }
      }

export const TextDecoder =
  typeof globalThis.TextDecoder === 'function'
    ? globalThis.TextDecoder
    : class TextDecoder {
        constructor(label) {
          const l = label == null ? 'utf-8' : String(label).toLowerCase()
          if (l !== 'utf-8' && l !== 'utf8') {
            throw new RangeError('TextDecoder: unsupported encoding: ' + l)
          }
          this.encoding = 'utf-8'
        }
        decode(input) {
          if (input == null) return ''
          if (typeof input === 'string') return String(input)
          if (isArrayBuffer(input)) return utf8Decode(new Uint8Array(input))
          if (isTypedArray(input)) return utf8Decode(new Uint8Array(input.buffer, input.byteOffset, input.byteLength))
          if (input instanceof Uint8Array) return utf8Decode(input)
          return utf8Decode(input)
        }
      }

function concatBytes(chunks) {
  let total = 0
  for (let i = 0; i < chunks.length; i++) total += chunks[i].byteLength
  const out = new Uint8Array(total)
  let off = 0
  for (let i = 0; i < chunks.length; i++) {
    const c = chunks[i]
    out.set(c, off)
    off += c.byteLength
  }
  return out
}

const blobBytesSymbol = Symbol('sage.blob.bytes')

function toBytes(data) {
  if (data == null) return new Uint8Array(0)
  if (data instanceof Uint8Array) return data
  if (isTypedArray(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
  if (isArrayBuffer(data)) return new Uint8Array(data)
  if (typeof data === 'string') return utf8Encode(data)
  if (data instanceof Blob) return data[blobBytesSymbol]()
  throw new TypeError('Body must be a string, ArrayBuffer, TypedArray, Uint8Array, Blob, or FormData')
}

export class ReadableStream {
  #chunks
  #closed
  constructor(source) {
    this.locked = false
    this.#chunks = []
    this.#closed = false
    if (source != null) {
      if (source instanceof Uint8Array) {
        this.#chunks = [source]
      } else if (isArrayBuffer(source)) {
        this.#chunks = [new Uint8Array(source)]
      } else if (isTypedArray(source)) {
        this.#chunks = [new Uint8Array(source.buffer, source.byteOffset, source.byteLength)]
      } else if (Array.isArray(source)) {
        this.#chunks = source.map((c) => (c instanceof Uint8Array ? c : toBytes(c)))
      } else {
        throw new TypeError('ReadableStream: unsupported source')
      }
    }
  }
  getReader() {
    if (this.locked) throw new TypeError('ReadableStream is locked')
    this.locked = true
    const stream = this
    return {
      async read() {
        if (stream.#closed) return { done: true, value: undefined }
        const chunk = stream.#chunks.shift()
        if (!chunk) {
          stream.#closed = true
          return { done: true, value: undefined }
        }
        if (stream.#chunks.length === 0) {
          stream.#closed = true
        }
        return { done: false, value: chunk }
      },
      async cancel() {
        stream.#chunks = []
        stream.#closed = true
      },
      releaseLock() {
        stream.locked = false
      },
    }
  }
  async cancel() {
    this.#chunks = []
    this.#closed = true
  }
  [Symbol.asyncIterator]() {
    const reader = this.getReader()
    return {
      async next() {
        const r = await reader.read()
        if (r.done) {
          reader.releaseLock()
          return { done: true, value: undefined }
        }
        return { done: false, value: r.value }
      },
    }
  }
}

export class Blob {
  #data
  constructor(parts, opts) {
    const o = opts && typeof opts === 'object' ? opts : null
    const type = o && typeof o.type === 'string' ? o.type : ''
    this.type = String(type).toLowerCase()

    const p = parts == null ? [] : Array.isArray(parts) ? parts : [parts]
    const chunks = []
    for (let i = 0; i < p.length; i++) {
      const v = p[i]
      if (v == null) continue
      if (typeof v === 'string') {
        chunks.push(utf8Encode(v))
        continue
      }
      if (v instanceof Uint8Array) {
        chunks.push(v)
        continue
      }
      if (isTypedArray(v)) {
        chunks.push(new Uint8Array(v.buffer, v.byteOffset, v.byteLength))
        continue
      }
      if (isArrayBuffer(v)) {
        chunks.push(new Uint8Array(v))
        continue
      }
      if (v instanceof Blob) {
        chunks.push(v[blobBytesSymbol]())
        continue
      }
      chunks.push(utf8Encode(String(v)))
    }
    this.#data = concatBytes(chunks)
    this.size = this.#data.byteLength
  }

  [blobBytesSymbol]() {
    return this.#data
  }

  async arrayBuffer() {
    return this.#data.buffer.slice(this.#data.byteOffset, this.#data.byteOffset + this.#data.byteLength)
  }

  async text() {
    const dec = new TextDecoder('utf-8')
    return dec.decode(this.#data)
  }

  stream() {
    return new ReadableStream(this.#data)
  }

  slice(start, end, type) {
    const size = this.#data.byteLength
    let s = start == null ? 0 : Number(start)
    let e = end == null ? size : Number(end)
    if (Number.isNaN(s)) s = 0
    if (Number.isNaN(e)) e = size
    if (s < 0) s = Math.max(size + s, 0)
    if (e < 0) e = Math.max(size + e, 0)
    s = Math.min(Math.max(s, 0), size)
    e = Math.min(Math.max(e, 0), size)
    const t = type == null ? this.type : String(type).toLowerCase()
    const chunk = this.#data.slice(s, e)
    return new Blob([chunk], { type: t })
  }
}

function escapeFormDataValue(v) {
  return String(v).replace(/[\r\n"]/g, '_')
}

const encodeMultipartSymbol = Symbol('sage.formdata.encodeMultipart')

export class FormData {
  #entries
  constructor() {
    this.#entries = []
  }
  append(name, value, filename) {
    const n = String(name)
    if (value instanceof Blob) {
      const fn = filename == null ? 'blob' : String(filename)
      this.#entries.push({ name: n, value, filename: fn })
    } else {
      this.#entries.push({ name: n, value: String(value), filename: null })
    }
  }
  set(name, value, filename) {
    this.delete(name)
    this.append(name, value, filename)
  }
  get(name) {
    const n = String(name)
    for (let i = 0; i < this.#entries.length; i++) {
      const e = this.#entries[i]
      if (e.name === n) return e.value
    }
    return null
  }
  getAll(name) {
    const n = String(name)
    const out = []
    for (let i = 0; i < this.#entries.length; i++) {
      const e = this.#entries[i]
      if (e.name === n) out.push(e.value)
    }
    return out
  }
  has(name) {
    const n = String(name)
    for (let i = 0; i < this.#entries.length; i++) {
      if (this.#entries[i].name === n) return true
    }
    return false
  }
  delete(name) {
    const n = String(name)
    for (let i = this.#entries.length - 1; i >= 0; i--) {
      if (this.#entries[i].name === n) this.#entries.splice(i, 1)
    }
  }
  *entries() {
    for (let i = 0; i < this.#entries.length; i++) {
      const e = this.#entries[i]
      yield [e.name, e.value]
    }
  }
  *keys() {
    for (const [k] of this.entries()) yield k
  }
  *values() {
    for (const [, v] of this.entries()) yield v
  }
  [Symbol.iterator]() {
    return this.entries()
  }
  forEach(fn, thisArg) {
    if (typeof fn !== 'function') throw new TypeError('FormData.forEach: fn must be a function')
    for (const [k, v] of this.entries()) fn.call(thisArg, v, k, this)
  }

  [encodeMultipartSymbol]() {
    const boundary =
      '----sageformdata-' + Math.random().toString(16).slice(2) + Math.random().toString(16).slice(2)
    const chunks = []

    for (let i = 0; i < this.#entries.length; i++) {
      const e = this.#entries[i]
      chunks.push(utf8Encode(`--${boundary}\r\n`))
      if (e.value instanceof Blob) {
        const name = escapeFormDataValue(e.name)
        const filename = escapeFormDataValue(e.filename || 'blob')
        const ctype = e.value.type || 'application/octet-stream'
        chunks.push(
          utf8Encode(
            `Content-Disposition: form-data; name="${name}"; filename="${filename}"\r\nContent-Type: ${ctype}\r\n\r\n`
          )
        )
        chunks.push(e.value[blobBytesSymbol]())
        chunks.push(utf8Encode('\r\n'))
      } else {
        const name = escapeFormDataValue(e.name)
        chunks.push(utf8Encode(`Content-Disposition: form-data; name="${name}"\r\n\r\n`))
        chunks.push(utf8Encode(String(e.value)))
        chunks.push(utf8Encode('\r\n'))
      }
    }
    chunks.push(utf8Encode(`--${boundary}--\r\n`))
    return {
      contentType: `multipart/form-data; boundary=${boundary}`,
      body: concatBytes(chunks),
    }
  }
}

function isTChar(code) {
  // token = 1*tchar; tchar per RFC 7230.
  if (code >= 48 && code <= 57) return true // 0-9
  if (code >= 65 && code <= 90) return true // A-Z
  if (code >= 97 && code <= 122) return true // a-z
  switch (code) {
    case 33: // !
    case 35: // #
    case 36: // $
    case 37: // %
    case 38: // &
    case 39: // '
    case 42: // *
    case 43: // +
    case 45: // -
    case 46: // .
    case 94: // ^
    case 95: // _
    case 96: // `
    case 124: // |
    case 126: // ~
      return true
    default:
      return false
  }
}

function normalizeHeaderName(name) {
  const n = String(name)
  if (!n) throw new TypeError('Invalid header name')
  for (let i = 0; i < n.length; i++) {
    if (!isTChar(n.charCodeAt(i))) throw new TypeError('Invalid header name: ' + n)
  }
  return n.toLowerCase()
}

function normalizeHeaderValue(value) {
  const v = String(value)
  if (v.includes('\r') || v.includes('\n')) throw new TypeError('Invalid header value')
  return v.trim()
}

export class Headers {
  #map
  #order
  constructor(init) {
    this.#map = Object.create(null) // lower -> string[]
    this.#order = [] // insertion order of lower keys

    if (init == null) return
    if (init instanceof Headers) {
      init.forEach((v, k) => this.append(k, v))
      return
    }
    if (Array.isArray(init)) {
      for (let i = 0; i < init.length; i++) {
        const pair = init[i]
        if (!pair) continue
        const k = pair[0]
        const v = pair[1]
        if (k == null) continue
        this.append(k, v)
      }
      return
    }
    const iter = init && init[Symbol.iterator]
    if (typeof iter === 'function') {
      for (const pair of init) {
        if (!pair) continue
        const k = pair[0]
        const v = pair[1]
        if (k == null) continue
        this.append(k, v)
      }
      return
    }
    if (typeof init === 'object') {
      const keys = Object.keys(init)
      for (let i = 0; i < keys.length; i++) {
        const k = keys[i]
        this.append(k, init[k])
      }
      return
    }
    throw new TypeError('Headers: unsupported init')
  }

  append(name, value) {
    const k = normalizeHeaderName(name)
    const v = normalizeHeaderValue(value)
    if (!this.#map[k]) {
      this.#map[k] = []
      this.#order.push(k)
    }
    this.#map[k].push(v)
  }

  set(name, value) {
    const k = normalizeHeaderName(name)
    const v = normalizeHeaderValue(value)
    if (!this.#map[k]) {
      this.#order.push(k)
    }
    this.#map[k] = [v]
  }

  get(name) {
    const k = normalizeHeaderName(name)
    const arr = this.#map[k]
    if (!arr || arr.length === 0) return null
    return arr.join(', ')
  }

  has(name) {
    const k = normalizeHeaderName(name)
    return !!this.#map[k]
  }

  delete(name) {
    const k = normalizeHeaderName(name)
    if (!this.#map[k]) return
    delete this.#map[k]
    const i = this.#order.indexOf(k)
    if (i >= 0) this.#order.splice(i, 1)
  }

  forEach(fn, thisArg) {
    if (typeof fn !== 'function') throw new TypeError('Headers.forEach: fn must be a function')
    for (const [k, v] of this.entries()) fn.call(thisArg, v, k, this)
  }

  *entries() {
    for (let i = 0; i < this.#order.length; i++) {
      const k = this.#order[i]
      const v = this.get(k)
      if (v != null) yield [k, v]
    }
  }

  *keys() {
    for (const [k] of this.entries()) yield k
  }

  *values() {
    for (const [, v] of this.entries()) yield v
  }

  [Symbol.iterator]() {
    return this.entries()
  }
}

function cloneBytes(bytes) {
  if (!bytes) return null
  return bytes.slice ? bytes.slice() : new Uint8Array(bytes)
}

const requestState = new WeakMap()
const responseState = new WeakMap()

function initBody(state, body) {
  if (body == null) {
    state.bodyBytes = null
    state.bodyStream = null
    return
  }
  if (body instanceof FormData) {
    const enc = body[encodeMultipartSymbol]()
    state.bodyBytes = enc.body
    state.bodyStream = new ReadableStream(enc.body)
    if (state.headers && typeof state.headers.has === 'function' && !state.headers.has('content-type')) {
      state.headers.set('content-type', enc.contentType)
    }
    return
  }
  const bytes = toBytes(body)
  state.bodyBytes = bytes
  state.bodyStream = new ReadableStream(bytes)
}

function consumeBody(state) {
  if (state.bodyUsed) {
    throw new TypeError('Body has already been used')
  }
  state.bodyUsed = true
  const bytes = state.bodyBytes || new Uint8Array(0)
  // Keep the bytes for clone() calls made before consumption.
  return bytes
}

export class Request {
  constructor(input, init) {
    const ii = init && typeof init === 'object' ? init : null

    let url
    let method = 'GET'
    let headers = new Headers()
    let body = undefined
    let signal = undefined

    if (input instanceof Request) {
      const s = requestState.get(input)
      url = s.url
      method = s.method
      headers = new Headers(s.headers)
      body = s.bodyBytes ? cloneBytes(s.bodyBytes) : undefined
      signal = s.signal
    } else {
      url = String(input)
    }

    if (ii) {
      if (ii.method != null) method = String(ii.method).toUpperCase()
      if (ii.headers != null) headers = new Headers(ii.headers)
      if (Object.prototype.hasOwnProperty.call(ii, 'body')) body = ii.body
      if (ii.signal != null) signal = ii.signal
    }

    if (!url) throw new TypeError('Request: url required')
    if (!method) method = 'GET'

    const state = {
      url,
      method,
      headers,
      signal,
      bodyUsed: false,
      bodyBytes: null,
      bodyStream: null,
    }

    if (body != null) {
      if (method === 'GET' || method === 'HEAD') {
        throw new TypeError('Request with GET/HEAD method cannot have body')
      }
      initBody(state, body)
      // Default Content-Type for common body types.
      if (typeof body === 'string' && !headers.has('content-type')) {
        headers.set('content-type', 'text/plain;charset=UTF-8')
      } else if (body instanceof Blob && body.type && !headers.has('content-type')) {
        headers.set('content-type', body.type)
      }
    }

    requestState.set(this, state)
  }

  get url() {
    return requestState.get(this).url
  }
  get method() {
    return requestState.get(this).method
  }
  get headers() {
    return requestState.get(this).headers
  }
  get signal() {
    return requestState.get(this).signal
  }
  get body() {
    return requestState.get(this).bodyStream
  }
  get bodyUsed() {
    return requestState.get(this).bodyUsed
  }

  clone() {
    const state = requestState.get(this)
    if (state.bodyUsed) {
      throw new TypeError('Request body is already used')
    }
    return new Request(this)
  }

  async arrayBuffer() {
    const state = requestState.get(this)
    const bytes = consumeBody(state)
    return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength)
  }
  async text() {
    const state = requestState.get(this)
    const bytes = consumeBody(state)
    const dec = new TextDecoder('utf-8')
    return dec.decode(bytes)
  }
  async json() {
    return JSON.parse(await this.text())
  }
  async blob() {
    const state = requestState.get(this)
    const bytes = consumeBody(state)
    const ct = state.headers.get('content-type') || ''
    return new Blob([bytes], { type: ct })
  }
}

export function toHostFetchRequest(req) {
  if (!(req instanceof Request)) {
    throw new TypeError('toHostFetchRequest(req): req must be a Request')
  }
  const state = requestState.get(req)
  if (!state) {
    throw new TypeError('toHostFetchRequest(req): invalid Request')
  }
  const headersPairs = []
  state.headers.forEach((v, k) => headersPairs.push([k, v]))
  return {
    url: state.url,
    method: state.method,
    headers: headersPairs,
    body: state.bodyBytes || null,
    signal: state.signal,
  }
}

export class Response {
  constructor(body, init) {
    const ii = init && typeof init === 'object' ? init : null
    const status = ii && typeof ii.status === 'number' ? ii.status : 200
    const statusText = ii && typeof ii.statusText === 'string' ? ii.statusText : ''
    const headers = ii && ii.headers != null ? new Headers(ii.headers) : new Headers()
    const url = ii && typeof ii.url === 'string' ? ii.url : ''

    const state = {
      status: Number(status),
      statusText: String(statusText),
      headers,
      url: String(url),
      bodyUsed: false,
      bodyBytes: null,
      bodyStream: null,
    }
    initBody(state, body)
    responseState.set(this, state)
  }

  get ok() {
    const s = responseState.get(this).status
    return s >= 200 && s <= 299
  }
  get status() {
    return responseState.get(this).status
  }
  get statusText() {
    return responseState.get(this).statusText
  }
  get headers() {
    return responseState.get(this).headers
  }
  get url() {
    return responseState.get(this).url
  }
  get body() {
    return responseState.get(this).bodyStream
  }
  get bodyUsed() {
    return responseState.get(this).bodyUsed
  }

  clone() {
    const s = responseState.get(this)
    if (s.bodyUsed) {
      throw new TypeError('Response body is already used')
    }
    const bytes = s.bodyBytes ? cloneBytes(s.bodyBytes) : null
    return new Response(bytes, {
      status: s.status,
      statusText: s.statusText,
      headers: new Headers(s.headers),
      url: s.url,
    })
  }

  async arrayBuffer() {
    const state = responseState.get(this)
    const bytes = consumeBody(state)
    return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength)
  }
  async text() {
    const state = responseState.get(this)
    const bytes = consumeBody(state)
    const dec = new TextDecoder('utf-8')
    return dec.decode(bytes)
  }
  async json() {
    return JSON.parse(await this.text())
  }
  async blob() {
    const state = responseState.get(this)
    const bytes = consumeBody(state)
    const ct = state.headers.get('content-type') || ''
    return new Blob([bytes], { type: ct })
  }

  static json(data, init) {
    const body = JSON.stringify(data)
    const ii = init && typeof init === 'object' ? init : {}
    const headers = new Headers(ii.headers || {})
    if (!headers.has('content-type')) headers.set('content-type', 'application/json')
    return new Response(body, { ...ii, headers })
  }
}

export class AbortSignal extends (typeof globalThis.EventTarget === 'function' ? globalThis.EventTarget : class {}) {
  constructor() {
    super()
    this.aborted = false
    this.reason = undefined
  }
  throwIfAborted() {
    if (!this.aborted) return
    if (this.reason !== undefined) throw this.reason
    throw new DOMException('The operation was aborted.', 'AbortError')
  }
}

export class AbortController {
  constructor() {
    this.signal = new AbortSignal()
  }
  abort(reason) {
    const sig = this.signal
    if (sig.aborted) return
    if (arguments.length < 1) {
      reason = new DOMException('The operation was aborted.', 'AbortError')
    }
    sig.aborted = true
    sig.reason = reason
    try {
      const ev = typeof globalThis.Event === 'function' ? new globalThis.Event('abort') : { type: 'abort' }
      if (typeof sig.dispatchEvent === 'function') sig.dispatchEvent(ev)
    } catch (_) {
      // ignore
    }
  }
}

export function installGlobals() {
  defineGlobal('TextEncoder', TextEncoder)
  defineGlobal('TextDecoder', TextDecoder)
  defineGlobal('ReadableStream', ReadableStream)
  defineGlobal('Blob', Blob)
  defineGlobal('FormData', FormData)
  defineGlobal('Headers', Headers)
  defineGlobal('Request', Request)
  defineGlobal('Response', Response)
  defineGlobal('AbortController', AbortController)
  defineGlobal('AbortSignal', AbortSignal)
}

export default Object.freeze({
  TextEncoder,
  TextDecoder,
  ReadableStream,
  Blob,
  FormData,
  Headers,
  Request,
  Response,
  AbortController,
  AbortSignal,
  toHostFetchRequest,
  installGlobals,
})
