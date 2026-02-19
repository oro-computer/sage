'use strict'

// Tracks simple session stats from events and exposes `:` commands to inspect
// and reset them.
//
// Console output is filtered by `SAGE_CONSOLE_LEVEL` (default: `warn`).
//
// Install:
//   mkdir -p ~/.config/sage/plugins
//   cp examples/plugins/20-session-stats.js ~/.config/sage/plugins/
//
// Commands:
//   :stats
//   :stats reset
//   :stats-reset

const state = {
  opened: 0,
  tab_changes: 0,
  searches: 0,
  copies: 0,
  copied_bytes: 0,

  path: '',
  tab: 0,
  tab_count: 0,

  last_search: '',
  last_search_regex: false,
  last_search_ignore_case: false,
}

function resetCounters() {
  state.opened = 0
  state.tab_changes = 0
  state.searches = 0
  state.copies = 0
  state.copied_bytes = 0
  state.last_search = ''
  state.last_search_regex = false
  state.last_search_ignore_case = false
}

function dump(prefix) {
  console.info(
    prefix,
    `tab=${state.tab}/${state.tab_count}`,
    `path=${state.path || '(none)'}`,
    `opened=${state.opened}`,
    `tab_changes=${state.tab_changes}`,
    `searches=${state.searches}`,
    `copies=${state.copies}`,
    `copied_bytes=${state.copied_bytes}`
  )
  if (state.last_search) {
    console.debug(
      prefix,
      'last_search=',
      JSON.stringify({
        query: state.last_search,
        regex: state.last_search_regex,
        ignore_case: state.last_search_ignore_case,
      })
    )
  }
}

on('open', (p) => {
  state.opened++
  state.path = String(p && p.path ? p.path : '')
  state.tab = Number(p && p.tab ? p.tab : 0) || 0
  state.tab_count = Number(p && p.tab_count ? p.tab_count : 0) || 0
})

on('tab_change', (p) => {
  state.tab_changes++
  state.tab = Number(p && p.to ? p.to : 0) || state.tab
  state.tab_count = Number(p && p.tab_count ? p.tab_count : 0) || state.tab_count
})

on('search', (p) => {
  state.searches++
  state.last_search = String(p && p.query ? p.query : '')
  state.last_search_regex = !!(p && p.regex)
  state.last_search_ignore_case = !!(p && p.ignore_case)
})

on('copy', (p) => {
  state.copies++
  const n = Number(p && p.bytes ? p.bytes : 0) || 0
  if (n > 0) state.copied_bytes += n
})

on('quit', () => {
  dump('stats(final)')
})

command('stats', (args) => {
  const a = String(args || '').trim().toLowerCase()
  if (a === 'reset' || a === 'clear') {
    resetCounters()
    dump('stats(reset)')
    return
  }
  dump('stats')
})

command('stats-reset', () => {
  resetCounters()
  dump('stats(reset)')
})
