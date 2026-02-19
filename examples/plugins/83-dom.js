console.info('[plugin] 83-dom loaded; DOMException=', typeof DOMException, 'structuredClone=', typeof structuredClone)

command('dom-demo', () => {
  try {
    const e = new DOMException('The operation was aborted.', 'AbortError')
    console.info(
      '[dom-demo] DOMException',
      'name=',
      e.name,
      'message=',
      e.message,
      'code=',
      e.code,
      'instanceof Error=',
      e instanceof Error
    )
  } catch (err) {
    console.error('[dom-demo] DOMException failed', err && err.name, err && err.message)
  }

  try {
    const o = {
      n: 1,
      s: 'hi',
      a: [1, 2, 3],
      m: new Map([
        ['k', { v: 1 }],
        ['x', [1, 2]],
      ]),
      set: new Set([1, 2, 3]),
      buf: new Uint8Array([1, 2, 3]),
      when: new Date(0),
      re: /a+b/gi,
    }
    o.self = o

    const c = structuredClone(o)
    console.info('[dom-demo] structuredClone ok', 'cycle=', c && c.self === c, 'map=', c && c.m && c.m.get('k') && c.m.get('k').v)
  } catch (err) {
    console.error('[dom-demo] structuredClone failed', err && err.name, err && err.message)
  }

  try {
    structuredClone({ fn() {} })
    console.error('[dom-demo] expected function clone to throw')
  } catch (err) {
    console.info('[dom-demo] function clone throws', err && err.name, err && err.message)
  }
})
