'use strict'

// Sage plugin host bootstrap (evaluated before user plugins).
//
// Goals:
// - Prefer ESM + `sage:*` builtin modules for functionality.
// - Keep the global surface small and browser-like:
//   - EventTarget + Event/CustomEvent
//   - global event helpers:
//     - addEventListener/removeEventListener/dispatchEvent (Event objects)
//     - on/once/off (payload-only helpers; CustomEvent.detail / MessageEvent.data)
//   - console w/ level filtering (SAGE_CONSOLE_LEVEL)
//   - command + exec for `:` integration
//
// The native host emits events by calling `globalThis.__sage_emit(type, payload)`.
// The native host dispatches `:` commands by calling `globalThis.__sage_cmd(name, args)`.

void (function () {
  // ---------------------------------------------------------------------------
  // Minimal DOM-ish event system (EventTarget, Event, CustomEvent, MessageEvent).

  function reportException(e) {
    try {
      if (typeof __sage_report_exception === 'function') {
        __sage_report_exception(e)
      }
    } catch (_) {
      // ignore
    }
  }

  function maybeCatchPromise(r) {
    if (!r) return
    const t = typeof r
    if (t !== 'object' && t !== 'function') return
    const then = r.then
    if (typeof then !== 'function') return
    then.call(
      r,
      function () {},
      function (e) {
        reportException(e)
      }
    )
  }

  class Event {
    constructor(type) {
      this.type = String(type)
      this.target = null
      this.currentTarget = null
      this.defaultPrevented = false
    }
    preventDefault() {
      this.defaultPrevented = true
    }
  }

  class CustomEvent extends Event {
    constructor(type, init) {
      super(type)
      this.detail = init && Object.prototype.hasOwnProperty.call(init, 'detail') ? init.detail : undefined
    }
  }

  class MessageEvent extends Event {
    constructor(type, init) {
      super(type)
      this.data = init && Object.prototype.hasOwnProperty.call(init, 'data') ? init.data : undefined
      this.origin = init && Object.prototype.hasOwnProperty.call(init, 'origin') ? init.origin : ''
      this.lastEventId = init && Object.prototype.hasOwnProperty.call(init, 'lastEventId') ? init.lastEventId : ''
      this.source = init && Object.prototype.hasOwnProperty.call(init, 'source') ? init.source : null
      this.ports = init && Object.prototype.hasOwnProperty.call(init, 'ports') ? init.ports : []
    }
  }

  class EventTarget {
    #listeners
    constructor() {
      this.#listeners = Object.create(null)
    }
    addEventListener(type, fn, opts) {
      if (typeof fn !== 'function') return
      const t = String(type)
      const once = !!(opts && opts.once)
      let arr = this.#listeners[t]
      if (!arr) {
        arr = []
        this.#listeners[t] = arr
      }
      arr.push({ fn, once })
    }
    removeEventListener(type, fn) {
      const t = String(type)
      const arr = this.#listeners[t]
      if (!arr || arr.length === 0) return
      for (let i = arr.length - 1; i >= 0; i--) {
        if (arr[i] && arr[i].fn === fn) {
          arr.splice(i, 1)
        }
      }
      if (arr.length === 0) {
        delete this.#listeners[t]
      }
    }
    dispatchEvent(ev) {
      if (!ev || typeof ev.type !== 'string') {
        throw new TypeError('dispatchEvent(ev): ev.type must be a string')
      }
      ev.target = this
      ev.currentTarget = this
      const arr = this.#listeners[ev.type]
      if (!arr || arr.length === 0) return true
      for (let i = 0; i < arr.length; i++) {
        const it = arr[i]
        if (!it || typeof it.fn !== 'function') continue
        try {
          const r = it.fn(ev)
          maybeCatchPromise(r)
        } catch (e) {
          reportException(e)
        }
        if (it.once) {
          arr.splice(i, 1)
          i--
        }
      }
      return !ev.defaultPrevented
    }
  }

  // Expose event primitives.
  globalThis.EventTarget = EventTarget
  globalThis.Event = Event
  globalThis.CustomEvent = CustomEvent
  globalThis.MessageEvent = MessageEvent

  // Bind an internal event bus to globalThis.
  const bus = new EventTarget()
  function bindGlobal(name, fn) {
    try {
      Object.defineProperty(globalThis, name, {
        value: fn,
        enumerable: false,
        configurable: false,
        writable: false,
      })
    } catch (_) {
      globalThis[name] = fn
    }
  }

  // Stable runtime detection for downstream JS libraries.
  try {
    Object.defineProperty(globalThis, 'isSageRuntime', {
      get() {
        return true
      },
      set(_) {
        // Ignore attempts to override.
      },
      enumerable: false,
      configurable: false,
    })
  } catch (_) {
    // Best-effort fallback.
    globalThis.isSageRuntime = true
  }

  bindGlobal('addEventListener', bus.addEventListener.bind(bus))
  bindGlobal('removeEventListener', bus.removeEventListener.bind(bus))
  bindGlobal('dispatchEvent', bus.dispatchEvent.bind(bus))

  // Convenience aliases: payload-only helpers (CustomEvent.detail / MessageEvent.data).
  const payloadWrappers = new Map() // type -> Map(originalFn -> wrapperFn)
  function payload(ev) {
    if (!ev || typeof ev !== 'object') return ev
    if (ev instanceof CustomEvent) return ev.detail
    if (ev instanceof MessageEvent) return ev.data
    if (Object.prototype.hasOwnProperty.call(ev, 'detail')) return ev.detail
    if (Object.prototype.hasOwnProperty.call(ev, 'data')) return ev.data
    return ev
  }

  function on(type, fn, opts) {
    if (typeof fn !== 'function') return
    const t = String(type)
    let m = payloadWrappers.get(t)
    if (!m) {
      m = new Map()
      payloadWrappers.set(t, m)
    }
    if (m.has(fn)) return
    const wrapper = function (ev) {
      return fn(payload(ev))
    }
    m.set(fn, wrapper)
    return globalThis.addEventListener(t, wrapper, opts)
  }

  function off(type, fn) {
    const t = String(type)
    if (typeof fn !== 'function') return
    const m = payloadWrappers.get(t)
    if (m) {
      const wrapper = m.get(fn)
      if (typeof wrapper === 'function') {
        m.delete(fn)
        if (m.size === 0) payloadWrappers.delete(t)
        return globalThis.removeEventListener(t, wrapper)
      }
    }
    // Fallback: allow removing direct `addEventListener` listeners.
    return globalThis.removeEventListener(t, fn)
  }

  function once(type, fn) {
    if (typeof fn !== 'function') return
    const t = String(type)
    let m = payloadWrappers.get(t)
    if (!m) {
      m = new Map()
      payloadWrappers.set(t, m)
    }
    if (m.has(fn)) return
    const wrapper = function (ev) {
      // Drop wrapper mapping before running user code.
      try {
        const mm = payloadWrappers.get(t)
        if (mm) {
          mm.delete(fn)
          if (mm.size === 0) payloadWrappers.delete(t)
        }
      } catch (_) {
        // ignore
      }
      return fn(payload(ev))
    }
    m.set(fn, wrapper)
    return globalThis.addEventListener(t, wrapper, { once: true })
  }

  bindGlobal('on', on)
  bindGlobal('off', off)
  bindGlobal('once', once)

  // ---------------------------------------------------------------------------
  // Console (level-gated, logs to plugin log file).

  class Console {
    constructor() {}
    _emit(level /* string */, args) {
      try {
        if (typeof __sage_console === 'function') {
          const argv = [String(level)]
          for (let i = 0; i < args.length; i++) argv.push(args[i])
          return __sage_console.apply(null, argv)
        }
        // Back-compat: fall back to verbose-only logging.
        if (typeof __sage_log === 'function') {
          return __sage_log.apply(null, args)
        }
      } catch (e) {
        reportException(e)
      }
    }
    log() {
      return this._emit('log', arguments)
    }
    info() {
      return this._emit('info', arguments)
    }
    warn() {
      return this._emit('warn', arguments)
    }
    error() {
      return this._emit('error', arguments)
    }
    verbose() {
      return this._emit('verbose', arguments)
    }
    debug() {
      return this._emit('debug', arguments)
    }
  }

  globalThis.Console = Console
  bindGlobal('console', new Console())

  // ---------------------------------------------------------------------------
  // queueMicrotask.

  if (typeof globalThis.queueMicrotask !== 'function') {
    bindGlobal('queueMicrotask', function queueMicrotask(fn) {
      if (typeof fn !== 'function') {
        throw new TypeError('queueMicrotask(fn): fn must be a function')
      }
      maybeCatchPromise(
        Promise.resolve().then(function () {
          return fn()
        })
      )
    })
  }

  // ---------------------------------------------------------------------------
  // `:` command integration.

  const commands = Object.create(null)

  function normCommandName(name) {
    try {
      return String(name).trim().toLowerCase()
    } catch (_) {
      return ''
    }
  }

  function command(name, fn) {
    if (typeof name !== 'string') {
      throw new TypeError('command(name, fn): name must be a string')
    }
    if (typeof fn !== 'function') {
      throw new TypeError('command(name, fn): fn must be a function')
    }
    const key = normCommandName(name)
    if (!key) {
      throw new TypeError('command(name, fn): name must be non-empty')
    }
    commands[key] = fn
  }

  function exec(cmd) {
    if (typeof cmd !== 'string') {
      throw new TypeError('exec(cmd): cmd must be a string')
    }
    if (typeof __sage_exec !== 'function') {
      return false
    }
    // Host returns 0 on success.
    return __sage_exec(cmd) === 0
  }

  bindGlobal('command', command)
  bindGlobal('exec', exec)

  // ---------------------------------------------------------------------------
  // Host entry points (captured by native code).

  function __sage_emit(type, payload) {
    const ev = new CustomEvent(String(type), { detail: payload })
    globalThis.dispatchEvent(ev)
  }

  function __sage_cmd(name, args) {
    const key = normCommandName(name)
    if (!key) return false
    const fn = commands[key]
    if (typeof fn !== 'function') return false
    try {
      const r = fn(typeof args === 'string' ? args : '')
      maybeCatchPromise(r)
    } catch (e) {
      reportException(e)
    }
    return true
  }

  bindGlobal('__sage_emit', __sage_emit)
  bindGlobal('__sage_cmd', __sage_cmd)

  // ---------------------------------------------------------------------------
  // Navigator (global + module via `sage:navigator`).

  function optFn(name) {
    const fn = globalThis[name]
    return typeof fn === 'function' ? fn : null
  }
  const appVerFn = optFn('__sage_app_version')
  const qjsVerFn = optFn('__sage_qjs_version')
  const appName = 'sage'
  const appVersion = appVerFn ? String(appVerFn()) : ''
  const qjsVersion = qjsVerFn ? String(qjsVerFn()) : ''

  class Navigator {
    constructor() {
      this.appName = appName
      this.appVersion = appVersion
      this.userAgent =
        (appVersion ? appName + '/' + appVersion + ' ' : appName + ' ') + 'quickjs/' + qjsVersion
    }
  }

  globalThis.Navigator = Navigator
  bindGlobal('navigator', new Navigator())

  // ---------------------------------------------------------------------------
  // URL + URLSearchParams (ESM builtin: `sage:url`).
  // DOMException + structuredClone (ESM builtin: `sage:core/dom`).
  // Web core primitives (ESM builtin: `sage:core/web`).
  // Fetch (ESM builtin: `sage:fetch`).
  // Crypto + performance (ESM builtin: `sage:crypto` / `sage:performance`).
  //
  // We use dynamic import so this bootstrap stays a script (and so `EventTarget`
  // exists before AbortSignal installs).

  try {
    import('sage:performance').then(
      function (m) {
        try {
          if (m && typeof m.installGlobals === 'function') {
            m.installGlobals()
          }
        } catch (e) {
          reportException(e)
        }
      },
      function (e) {
        reportException(e)
      }
    )
  } catch (e) {
    reportException(e)
  }

  try {
    import('sage:crypto').then(
      function (m) {
        try {
          if (m && typeof m.installGlobals === 'function') {
            m.installGlobals()
          }
        } catch (e) {
          reportException(e)
        }
      },
      function (e) {
        reportException(e)
      }
    )
  } catch (e) {
    reportException(e)
  }

  try {
    import('sage:url').then(
      function (m) {
        try {
          if (m && typeof m.installGlobals === 'function') {
            m.installGlobals()
          }
        } catch (e) {
          reportException(e)
        }
      },
      function (e) {
        reportException(e)
      }
    )
  } catch (e) {
    reportException(e)
  }

  try {
    import('sage:core/dom').then(
      function (m) {
        try {
          if (m && typeof m.installGlobals === 'function') {
            m.installGlobals()
          }
        } catch (e) {
          reportException(e)
        }
      },
      function (e) {
        reportException(e)
      }
    )
  } catch (e) {
    reportException(e)
  }

  try {
    import('sage:core/web').then(
      function (m) {
        try {
          if (m && typeof m.installGlobals === 'function') {
            m.installGlobals()
          }
        } catch (e) {
          reportException(e)
        }
      },
      function (e) {
        reportException(e)
      }
    )
  } catch (e) {
    reportException(e)
  }

  try {
    import('sage:fetch').then(
      function (m) {
        try {
          if (m && typeof m.installGlobals === 'function') {
            m.installGlobals()
          }
        } catch (e) {
          reportException(e)
        }
      },
      function (e) {
        reportException(e)
      }
    )
  } catch (e) {
    reportException(e)
  }
})()
