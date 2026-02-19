console.info('[plugin] 81-url loaded; URL=', typeof URL, 'URLSearchParams=', typeof URLSearchParams)

command('url', (args) => {
  const s = String(args || '').trim()
  const parts = s ? s.split(/\s+/, 2) : []
  const input = parts[0] || 'https://example.com/a/b?x=1#y'
  const base = parts[1] || ''

  if (typeof URL !== 'function') {
    console.error('[url] URL is not available (did sage:url fail to load?)')
    return
  }

  let u
  try {
    u = base ? new URL(input, base) : new URL(input)
  } catch (e) {
    console.warn('[url] invalid URL:', e && e.name, e && e.message)
    return
  }
  console.info('[url] href', u.href)
  console.info('[url] origin', u.origin)
  console.info('[url] protocol', u.protocol)
  console.info('[url] username', JSON.stringify(u.username), 'password', JSON.stringify(u.password))
  console.info('[url] host', u.host, 'hostname', u.hostname, 'port', JSON.stringify(u.port))
  console.info('[url] pathname', JSON.stringify(u.pathname))
  console.info('[url] search', JSON.stringify(u.search), 'hash', JSON.stringify(u.hash))
  console.info('[url] searchParams', u.searchParams.toString(), 'size', u.searchParams.size)
})

command('url-demo', () => {
  const u1 = new URL('/rel/path?x=1', 'https://example.com/base/dir/')
  console.info('[url-demo] base+rel', u1.href)

  u1.searchParams.append('ts', String(Date.now()))
  console.info('[url-demo] searchParams.append', u1.href)

  console.info('[url-demo] ipv4 canonical', new URL('http://0177.0.0.1/').href)
  try {
    void new URL('http://example.123/')
  } catch (e) {
    console.warn('[url-demo] invalid host', e && e.name, e && e.message)
  }

  console.info('[url-demo] URL.canParse("https://example.com")', URL.canParse('https://example.com'))
  console.info('[url-demo] URL.parse("http:")', URL.parse('http:'))
})
