// Internal helpers for `sage:*` builtin modules.

export function requireHostFunction(name) {
  const fn = globalThis[name]
  if (typeof fn !== 'function') {
    throw new Error('sage host function missing: ' + String(name))
  }
  return fn
}

export function optionalHostFunction(name) {
  const fn = globalThis[name]
  return typeof fn === 'function' ? fn : null
}
