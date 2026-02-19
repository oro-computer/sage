import cryptoMod from 'sage:crypto'
import perfMod from 'sage:performance'

console.info('[plugin] 82-crypto loaded; crypto=', typeof crypto, 'performance=', typeof performance)
console.info('[plugin] 82-crypto globals match modules:', crypto === cryptoMod, performance === perfMod)

function toInt(s, def) {
  const n = parseInt(String(s || '').trim(), 10)
  return Number.isFinite(n) ? n : def
}

function toHex(bytes) {
  let out = ''
  for (let i = 0; i < bytes.length; i++) {
    out += (bytes[i] + 0x100).toString(16).slice(1)
  }
  return out
}

command('uuid', () => {
  console.info('[crypto] uuid', crypto.randomUUID())
})

command('rand', (args) => {
  const n = Math.max(0, Math.min(toInt(args, 16), 256))
  const b = new Uint8Array(n)
  crypto.getRandomValues(b)
  console.info('[crypto] rand', n, 'bytes:', toHex(b))
})

command('perf', () => {
  console.info('[perf] now(ms)=', performance.now().toFixed(3), 'timeOrigin=', performance.timeOrigin, 'Date.now()=', Date.now())
})

command('crypto-demo', () => {
  exec('uuid')
  exec('rand 16')
  exec('perf')
})

