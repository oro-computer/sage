'use strict';

// Sage plugin host bootstrap (evaluated before user plugins).
//
// Provides a small stable JS API surface:
// - `sage.on(event, fn)`    register an event listener
// - `sage.log(...args)`     verbose logging (no-op unless `sage` runs with `--verbose`)
//
// Events are emitted from the native host via `sage.__emit(event, payload)`.

(function () {
  const listeners = Object.create(null);

  function on(event, fn) {
    if (typeof event !== 'string') {
      throw new TypeError('sage.on(event, fn): event must be a string');
    }
    if (typeof fn !== 'function') {
      throw new TypeError('sage.on(event, fn): fn must be a function');
    }
    let arr = listeners[event];
    if (!arr) {
      arr = [];
      listeners[event] = arr;
    }
    arr.push(fn);
  }

  function log() {
    try {
      if (typeof __sage_log === 'function') {
        // Avoid spread to keep this compatible with very old runtimes.
        return __sage_log.apply(null, arguments);
      }
    } catch (_) {
      // ignore
    }
  }

  function emit(event, payload) {
    const arr = listeners[event];
    if (!arr || arr.length === 0) {
      return;
    }
    for (let i = 0; i < arr.length; i++) {
      const fn = arr[i];
      if (typeof fn !== 'function') {
        continue;
      }
      try {
        fn(payload);
      } catch (e) {
        try {
          if (typeof __sage_report_exception === 'function') {
            __sage_report_exception(e);
          }
        } catch (_) {
          // ignore
        }
      }
    }
  }

  const api = {
    on,
    log,
    __emit: emit,
  };

  try {
    Object.defineProperty(globalThis, 'sage', {
      value: Object.freeze(api),
      enumerable: false,
      configurable: false,
      writable: false,
    });
  } catch (_) {
    globalThis.sage = api;
  }
})();

