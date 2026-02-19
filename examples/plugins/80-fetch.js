console.info('[plugin] 80-fetch loaded; fetch=', typeof fetch)

command('fetch', async (args) => {
  const url = String(args || '').trim() || 'https://example.com/'
  console.info('[fetch] GET', url)

  const res = await fetch(url, { timeoutMs: 15_000, maxBytes: 1024 * 1024, followRedirects: true })
  console.info('[fetch] status', res.status, res.statusText, 'url=', res.url)

  const ct = res.headers.get('content-type') || ''
  console.info('[fetch] content-type', ct)

  const text = await res.text()
  console.log('[fetch] body (first 300 chars):\n' + text.slice(0, 300))
})

command('fetch-abort', async (args) => {
  const url = String(args || '').trim() || 'https://example.com/'
  console.info('[fetch] abort demo', url)

  const ac = new AbortController()
  ac.abort()

  try {
    await fetch(url, { signal: ac.signal, timeoutMs: 15_000 })
  } catch (e) {
    console.warn('[fetch] aborted:', e && e.name, e && e.message)
  }
})

command('fetch-post-form', async (args) => {
  const url = String(args || '').trim() || 'https://httpbin.org/post'
  console.info('[fetch] POST FormData', url)

  const fd = new FormData()
  fd.append('hello', 'world')
  fd.append('ts', String(Date.now()))

  const res = await fetch(url, { method: 'POST', body: fd, timeoutMs: 15_000, maxBytes: 1024 * 1024 })
  console.info('[fetch] status', res.status, res.statusText)
  console.log('[fetch] body (first 300 chars):\n' + (await res.text()).slice(0, 300))
})

