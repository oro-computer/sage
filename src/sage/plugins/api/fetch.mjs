import { requireHostFunction } from 'sage:internal/host'
import { defineGlobal } from 'sage:core/global'
import { DOMException } from 'sage:core/dom'
import {
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
  installGlobals as installWebGlobals,
} from 'sage:core/web'

export {
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
}

const fetchHost = requireHostFunction('__sage_fetch')
const fetchAbortHost = requireHostFunction('__sage_fetch_abort')

export async function fetch(input, init) {
  const req = new Request(input, init)
  const hostReq = toHostFetchRequest(req)

  const o = init && typeof init === 'object' ? init : null
  const timeoutMs = o && typeof o.timeoutMs === 'number' ? o.timeoutMs : 30_000
  const maxBytes = o && typeof o.maxBytes === 'number' ? o.maxBytes : 16 * 1024 * 1024
  const followRedirects = o && Object.prototype.hasOwnProperty.call(o, 'followRedirects') ? !!o.followRedirects : true

  const signal = hostReq.signal
  if (signal && typeof signal === 'object') {
    let aborted = false
    try {
      aborted = !!signal.aborted
    } catch (_) {
      aborted = false
    }

    if (aborted) {
      let reason = undefined
      try {
        if (Object.prototype.hasOwnProperty.call(signal, 'reason')) reason = signal.reason
      } catch (_) {
        reason = undefined
      }
      if (reason !== undefined) throw reason
      throw new DOMException('The operation was aborted.', 'AbortError')
    }
  }

  const p = fetchHost(hostReq.url, {
    method: hostReq.method,
    headers: hostReq.headers,
    body: hostReq.body,
    timeoutMs,
    maxBytes,
    followRedirects,
  })

  const fetchId = p && typeof p === 'object' ? p.sageFetchId : 0

  let abortListener = null
  if (fetchId && signal && typeof signal === 'object' && typeof signal.addEventListener === 'function') {
    abortListener = () => {
      try {
        fetchAbortHost(fetchId)
      } catch (_) {
        // ignore
      }
    }
    try {
      signal.addEventListener('abort', abortListener, { once: true })
    } catch (_) {
      abortListener = null
    }
    try {
      if (signal.aborted) abortListener && abortListener()
    } catch (_) {
      // ignore
    }
  }

  try {
    const raw = await p
    return new Response(raw.body, {
      status: raw.status,
      statusText: raw.statusText,
      headers: raw.headers,
      url: raw.url,
    })
  } finally {
    if (signal && abortListener && typeof signal.removeEventListener === 'function') {
      try {
        signal.removeEventListener('abort', abortListener)
      } catch (_) {
        // ignore
      }
    }
  }
}

export function installGlobals() {
  if (typeof globalThis.Request !== 'function' || typeof globalThis.Response !== 'function') {
    try {
      installWebGlobals()
    } catch (_) {
      // ignore
    }
  }

  defineGlobal('fetch', fetch)
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
  fetch,
  installGlobals,
})
