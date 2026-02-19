import { defineGlobal } from 'sage:core/global'

function isHex(code) {
  return (
    (code >= 48 && code <= 57) || // 0-9
    (code >= 65 && code <= 70) || // A-F
    (code >= 97 && code <= 102) // a-f
  )
}

function hexVal(code) {
  if (code >= 48 && code <= 57) return code - 48
  if (code >= 65 && code <= 70) return code - 65 + 10
  if (code >= 97 && code <= 102) return code - 97 + 10
  return -1
}

function utf8EncodeCodePoint(cp, out) {
  if (cp <= 0x7f) {
    out.push(cp)
    return
  }
  if (cp <= 0x7ff) {
    out.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f))
    return
  }
  if (cp <= 0xffff) {
    out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f))
    return
  }
  out.push(
    0xf0 | (cp >> 18),
    0x80 | ((cp >> 12) & 0x3f),
    0x80 | ((cp >> 6) & 0x3f),
    0x80 | (cp & 0x3f)
  )
}

function utf8EncodeString(str) {
  str = String(str)
  const out = []
  for (let i = 0; i < str.length; i++) {
    let c = str.charCodeAt(i)
    // Surrogates.
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
      const next = str.charCodeAt(i + 1)
      if (next >= 0xdc00 && next <= 0xdfff) {
        const cp = 0x10000 + ((c - 0xd800) << 10) + (next - 0xdc00)
        utf8EncodeCodePoint(cp, out)
        i++
        continue
      }
    }
    if (c >= 0xd800 && c <= 0xdfff) {
      // Replacement char.
      utf8EncodeCodePoint(0xfffd, out)
      continue
    }
    utf8EncodeCodePoint(c, out)
  }
  return out
}

function utf8DecodeBytes(bytes) {
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
      if (i + 1 >= b.length) break
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
      if (i + 2 >= b.length) break
      const b1 = b[i + 1]
      const b2 = b[i + 2]
      if ((b1 & 0xc0) !== 0x80 || (b2 & 0xc0) !== 0x80) {
        out += '\uFFFD'
        i++
        continue
      }
      const cp = ((b0 & 0x0f) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f)
      out += String.fromCharCode(cp >= 0xd800 && cp <= 0xdfff ? 0xfffd : cp)
      i += 3
      continue
    }
    if (b0 >= 0xf0 && b0 <= 0xf4) {
      if (i + 3 >= b.length) break
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
      if (cp < 0x10000 || cp > 0x10ffff) {
        out += '\uFFFD'
        i += 4
        continue
      }
      const u = cp - 0x10000
      out += String.fromCharCode(0xd800 | (u >> 10), 0xdc00 | (u & 0x3ff))
      i += 4
      continue
    }
    out += '\uFFFD'
    i++
  }
  return out
}

function percentDecodeString(input, plusAsSpace) {
  const s = String(input)
  const bytes = []
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i)
    if (plusAsSpace && c === 43) {
      bytes.push(0x20)
      continue
    }
    if (c === 37 /* % */ && i + 2 < s.length) {
      const c1 = s.charCodeAt(i + 1)
      const c2 = s.charCodeAt(i + 2)
      if (isHex(c1) && isHex(c2)) {
        bytes.push((hexVal(c1) << 4) | hexVal(c2))
        i += 2
        continue
      }
    }
    // Encode this code point as UTF-8 bytes.
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
      const next = s.charCodeAt(i + 1)
      if (next >= 0xdc00 && next <= 0xdfff) {
        const cp = 0x10000 + ((c - 0xd800) << 10) + (next - 0xdc00)
        utf8EncodeCodePoint(cp, bytes)
        i++
        continue
      }
    }
    if (c >= 0xd800 && c <= 0xdfff) {
      utf8EncodeCodePoint(0xfffd, bytes)
      continue
    }
    utf8EncodeCodePoint(c, bytes)
  }
  return utf8DecodeBytes(new Uint8Array(bytes))
}

function percentEncodeString(input, shouldEncode) {
  const s = String(input)
  let out = ''
  for (let i = 0; i < s.length; i++) {
    const code = s.charCodeAt(i)
    // Preserve already-encoded sequences.
    if (code === 37 /* % */ && i + 2 < s.length) {
      const c1 = s.charCodeAt(i + 1)
      const c2 = s.charCodeAt(i + 2)
      if (isHex(c1) && isHex(c2)) {
        out += s.slice(i, i + 3)
        i += 2
        continue
      }
    }

    // Decode code point (handle surrogate pair).
    let cp = code
    if (code >= 0xd800 && code <= 0xdbff && i + 1 < s.length) {
      const next = s.charCodeAt(i + 1)
      if (next >= 0xdc00 && next <= 0xdfff) {
        cp = 0x10000 + ((code - 0xd800) << 10) + (next - 0xdc00)
        i++
      }
    } else if (code >= 0xd800 && code <= 0xdfff) {
      cp = 0xfffd
    }

    if (!shouldEncode(cp)) {
      out += String.fromCodePoint(cp)
      continue
    }

    const bytes = utf8EncodeString(String.fromCodePoint(cp))
    for (let bi = 0; bi < bytes.length; bi++) {
      const b = bytes[bi]
      out += '%' + b.toString(16).toUpperCase().padStart(2, '0')
    }
  }
  return out
}

function isC0Control(cp) {
  return cp <= 0x1f || cp === 0x7f
}

function inFragmentPercentEncodeSet(cp) {
  return (
    isC0Control(cp) ||
    cp === 0x20 /* space */ ||
    cp === 0x22 /* " */ ||
    cp === 0x3c /* < */ ||
    cp === 0x3e /* > */ ||
    cp === 0x60 /* ` */
  )
}

function inQueryPercentEncodeSet(cp) {
  return (
    isC0Control(cp) ||
    cp === 0x20 /* space */ ||
    cp === 0x22 /* " */ ||
    cp === 0x23 /* # */ ||
    cp === 0x3c /* < */ ||
    cp === 0x3e /* > */
  )
}

function inSpecialQueryPercentEncodeSet(cp) {
  return inQueryPercentEncodeSet(cp) || cp === 0x27 /* ' */
}

function inPathPercentEncodeSet(cp) {
  return inQueryPercentEncodeSet(cp) || cp === 0x3f /* ? */ || cp === 0x60 /* ` */ || cp === 0x7b /* { */ || cp === 0x7d /* } */ || cp === 0x5e /* ^ */
}

function inUserinfoPercentEncodeSet(cp) {
  return (
    inPathPercentEncodeSet(cp) ||
    cp === 0x2f /* / */ ||
    cp === 0x3a /* : */ ||
    cp === 0x3b /* ; */ ||
    cp === 0x3d /* = */ ||
    cp === 0x40 /* @ */ ||
    cp === 0x5b /* [ */ ||
    cp === 0x5c /* \\ */ ||
    cp === 0x5d /* ] */ ||
    cp === 0x7c /* | */
  )
}

function specialSchemeDefaultPort(scheme) {
  switch (scheme) {
    case 'http':
    case 'ws':
      return '80'
    case 'https':
    case 'wss':
      return '443'
    case 'ftp':
      return '21'
    default:
      return ''
  }
}

function isSpecialScheme(scheme) {
  return scheme === 'http' || scheme === 'https' || scheme === 'ws' || scheme === 'wss' || scheme === 'ftp' || scheme === 'file'
}

// -----------------------------------------------------------------------------
// Punycode (RFC 3492) for domain label ASCII encoding (minimal; no full TR46).
// Adapted from public-domain references; kept small for day-1 usability.

function punycodeEncode(input) {
  // Input is a single label (no dots). Returns ASCII label without xn-- prefix.
  const codePoints = []
  for (const ch of String(input)) codePoints.push(ch.codePointAt(0))

  const base = 36
  const tMin = 1
  const tMax = 26
  const skew = 38
  const damp = 700
  const initialBias = 72
  const initialN = 128
  const delimiter = 0x2d // '-'

  function encodeDigit(d) {
    return d < 26 ? String.fromCharCode(97 + d) : String.fromCharCode(48 + (d - 26))
  }

  function adapt(delta, numPoints, firstTime) {
    delta = firstTime ? Math.floor(delta / damp) : delta >> 1
    delta += Math.floor(delta / numPoints)
    let k = 0
    while (delta > (((base - tMin) * tMax) >> 1)) {
      delta = Math.floor(delta / (base - tMin))
      k += base
    }
    return k + Math.floor(((base - tMin + 1) * delta) / (delta + skew))
  }

  let n = initialN
  let delta = 0
  let bias = initialBias

  let output = ''
  let basicCount = 0
  for (let i = 0; i < codePoints.length; i++) {
    const cp = codePoints[i]
    if (cp < 0x80) {
      output += String.fromCharCode(cp)
      basicCount++
    }
  }

  let handled = basicCount
  if (basicCount > 0 && basicCount < codePoints.length) {
    output += String.fromCharCode(delimiter)
  }

  while (handled < codePoints.length) {
    let m = 0x7fffffff
    for (let i = 0; i < codePoints.length; i++) {
      const cp = codePoints[i]
      if (cp >= n && cp < m) m = cp
    }

    delta += (m - n) * (handled + 1)
    n = m

    for (let i = 0; i < codePoints.length; i++) {
      const cp = codePoints[i]
      if (cp < n) {
        delta++
        continue
      }
      if (cp !== n) continue

      let q = delta
      for (let k = base; ; k += base) {
        const t = k <= bias ? tMin : k >= bias + tMax ? tMax : k - bias
        if (q < t) break
        output += encodeDigit(t + ((q - t) % (base - t)))
        q = Math.floor((q - t) / (base - t))
      }
      output += encodeDigit(q)
      bias = adapt(delta, handled + 1, handled === basicCount)
      delta = 0
      handled++
    }

    delta++
    n++
  }

  return output
}

function domainToASCII(hostname) {
  const h = String(hostname)
  if (h === '') return ''
  if (h.startsWith('[') && h.endsWith(']')) return h // IPv6 literal

  const labels = h.split('.')
  const out = []
  for (let i = 0; i < labels.length; i++) {
    const label = labels[i]
    if (label === '') {
      out.push('')
      continue
    }
    const lower = label.toLowerCase()
    const isAscii = /^[\x00-\x7F]*$/.test(lower)
    if (isAscii) {
      out.push(lower)
      continue
    }
    const puny = punycodeEncode(lower)
    out.push('xn--' + puny)
  }
  return out.join('.')
}

// -----------------------------------------------------------------------------
// IPv4 parsing (WHATWG special schemes)

function parseIPv4Number(input) {
  const s = String(input)
  if (s === '') return null

  let base = 10
  let start = 0
  if ((s.startsWith('0x') || s.startsWith('0X')) && s.length > 2) {
    base = 16
    start = 2
  } else if (s.length > 1 && s[0] === '0') {
    base = 8
  }

  let value = 0n
  const b = BigInt(base)
  for (let i = start; i < s.length; i++) {
    const c = s.charCodeAt(i)
    let d = -1
    if (c >= 48 && c <= 57) d = c - 48
    else if (c >= 65 && c <= 70) d = c - 65 + 10
    else if (c >= 97 && c <= 102) d = c - 97 + 10
    if (d < 0 || d >= base) return null
    value = value * b + BigInt(d)
  }
  return value
}

function endsInNumber(hostname) {
  const h = String(hostname)
  if (h === '') return false
  const parts = h.split('.')
  const last = parts[parts.length - 1]
  if (last === '') return false
  // Per URL Standard, "ends in a number" checks whether the last label is a
  // valid base-10 integer, or a valid 0x-prefixed hex integer. (Not octal.)
  if (/^[0-9]+$/.test(last)) return true
  if (/^0[xX][0-9A-Fa-f]+$/.test(last)) return true
  return false
}

function parseIPv4(hostname) {
  const h = String(hostname)
  if (h === '') return null

  const parts = h.split('.')
  if (parts.length > 4) return null
  if (parts[parts.length - 1] === '') return null

  const nums = []
  for (let i = 0; i < parts.length; i++) {
    const n = parseIPv4Number(parts[i])
    if (n === null) return null
    nums.push(n)
  }

  for (let i = 0; i < nums.length - 1; i++) {
    if (nums[i] > 255n) return null
  }

  let ipv4 = 0n
  for (let i = 0; i < nums.length - 1; i++) {
    ipv4 = ipv4 * 256n + nums[i]
  }

  let factor = 1n
  for (let i = 0; i < 5 - nums.length; i++) factor *= 256n
  const last = nums[nums.length - 1]
  if (last >= factor) return null
  ipv4 = ipv4 * factor + last
  if (ipv4 > 0xffffffffn) return null

  const a = Number((ipv4 >> 24n) & 255n)
  const b = Number((ipv4 >> 16n) & 255n)
  const c = Number((ipv4 >> 8n) & 255n)
  const d = Number(ipv4 & 255n)
  return String(a) + '.' + String(b) + '.' + String(c) + '.' + String(d)
}

function canonicalizeSpecialHost(hostname) {
  const host = String(hostname)
  if (host.startsWith('[') && host.endsWith(']')) return host // IPv6 literal

  let check = host
  // Trim a single trailing '.' for IPv4 detection.
  if (check.endsWith('.')) check = check.slice(0, -1)

  if (!endsInNumber(check)) return host
  return parseIPv4(check)
}

// -----------------------------------------------------------------------------
// URLSearchParams

const bindToURLSymbol = Symbol('sage.urlsearchparams.bindToURL')
const fromStringSymbol = Symbol('sage.urlsearchparams.fromString')

function urlSearchParamsEncode(str) {
  const s = String(str)
  let out = ''
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i)
    // Space => +
    if (c === 0x20) {
      out += '+'
      continue
    }
    // Allowed: A-Z a-z 0-9 * - . _
    const isAlphaNum =
      (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122)
    if (isAlphaNum || c === 0x2a || c === 0x2d || c === 0x2e || c === 0x5f) {
      out += String.fromCharCode(c)
      continue
    }
    // Encode this code point to UTF-8, then percent encode.
    let cp = c
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
      const next = s.charCodeAt(i + 1)
      if (next >= 0xdc00 && next <= 0xdfff) {
        cp = 0x10000 + ((c - 0xd800) << 10) + (next - 0xdc00)
        i++
      }
    } else if (c >= 0xd800 && c <= 0xdfff) {
      cp = 0xfffd
    }
    const bytes = []
    utf8EncodeCodePoint(cp, bytes)
    for (let bi = 0; bi < bytes.length; bi++) {
      const b = bytes[bi]
      out += '%' + b.toString(16).toUpperCase().padStart(2, '0')
    }
  }
  return out
}

function urlSearchParamsDecode(str) {
  return percentDecodeString(str, true)
}

export class URLSearchParams {
  #list
  #update

  constructor(init) {
    this.#list = []
    this.#update = null

    if (init == null) return
    if (init instanceof URLSearchParams) {
      for (const [k, v] of init) this.#list.push([k, v])
      return
    }
    if (typeof init === 'string') {
      this[fromStringSymbol](init)
      return
    }
    if (Array.isArray(init)) {
      for (let i = 0; i < init.length; i++) {
        const pair = init[i]
        if (!pair) continue
        const k = pair[0]
        const v = pair[1]
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
    throw new TypeError('URLSearchParams: unsupported init')
  }

  get size() {
    return this.#list.length
  }

  [bindToURLSymbol](updateFn) {
    this.#update = typeof updateFn === 'function' ? updateFn : null
  }

  #signalUpdate() {
    if (this.#update) {
      try {
        this.#update()
      } catch (_) {
        // ignore
      }
    }
  }

  [fromStringSymbol](str) {
    this.#list.length = 0
    let s = String(str)
    if (s.startsWith('?')) s = s.slice(1)
    if (s === '') return
    const parts = s.split('&')
    for (let i = 0; i < parts.length; i++) {
      const part = parts[i]
      if (part === '') continue
      const eq = part.indexOf('=')
      const name = eq >= 0 ? part.slice(0, eq) : part
      const value = eq >= 0 ? part.slice(eq + 1) : ''
      this.#list.push([urlSearchParamsDecode(name), urlSearchParamsDecode(value)])
    }
  }

  append(name, value) {
    this.#list.push([String(name), String(value)])
    this.#signalUpdate()
  }

  delete(name, value) {
    const n = String(name)
    const hasValue = arguments.length > 1
    const v = hasValue ? String(value) : ''
    for (let i = this.#list.length - 1; i >= 0; i--) {
      if (this.#list[i][0] !== n) continue
      if (!hasValue || this.#list[i][1] === v) this.#list.splice(i, 1)
    }
    this.#signalUpdate()
  }

  get(name) {
    const n = String(name)
    for (let i = 0; i < this.#list.length; i++) {
      const it = this.#list[i]
      if (it[0] === n) return it[1]
    }
    return null
  }

  getAll(name) {
    const n = String(name)
    const out = []
    for (let i = 0; i < this.#list.length; i++) {
      const it = this.#list[i]
      if (it[0] === n) out.push(it[1])
    }
    return out
  }

  has(name, value) {
    const n = String(name)
    const hasValue = arguments.length > 1
    const v = hasValue ? String(value) : ''
    for (let i = 0; i < this.#list.length; i++) {
      const it = this.#list[i]
      if (it[0] !== n) continue
      if (!hasValue || it[1] === v) return true
    }
    return false
  }

  set(name, value) {
    const n = String(name)
    const v = String(value)
    let found = false
    for (let i = this.#list.length - 1; i >= 0; i--) {
      if (this.#list[i][0] !== n) continue
      if (!found) {
        this.#list[i][1] = v
        found = true
      } else {
        this.#list.splice(i, 1)
      }
    }
    if (!found) this.#list.push([n, v])
    this.#signalUpdate()
  }

  sort() {
    this.#list.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0))
    this.#signalUpdate()
  }

  forEach(fn, thisArg) {
    if (typeof fn !== 'function') throw new TypeError('URLSearchParams.forEach: fn must be a function')
    for (const [k, v] of this.#list) fn.call(thisArg, v, k, this)
  }

  *entries() {
    for (let i = 0; i < this.#list.length; i++) {
      const it = this.#list[i]
      yield [it[0], it[1]]
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

  toString() {
    const out = []
    for (let i = 0; i < this.#list.length; i++) {
      const [k, v] = this.#list[i]
      out.push(urlSearchParamsEncode(k) + '=' + urlSearchParamsEncode(v))
    }
    return out.join('&')
  }
}

// -----------------------------------------------------------------------------
// URL

const urlState = new WeakMap()

function cloneState(s) {
  return {
    scheme: s.scheme,
    username: s.username,
    password: s.password,
    host: s.host,
    port: s.port,
    pathname: s.pathname,
    query: s.query,
    fragment: s.fragment,
    hasAuthority: s.hasAuthority,
    searchParams: null,
    updatingSearchParams: false,
  }
}

function isSingleDotPathSegment(seg) {
  const s = String(seg).toLowerCase()
  return s === '.' || s === '%2e'
}

function isDoubleDotPathSegment(seg) {
  const s = String(seg).toLowerCase()
  return s === '..' || s === '.%2e' || s === '%2e.' || s === '%2e%2e'
}

function normalizePathname(pathname) {
  let p = String(pathname || '')
  if (p === '') return '/'
  if (!p.startsWith('/')) p = '/' + p

  // Operate on the path segment list (excluding the leading '/'), preserving
  // empty segments (consecutive slashes + trailing slashes) while removing dot
  // segments per WHATWG rules (including percent-encoded dot segments).
  const parts = p.slice(1).split('/')
  const out = []
  for (let i = 0; i < parts.length; i++) {
    const seg = parts[i]
    const isLast = i === parts.length - 1

    if (isSingleDotPathSegment(seg)) {
      if (isLast) out.push('')
      continue
    }
    if (isDoubleDotPathSegment(seg)) {
      if (out.length > 0) out.pop()
      if (isLast) out.push('')
      continue
    }
    out.push(seg)
  }
  return '/' + out.join('/')
}

function parseAuthority(auth) {
  let s = String(auth)
  let username = ''
  let password = ''
  let hostport = s

  const at = s.lastIndexOf('@')
  if (at >= 0) {
    const userinfo = s.slice(0, at)
    hostport = s.slice(at + 1)
    const colon = userinfo.indexOf(':')
    if (colon >= 0) {
      username = userinfo.slice(0, colon)
      password = userinfo.slice(colon + 1)
    } else {
      username = userinfo
    }
    username = percentEncodeString(percentDecodeString(username, false), inUserinfoPercentEncodeSet)
    password = percentEncodeString(percentDecodeString(password, false), inUserinfoPercentEncodeSet)
  }

  let host = ''
  let port = ''
  if (hostport.startsWith('[')) {
    const end = hostport.indexOf(']')
    if (end < 0) throw new TypeError('Invalid IPv6 address')
    host = hostport.slice(0, end + 1)
    const rest = hostport.slice(end + 1)
    if (rest.startsWith(':')) port = rest.slice(1)
  } else {
    const colon = hostport.lastIndexOf(':')
    if (colon >= 0 && hostport.indexOf(':') === colon) {
      host = hostport.slice(0, colon)
      port = hostport.slice(colon + 1)
    } else {
      host = hostport
    }
  }

  host = domainToASCII(host.trim())
  if (/\s/.test(host)) throw new TypeError('Invalid hostname')
  if (port) {
    if (!/^[0-9]*$/.test(port)) throw new TypeError('Invalid port')
    if (port.length > 0) {
      const n = Number(port)
      if (!Number.isFinite(n) || n < 0 || n > 65535) throw new TypeError('Invalid port')
      port = String(n)
    }
  }

  return { username, password, host, port }
}

function parseURL(input, base) {
  let s = String(input)
  // Trim leading/trailing whitespace like browsers do.
  s = s.replace(/^[\u0009\u000A\u000C\u000D\u0020]+|[\u0009\u000A\u000C\u000D\u0020]+$/g, '')

  const baseState = base ? (urlState.get(base) ? cloneState(urlState.get(base)) : parseURL(base, null)) : null

  const schemeMatch = /^[A-Za-z][A-Za-z0-9+.-]*:/.exec(s)
  if (schemeMatch) {
    const scheme = schemeMatch[0].slice(0, -1).toLowerCase()
    let rest = s.slice(schemeMatch[0].length)

    const state = {
      scheme,
      username: '',
      password: '',
      host: '',
      port: '',
      pathname: '',
      query: '',
      fragment: '',
      hasAuthority: false,
      searchParams: null,
      updatingSearchParams: false,
    }

    const isSpecial = isSpecialScheme(scheme)
    if (isSpecial) {
      // Special schemes treat backslashes as slashes and always have an authority
      // separator in serialization (even when host is empty, e.g. `file:///`).
      rest = rest.replace(/\\/g, '/')
      state.hasAuthority = true

      if (scheme === 'file') {
        if (rest.startsWith('//')) {
          rest = rest.slice(2)
          const endAuth = rest.search(/[/?#]/)
          const auth = endAuth >= 0 ? rest.slice(0, endAuth) : rest
          rest = endAuth >= 0 ? rest.slice(endAuth) : ''
          const a = parseAuthority(auth)
          if (a.username || a.password || a.port) throw new TypeError('Invalid URL')
          state.username = a.username
          state.password = a.password
          state.host = a.host
          state.port = a.port
        }

        // `file://localhost/...` is treated as empty host.
        if (state.host === 'localhost') {
          state.username = ''
          state.password = ''
          state.host = ''
          state.port = ''
        }
        if (state.host) {
          const canon = canonicalizeSpecialHost(state.host)
          if (canon == null) throw new TypeError('Invalid URL')
          state.host = canon
        }
      } else {
        // For other special schemes, ignore all leading slashes (and treat the
        // remainder as the authority), matching browser/Node URL parsing.
        while (rest.startsWith('/')) rest = rest.slice(1)
        const endAuth = rest.search(/[/?#]/)
        const auth = endAuth >= 0 ? rest.slice(0, endAuth) : rest
        rest = endAuth >= 0 ? rest.slice(endAuth) : ''
        if (auth === '') throw new TypeError('Invalid URL')
        const a = parseAuthority(auth)
        state.username = a.username
        state.password = a.password
        state.host = a.host
        state.port = a.port
        if (!state.host) throw new TypeError('Invalid URL')
        const canon = canonicalizeSpecialHost(state.host)
        if (canon == null) throw new TypeError('Invalid URL')
        state.host = canon
      }

      // Drop default ports for special schemes.
      if (state.port && state.port === specialSchemeDefaultPort(scheme)) state.port = ''
    } else if (rest.startsWith('//')) {
      state.hasAuthority = true
      rest = rest.slice(2)
      const endAuth = rest.search(/[/?#]/)
      const auth = endAuth >= 0 ? rest.slice(0, endAuth) : rest
      rest = endAuth >= 0 ? rest.slice(endAuth) : ''
      const a = parseAuthority(auth)
      state.username = a.username
      state.password = a.password
      state.host = a.host
      state.port = a.port
    }

    // Fragment.
    const hash = rest.indexOf('#')
    if (hash >= 0) {
      state.fragment = percentEncodeString(rest.slice(hash + 1), inFragmentPercentEncodeSet)
      rest = rest.slice(0, hash)
    }
    // Query.
    const q = rest.indexOf('?')
    if (q >= 0) {
      state.query = percentEncodeString(
        rest.slice(q + 1),
        isSpecialScheme(scheme) ? inSpecialQueryPercentEncodeSet : inQueryPercentEncodeSet
      )
      rest = rest.slice(0, q)
    }

    const encodedPath = percentEncodeString(rest || '', inPathPercentEncodeSet)
    if (isSpecial) {
      state.pathname = normalizePathname(encodedPath)
    } else if (state.hasAuthority) {
      // For non-special schemes with an authority, the path is hierarchical but
      // may be empty (no implicit trailing slash).
      state.pathname = encodedPath === '' ? '' : normalizePathname(encodedPath)
    } else if (encodedPath.startsWith('/')) {
      // Path-absolute non-special URLs normalize dot segments.
      state.pathname = normalizePathname(encodedPath)
    } else {
      // Opaque path.
      state.pathname = encodedPath
    }

    return state
  }

  // Relative URL.
  if (!baseState) {
    throw new TypeError('Invalid URL (no base)')
  }

  const state = cloneState(baseState)
  state.fragment = ''

  if (s === '') {
    // Empty relative URL: keep base as-is (except fragment cleared).
    return state
  }

  // Special schemes treat backslashes as slashes in relative inputs.
  if (isSpecialScheme(state.scheme)) {
    s = s.replace(/\\/g, '/')
  }

  if (s.startsWith('//')) {
    // Scheme-relative.
    state.hasAuthority = true
    s = s.slice(2)
    const endAuth = s.search(/[/?#]/)
    const auth = endAuth >= 0 ? s.slice(0, endAuth) : s
    s = endAuth >= 0 ? s.slice(endAuth) : ''
    const a = parseAuthority(auth)
    state.username = a.username
    state.password = a.password
    state.host = a.host
    state.port = a.port

    if (state.scheme === 'file') {
      if (state.username || state.password || state.port) throw new TypeError('Invalid URL')
      if (state.host === 'localhost') {
        state.username = ''
        state.password = ''
        state.host = ''
        state.port = ''
      }
      if (state.host) {
        const canon = canonicalizeSpecialHost(state.host)
        if (canon == null) throw new TypeError('Invalid URL')
        state.host = canon
      }
    }

    if (isSpecialScheme(state.scheme) && state.scheme !== 'file') {
      if (!state.host) throw new TypeError('Invalid URL')
      const canon = canonicalizeSpecialHost(state.host)
      if (canon == null) throw new TypeError('Invalid URL')
      state.host = canon
      if (state.port && state.port === specialSchemeDefaultPort(state.scheme)) state.port = ''
    }

    state.pathname = isSpecialScheme(state.scheme) ? '/' : ''
    state.query = ''

    if (s === '') return state
  }

  // Fragment-only.
  if (s.startsWith('#')) {
    state.fragment = percentEncodeString(s.slice(1), inFragmentPercentEncodeSet)
    return state
  }

  // Query-only.
  if (s.startsWith('?')) {
    state.query = percentEncodeString(
      s.slice(1),
      isSpecialScheme(state.scheme) ? inSpecialQueryPercentEncodeSet : inQueryPercentEncodeSet
    )
    return state
  }

  // Split off fragment/query for path handling.
  const hash = s.indexOf('#')
  if (hash >= 0) {
    state.fragment = percentEncodeString(s.slice(hash + 1), inFragmentPercentEncodeSet)
    s = s.slice(0, hash)
  }
  const q = s.indexOf('?')
  if (q >= 0) {
    state.query = percentEncodeString(
      s.slice(q + 1),
      isSpecialScheme(state.scheme) ? inSpecialQueryPercentEncodeSet : inQueryPercentEncodeSet
    )
    s = s.slice(0, q)
  } else {
    state.query = ''
  }

  if (s.startsWith('/')) {
    state.pathname = normalizePathname(percentEncodeString(s, inPathPercentEncodeSet))
    return state
  }

  // Merge with base path.
  const basePath = String(state.pathname || '/')
  const slash = basePath.lastIndexOf('/')
  const merged = (slash >= 0 ? basePath.slice(0, slash + 1) : '/') + s
  state.pathname = normalizePathname(percentEncodeString(merged, inPathPercentEncodeSet))
  return state
}

function serializeURL(state) {
  const scheme = state.scheme
  let out = scheme + ':'

  if (state.hasAuthority) {
    out += '//'
    if (state.username || state.password) {
      out += state.username
      if (state.password) out += ':' + state.password
      out += '@'
    }
    out += state.host
    if (state.port) out += ':' + state.port
  }

  if (isSpecialScheme(scheme)) {
    out += state.pathname || '/'
  } else {
    out += state.pathname || ''
  }

  if (state.query) out += '?' + state.query
  if (state.fragment) out += '#' + state.fragment
  return out
}

function originFor(state) {
  if (!state.hasAuthority) return 'null'
  const scheme = state.scheme
  if (!isSpecialScheme(scheme) || scheme === 'file') return 'null'
  const port = state.port && state.port !== specialSchemeDefaultPort(scheme) ? ':' + state.port : ''
  return scheme + '://' + state.host + port
}

function isOpaqueURLState(state) {
  if (isSpecialScheme(state.scheme)) return false
  if (state.hasAuthority) return false
  const p = String(state.pathname || '')
  return p === '' || !p.startsWith('/')
}

export class URL {
  constructor(url, base) {
    const b = base == null ? null : base instanceof URL ? base : new URL(String(base))
    const state = parseURL(url, b)
    urlState.set(this, state)
  }

  static parse(url, base) {
    try {
      return new URL(url, base)
    } catch (_) {
      return null
    }
  }

  static canParse(url, base) {
    try {
      void new URL(url, base)
      return true
    } catch (_) {
      return false
    }
  }

  get href() {
    return serializeURL(urlState.get(this))
  }
  set href(v) {
    const state = parseURL(v, null)
    const cur = urlState.get(this)
    state.searchParams = cur.searchParams
    if (state.searchParams) {
      const usp = state.searchParams
      state.updatingSearchParams = true
      usp[fromStringSymbol](state.query ? '?' + state.query : '')
      state.updatingSearchParams = false

      // Re-bind to the new URL state so updates remain live.
      usp[bindToURLSymbol](() => {
        if (state.updatingSearchParams) return
        state.query = usp.toString()
      })
    }
    urlState.set(this, state)
  }

  get origin() {
    return originFor(urlState.get(this))
  }

  get protocol() {
    return urlState.get(this).scheme + ':'
  }
  set protocol(v) {
    const state = urlState.get(this)
    if (isOpaqueURLState(state)) return
    const s = String(v)
    const i = s.indexOf(':')
    const scheme = (i >= 0 ? s.slice(0, i) : s).toLowerCase()
    if (!/^[a-z][a-z0-9+.-]*$/.test(scheme)) return
    const curSpecial = isSpecialScheme(state.scheme)
    const nextSpecial = isSpecialScheme(scheme)
    if (curSpecial !== nextSpecial) return

    if (nextSpecial) {
      // Special schemes require an authority; most require a non-empty host.
      if (!state.hasAuthority) return
      if (scheme !== 'file' && !state.host) return
      if (scheme === 'file') {
        // `file:` rejects userinfo + port.
        if (!state.host) return
        if (state.username || state.password || state.port) return
        state.username = ''
        state.password = ''
        state.port = ''
        if (state.host === 'localhost') state.host = ''
      }
      if (state.port && state.port === specialSchemeDefaultPort(scheme)) state.port = ''
      if (!state.pathname) state.pathname = '/'
    }

    state.scheme = scheme
  }

  get username() {
    return percentDecodeString(urlState.get(this).username, false)
  }
  set username(v) {
    const state = urlState.get(this)
    if (isOpaqueURLState(state)) return
    if (!state.hasAuthority) return
    if (!state.host) return
    if (state.scheme === 'file') return
    state.username = percentEncodeString(String(v), inUserinfoPercentEncodeSet)
  }

  get password() {
    return percentDecodeString(urlState.get(this).password, false)
  }
  set password(v) {
    const state = urlState.get(this)
    if (isOpaqueURLState(state)) return
    if (!state.hasAuthority) return
    if (!state.host) return
    if (state.scheme === 'file') return
    state.password = percentEncodeString(String(v), inUserinfoPercentEncodeSet)
  }

  get host() {
    const s = urlState.get(this)
    return s.port ? s.host + ':' + s.port : s.host
  }
  set host(v) {
    const s = urlState.get(this)
    if (isOpaqueURLState(s)) return
    let a
    try {
      a = parseAuthority(String(v))
    } catch (_) {
      return
    }
    // Disallow userinfo in host assignments (matches browsers/Node).
    if (a.username || a.password) return

    if (s.scheme === 'file') {
      // `file:` rejects userinfo + port in the authority.
      if (a.port) return
      s.username = ''
      s.password = ''
      s.port = ''
      let host = a.host === 'localhost' ? '' : a.host
      if (host) {
        const canon = canonicalizeSpecialHost(host)
        if (canon == null) return
        host = canon
      }
      s.host = host
      s.hasAuthority = true
      if (!s.pathname) s.pathname = '/'
      return
    }

    if (isSpecialScheme(s.scheme)) {
      if (!a.host) return
      const canon = canonicalizeSpecialHost(a.host)
      if (canon == null) return
      s.host = canon
      s.port = a.port
      const def = specialSchemeDefaultPort(s.scheme)
      if (s.port && def && s.port === def) s.port = ''
      s.hasAuthority = true
      if (!s.pathname) s.pathname = '/'
      return
    }

    // Non-special: allow empty host.
    s.host = a.host
    s.port = a.port
    s.hasAuthority = true
  }

  get hostname() {
    return urlState.get(this).host
  }
  set hostname(v) {
    const s = urlState.get(this)
    if (isOpaqueURLState(s)) return
    let a
    try {
      a = parseAuthority(String(v))
    } catch (_) {
      return
    }
    // hostname must not include userinfo or port.
    if (a.username || a.password || a.port) return

    if (s.scheme === 'file') {
      s.username = ''
      s.password = ''
      s.port = ''
      let host = a.host === 'localhost' ? '' : a.host
      if (host) {
        const canon = canonicalizeSpecialHost(host)
        if (canon == null) return
        host = canon
      }
      s.host = host
      s.hasAuthority = true
      if (!s.pathname) s.pathname = '/'
      return
    }

    if (isSpecialScheme(s.scheme)) {
      if (!a.host) return
      const canon = canonicalizeSpecialHost(a.host)
      if (canon == null) return
      s.host = canon
      s.hasAuthority = true
      if (!s.pathname) s.pathname = '/'
      return
    }

    s.host = a.host
    s.hasAuthority = true
  }

  get port() {
    return urlState.get(this).port
  }
  set port(v) {
    const s = urlState.get(this)
    const p = String(v)
    if (isOpaqueURLState(s)) return
    if (!s.hasAuthority) return
    if (!s.host) return
    if (s.scheme === 'file') return
    if (p === '') {
      s.port = ''
      return
    }
    if (!/^[0-9]+$/.test(p)) return
    const n = Number(p)
    if (!Number.isFinite(n) || n < 0 || n > 65535) return
    const normalized = String(n)
    const def = specialSchemeDefaultPort(s.scheme)
    s.port = def && normalized === def ? '' : normalized
    s.hasAuthority = true
  }

  get pathname() {
    const s = urlState.get(this)
    return s.pathname || (isSpecialScheme(s.scheme) ? '/' : '')
  }
  set pathname(v) {
    const s = urlState.get(this)
    if (isOpaqueURLState(s)) return
    const p = String(v)
    const encoded = percentEncodeString(p, inPathPercentEncodeSet)

    if (encoded === '') {
      if (s.hasAuthority && !isSpecialScheme(s.scheme)) {
        s.pathname = ''
      } else {
        s.pathname = '/'
      }
      return
    }

    s.pathname = normalizePathname(encoded)
  }

  get search() {
    const q = urlState.get(this).query
    return q ? '?' + q : ''
  }
  set search(v) {
    const s = urlState.get(this)
    const raw = String(v)
    const q = raw.startsWith('?') ? raw.slice(1) : raw
    s.query = q
      ? percentEncodeString(
          q,
          isSpecialScheme(s.scheme) ? inSpecialQueryPercentEncodeSet : inQueryPercentEncodeSet
        )
      : ''
    if (s.searchParams && !s.updatingSearchParams) {
      s.updatingSearchParams = true
      s.searchParams[fromStringSymbol](s.query ? '?' + s.query : '')
      s.updatingSearchParams = false
    }
  }

  get searchParams() {
    const s = urlState.get(this)
    if (s.searchParams) return s.searchParams
    const usp = new URLSearchParams(s.query ? '?' + s.query : '')
    usp[bindToURLSymbol](() => {
      if (s.updatingSearchParams) return
      s.query = usp.toString()
    })
    s.searchParams = usp
    return usp
  }

  get hash() {
    const f = urlState.get(this).fragment
    return f ? '#' + f : ''
  }
  set hash(v) {
    const s = urlState.get(this)
    const raw = String(v)
    const f = raw.startsWith('#') ? raw.slice(1) : raw
    s.fragment = f ? percentEncodeString(f, inFragmentPercentEncodeSet) : ''
  }

  toString() {
    return this.href
  }

  toJSON() {
    return this.href
  }
}

export function installGlobals() {
  defineGlobal('URL', URL)
  defineGlobal('URLSearchParams', URLSearchParams)
}

export default Object.freeze({ URL, URLSearchParams, installGlobals })
