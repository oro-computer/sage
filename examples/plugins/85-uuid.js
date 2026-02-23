import uuid from 'sage:uuid'

console.info('[plugin] 85-uuid loaded; uuid.v4=', typeof uuid.v4, 'uuid.v7=', typeof uuid.v7)

function toInt(s, def) {
  const n = parseInt(String(s || '').trim(), 10)
  return Number.isFinite(n) ? n : def
}

command('uuid-v4', () => {
  console.info('[uuid] v4', uuid.v4())
})

command('uuid-v7', (args) => {
  const ms = args ? toInt(args, -1) : -1
  console.info('[uuid] v7', ms >= 0 ? uuid.v7(ms) : uuid.v7())
})

command('uuid-demo', () => {
  exec('uuid-v4')
  exec('uuid-v7')
})

