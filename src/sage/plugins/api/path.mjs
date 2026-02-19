function toStr(v) {
  return typeof v === 'string' ? v : String(v)
}

export function isAbsolute(p) {
  return toStr(p).startsWith('/')
}

export function normalize(p) {
  p = toStr(p)
  const abs = p.startsWith('/')
  const parts = p.split('/')
  const out = []
  for (let i = 0; i < parts.length; i++) {
    const part = parts[i]
    if (!part || part === '.') continue
    if (part === '..') {
      if (out.length && out[out.length - 1] !== '..') {
        out.pop()
      } else if (!abs) {
        out.push('..')
      }
      continue
    }
    out.push(part)
  }
  let res = out.join('/')
  if (abs) res = '/' + res
  if (res === '') res = abs ? '/' : '.'
  return res
}

export function join(...parts) {
  let s = ''
  for (let i = 0; i < parts.length; i++) {
    const part = toStr(parts[i])
    if (!part) continue
    if (s && !s.endsWith('/')) s += '/'
    s += part
  }
  return normalize(s)
}

export function dirname(p) {
  p = normalize(toStr(p))
  if (p === '/' || p === '.') return p
  const i = p.lastIndexOf('/')
  if (i < 0) return '.'
  if (i === 0) return '/'
  return p.slice(0, i)
}

export function basename(p, ext) {
  p = toStr(p)
  const i = p.lastIndexOf('/')
  let b = i >= 0 ? p.slice(i + 1) : p
  const e = ext ? toStr(ext) : ''
  if (e && b.endsWith(e)) b = b.slice(0, b.length - e.length)
  return b
}

export function extname(p) {
  const b = basename(p)
  const i = b.lastIndexOf('.')
  if (i <= 0) return ''
  return b.slice(i)
}

export function resolve(...parts) {
  let s = ''
  for (let i = parts.length - 1; i >= 0; i--) {
    const part = toStr(parts[i])
    if (!part) continue
    if (part.startsWith('/')) {
      s = part + '/' + s
      break
    }
    s = part + '/' + s
  }
  return normalize(s)
}

export default Object.freeze({ isAbsolute, normalize, join, dirname, basename, extname, resolve })
