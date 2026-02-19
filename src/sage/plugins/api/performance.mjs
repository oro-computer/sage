import { requireHostFunction } from 'sage:internal/host'
import { defineGlobal } from 'sage:core/global'

const nowMs = requireHostFunction('__sage_performance_now')

export class Performance {
  #t0
  constructor() {
    this.#t0 = +nowMs()
    const timeOrigin = Date.now() - this.#t0
    Object.defineProperty(this, 'timeOrigin', {
      value: timeOrigin,
      enumerable: true,
      configurable: false,
      writable: false,
    })
  }

  now() {
    return +nowMs() - this.#t0
  }

  toJSON() {
    return { timeOrigin: this.timeOrigin }
  }
}

export const performance = new Performance()

export function installGlobals() {
  defineGlobal('Performance', Performance)
  defineGlobal('performance', performance)
}

export default performance
