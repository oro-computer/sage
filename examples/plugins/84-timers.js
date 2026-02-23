console.info('[plugin] 84-timers loaded; setTimeout=', typeof setTimeout, 'setInterval=', typeof setInterval, 'sleep=', typeof sleep)

command('timers-demo', async () => {
  console.info('[timers-demo] start')

  setTimeout(
    (a, b) => {
      console.info('[timers-demo] timeout fired', 'a=', a, 'b=', b)
    },
    10,
    'hello',
    42
  )

  await sleep(50)
  console.info('[timers-demo] after sleep(50)')

  let ticks = 0
  const id = setInterval(() => {
    ticks++
    console.debug('[timers-demo] tick', ticks)
    if (ticks >= 3) {
      clearInterval(id)
      console.info('[timers-demo] done (interval cleared)')
    }
  }, 25)
})
