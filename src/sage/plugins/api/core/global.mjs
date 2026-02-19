export function defineGlobal(name, value) {
  try {
    Object.defineProperty(globalThis, name, {
      value,
      enumerable: false,
      configurable: true,
      writable: true,
    })
  } catch (_) {
    globalThis[name] = value
  }
}

export default Object.freeze({ defineGlobal })
