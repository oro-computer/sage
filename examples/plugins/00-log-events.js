'use strict';

// Logs high-level `sage` events.
//
// Run with `--verbose` to enable `sage.log(...)` output.
// Logs are written to `$SAGE_PLUGIN_LOG` (if set), else `$XDG_CACHE_HOME/sage/plugins.log`,
// else `~/.cache/sage/plugins.log`.
//
// Install:
//   mkdir -p ~/.config/sage/plugins
//   cp examples/plugins/00-log-events.js ~/.config/sage/plugins/

sage.on('open', (p) => {
  sage.log('open', p.path, `tab=${p.tab}/${p.tab_count}`);
});

sage.on('tab_change', (p) => {
  sage.log('tab_change', `${p.from} -> ${p.to}`, `tabs=${p.tab_count}`);
});

sage.on('search', (p) => {
  sage.log('search', JSON.stringify({ q: p.query, regex: p.regex, ignore_case: p.ignore_case }));
});

sage.on('copy', (p) => {
  sage.log('copy', `${p.bytes} bytes`);
});

sage.on('quit', () => {
  sage.log('quit');
});
