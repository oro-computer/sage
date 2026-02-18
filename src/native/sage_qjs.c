#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "quickjs.h"

typedef struct SageQjs {
  JSRuntime *rt;
  JSContext *ctx;
  JSValue emit_fn;
  int verbose;
  uint32_t load_timeout_ms;
  uint32_t event_timeout_ms;
  uint64_t deadline_ns;
  int timed_out;
  int disabled;
  int had_error;
  uint64_t mem_limit_bytes;
  uint64_t stack_limit_bytes;
  char *log_path;
  FILE *log_file;
  int log_stderr;
} SageQjs;

static uint64_t sage_qjs_now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static uint64_t sage_qjs_env_u64(const char *key, uint64_t def) {
  const char *s = getenv(key);
  if (!s || !*s) {
    return def;
  }
  errno = 0;
  char *end = NULL;
  unsigned long long v = strtoull(s, &end, 10);
  if (errno != 0 || !end || end == s) {
    return def;
  }
  return (uint64_t)v;
}

static uint32_t sage_qjs_env_u32(const char *key, uint32_t def) {
  uint64_t v = sage_qjs_env_u64(key, (uint64_t)def);
  if (v > 0xffffffffull) {
    return def;
  }
  return (uint32_t)v;
}

static void sage_qjs_begin_budget(SageQjs *q, uint32_t ms) {
  if (!q || q->disabled) {
    return;
  }
  q->timed_out = 0;
  if (ms == 0) {
    q->deadline_ns = 0;
    return;
  }
  uint64_t now = sage_qjs_now_ns();
  if (now == 0) {
    q->deadline_ns = 0;
    return;
  }
  q->deadline_ns = now + ((uint64_t)ms * 1000000ull);
}

static void sage_qjs_end_budget(SageQjs *q) {
  if (!q) {
    return;
  }
  q->deadline_ns = 0;
}

static int sage_qjs_interrupt_handler(JSRuntime *rt, void *opaque) {
  (void)rt;
  SageQjs *q = (SageQjs *)opaque;
  if (!q || q->disabled) {
    return 0;
  }
  if (q->deadline_ns == 0) {
    return 0;
  }
  uint64_t now = sage_qjs_now_ns();
  if (now == 0) {
    return 0;
  }
  if (now >= q->deadline_ns) {
    q->timed_out = 1;
    return 1;
  }
  return 0;
}

static int sage_qjs_mkdir_p(const char *dir, mode_t mode) {
  if (!dir || !*dir) {
    return -1;
  }

  char *tmp = strdup(dir);
  if (!tmp) {
    return -1;
  }

  size_t len = strlen(tmp);
  while (len > 0 && tmp[len - 1] == '/') {
    tmp[len - 1] = '\0';
    len--;
  }

  for (char *p = tmp + 1; *p; p++) {
    if (*p != '/') {
      continue;
    }
    *p = '\0';
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
      *p = '/';
      free(tmp);
      return -1;
    }
    *p = '/';
  }

  if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
    free(tmp);
    return -1;
  }

  free(tmp);
  return 0;
}

static char *sage_qjs_default_log_path(void) {
  const char *p = getenv("SAGE_PLUGIN_LOG");
  if (p && *p) {
    return strdup(p);
  }

  const char *xdg = getenv("XDG_CACHE_HOME");
  if (xdg && *xdg) {
    size_t n = strlen(xdg) + strlen("/sage/plugins.log") + 1;
    char *out = (char *)malloc(n);
    if (!out) {
      return NULL;
    }
    snprintf(out, n, "%s/sage/plugins.log", xdg);
    return out;
  }

  const char *home = getenv("HOME");
  if (home && *home) {
    size_t n = strlen(home) + strlen("/.cache/sage/plugins.log") + 1;
    char *out = (char *)malloc(n);
    if (!out) {
      return NULL;
    }
    snprintf(out, n, "%s/.cache/sage/plugins.log", home);
    return out;
  }

  return NULL;
}

static FILE *sage_qjs_log_file(SageQjs *q) {
  if (!q) {
    return NULL;
  }
  if (q->log_file) {
    return q->log_file;
  }
  if (!q->log_path || !*q->log_path) {
    return NULL;
  }

  char *tmp = strdup(q->log_path);
  if (!tmp) {
    return NULL;
  }
  char *slash = strrchr(tmp, '/');
  if (slash) {
    *slash = '\0';
    (void)sage_qjs_mkdir_p(tmp, 0755);
  }
  free(tmp);

  FILE *f = fopen(q->log_path, "a");
  if (!f) {
    return NULL;
  }

  setvbuf(f, NULL, _IOLBF, 0);
  q->log_file = f;
  return q->log_file;
}

static FILE *sage_qjs_log_stream(SageQjs *q) {
  if (q && q->log_stderr) {
    return stderr;
  }
  FILE *f = sage_qjs_log_file(q);
  if (f) {
    return f;
  }
  return stderr;
}

static void sage_qjs_print_value_to(JSContext *ctx, JSValueConst v, FILE *out) {
  const char *s = JS_ToCString(ctx, v);
  if (s) {
    fputs(s, out);
    JS_FreeCString(ctx, s);
  } else {
    fputs("<non-string>", out);
  }
}

static void sage_qjs_dump_exception_ctx(JSContext *ctx, int verbose) {
  SageQjs *q = (SageQjs *)JS_GetContextOpaque(ctx);
  FILE *out = sage_qjs_log_stream(q);
  if (q) {
    q->had_error = 1;
  }
  JSValue exc = JS_GetException(ctx);

  fputs("sage[plugin] exception: ", out);
  sage_qjs_print_value_to(ctx, exc, out);
  fputc('\n', out);

  if (verbose) {
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (!JS_IsUndefined(stack) && !JS_IsNull(stack)) {
      fputs("sage[plugin] stack: ", out);
      sage_qjs_print_value_to(ctx, stack, out);
      fputc('\n', out);
    }
    JS_FreeValue(ctx, stack);
  }

  JS_FreeValue(ctx, exc);
  fflush(out);
}

static void sage_qjs_dump_exception(SageQjs *q) {
  if (!q || !q->ctx) {
    return;
  }
  sage_qjs_dump_exception_ctx(q->ctx, q->verbose);
}

static void sage_qjs_drain_jobs(SageQjs *q) {
  if (!q || !q->rt) {
    return;
  }
  if (q->disabled) {
    return;
  }
  JSContext *job_ctx = NULL;
  int iters = 0;
  while (iters < 1024) {
    int rc = JS_ExecutePendingJob(q->rt, &job_ctx);
    if (rc <= 0) {
      if (rc < 0 && job_ctx) {
        sage_qjs_dump_exception_ctx(job_ctx, q->verbose);
      }
      break;
    }
    iters++;
  }
  if (q->timed_out) {
    FILE *out = sage_qjs_log_stream(q);
    fputs("sage[plugin] timeout while draining jobs; disabling plugins for session\n",
          out);
    fflush(out);
    q->disabled = 1;
    q->had_error = 1;
  }
}

static JSValue js_sage_log(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  (void)this_val;
  SageQjs *q = (SageQjs *)JS_GetContextOpaque(ctx);
  if (!q || !q->verbose) {
    return JS_UNDEFINED;
  }

  FILE *out = sage_qjs_log_stream(q);
  fputs("sage[js]", out);
  for (int i = 0; i < argc; i++) {
    fputc(' ', out);
    sage_qjs_print_value_to(ctx, argv[i], out);
  }
  fputc('\n', out);
  fflush(out);
  return JS_UNDEFINED;
}

static JSValue js_sage_report_exception(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  (void)this_val;
  SageQjs *q = (SageQjs *)JS_GetContextOpaque(ctx);
  int verbose = q ? q->verbose : 0;
  FILE *out = sage_qjs_log_stream(q);
  if (q) {
    q->had_error = 1;
  }

  JSValue exc = (argc >= 1) ? JS_DupValue(ctx, argv[0]) : JS_GetException(ctx);
  fputs("sage[plugin] error: ", out);
  sage_qjs_print_value_to(ctx, exc, out);
  fputc('\n', out);

  if (verbose) {
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (!JS_IsUndefined(stack) && !JS_IsNull(stack)) {
      fputs("sage[plugin] stack: ", out);
      sage_qjs_print_value_to(ctx, stack, out);
      fputc('\n', out);
    }
    JS_FreeValue(ctx, stack);
  }

  JS_FreeValue(ctx, exc);
  fflush(out);
  return JS_UNDEFINED;
}

static int sage_qjs_define_host_api(SageQjs *q) {
  JSContext *ctx = q->ctx;
  JSValue global = JS_GetGlobalObject(ctx);

  JS_SetPropertyStr(ctx, global, "__sage_log",
                    JS_NewCFunction(ctx, js_sage_log, "__sage_log", 1));
  JS_SetPropertyStr(ctx, global, "__sage_report_exception",
                    JS_NewCFunction(ctx, js_sage_report_exception,
                                    "__sage_report_exception", 1));

  JS_FreeValue(ctx, global);
  return 0;
}

SageQjs *sage_qjs_new(int64_t verbose) {
  SageQjs *q = (SageQjs *)calloc(1, sizeof(SageQjs));
  if (!q) {
    return NULL;
  }

  q->verbose = (verbose != 0);
  q->emit_fn = JS_UNDEFINED;
  q->load_timeout_ms = sage_qjs_env_u32("SAGE_PLUGIN_LOAD_TIMEOUT_MS", 500);
  q->event_timeout_ms = sage_qjs_env_u32("SAGE_PLUGIN_EVENT_TIMEOUT_MS", 50);
  q->deadline_ns = 0;
  q->timed_out = 0;
  q->disabled = 0;
  q->had_error = 0;
  q->log_path = sage_qjs_default_log_path();
  q->log_file = NULL;
  q->log_stderr = (sage_qjs_env_u64("SAGE_PLUGIN_LOG_STDERR", 0) != 0);

  uint64_t mem_mb = sage_qjs_env_u64("SAGE_PLUGIN_MEM_LIMIT_MB", 64);
  q->mem_limit_bytes = mem_mb ? (mem_mb * 1024ull * 1024ull) : 0;

  uint64_t stack_kb = sage_qjs_env_u64("SAGE_PLUGIN_STACK_LIMIT_KB", 1024);
  q->stack_limit_bytes = stack_kb ? (stack_kb * 1024ull) : 0;

  q->rt = JS_NewRuntime();
  if (!q->rt) {
    free(q->log_path);
    free(q);
    return NULL;
  }

  JS_SetInterruptHandler(q->rt, sage_qjs_interrupt_handler, q);
  JS_SetCanBlock(q->rt, false);
  if (q->mem_limit_bytes) {
    JS_SetMemoryLimit(q->rt, (size_t)q->mem_limit_bytes);
  }
  if (q->stack_limit_bytes) {
    JS_SetMaxStackSize(q->rt, (size_t)q->stack_limit_bytes);
  }

  q->ctx = JS_NewContext(q->rt);
  if (!q->ctx) {
    JS_FreeRuntime(q->rt);
    free(q->log_path);
    free(q);
    return NULL;
  }

  JS_SetContextOpaque(q->ctx, q);
  (void)sage_qjs_define_host_api(q);

  if (q->verbose && q->log_path) {
    fprintf(stderr, "sage[plugin] log: %s\n", q->log_path);
    fflush(stderr);
  }
  return q;
}

void sage_qjs_free(SageQjs *q) {
  if (!q) {
    return;
  }
  if (q->ctx) {
    if (!JS_IsUndefined(q->emit_fn)) {
      JS_FreeValue(q->ctx, q->emit_fn);
      q->emit_fn = JS_UNDEFINED;
    }
    JS_FreeContext(q->ctx);
    q->ctx = NULL;
  }
  if (q->rt) {
    JS_FreeRuntime(q->rt);
    q->rt = NULL;
  }
  if (q->log_file) {
    fclose(q->log_file);
    q->log_file = NULL;
  }
  free(q->log_path);
  q->log_path = NULL;
  free(q);
}

void sage_qjs_set_timeouts_ms(SageQjs *q, int64_t load_ms, int64_t event_ms) {
  if (!q) {
    return;
  }
  if (load_ms >= 0 && load_ms <= 0xffffffffll) {
    q->load_timeout_ms = (uint32_t)load_ms;
  }
  if (event_ms >= 0 && event_ms <= 0xffffffffll) {
    q->event_timeout_ms = (uint32_t)event_ms;
  }
}

void sage_qjs_set_limits(SageQjs *q, int64_t mem_limit_bytes,
                         int64_t stack_limit_bytes) {
  if (!q || !q->rt) {
    return;
  }
  if (mem_limit_bytes >= 0) {
    q->mem_limit_bytes = (uint64_t)mem_limit_bytes;
    JS_SetMemoryLimit(q->rt, (size_t)q->mem_limit_bytes);
  }
  if (stack_limit_bytes >= 0) {
    q->stack_limit_bytes = (uint64_t)stack_limit_bytes;
    JS_SetMaxStackSize(q->rt, (size_t)q->stack_limit_bytes);
  }
}

int64_t sage_qjs_set_log_path(SageQjs *q, const char *path) {
  if (!q) {
    return 1;
  }
  if (q->log_file) {
    fclose(q->log_file);
    q->log_file = NULL;
  }
  free(q->log_path);
  q->log_path = NULL;

  if (path && *path) {
    q->log_path = strdup(path);
    if (!q->log_path) {
      return 1;
    }
  }
  return 0;
}

int64_t sage_qjs_take_error(SageQjs *q) {
  if (!q) {
    return 0;
  }
  int v = q->had_error;
  q->had_error = 0;
  return v ? 1 : 0;
}

int64_t sage_qjs_eval_bootstrap(SageQjs *q, const char *source) {
  if (!q || !q->ctx || !source) {
    return 1;
  }
  if (q->disabled) {
    return 1;
  }

  JSContext *ctx = q->ctx;
  sage_qjs_begin_budget(q, q->load_timeout_ms);
  JSValue val =
      JS_Eval(ctx, source, strlen(source), "<sage-bootstrap>", JS_EVAL_TYPE_GLOBAL);
  if (q->timed_out) {
    FILE *out = sage_qjs_log_stream(q);
    fputs("sage[plugin] bootstrap timed out; disabling plugins for session\n",
          out);
    fflush(out);
    q->disabled = 1;
    q->had_error = 1;
  }
  if (JS_IsException(val)) {
    sage_qjs_end_budget(q);
    sage_qjs_dump_exception(q);
    JS_FreeValue(ctx, val);
    return 1;
  }
  JS_FreeValue(ctx, val);
  if (q->disabled) {
    sage_qjs_end_budget(q);
    return 1;
  }
  sage_qjs_drain_jobs(q);
  sage_qjs_end_budget(q);
  if (q->disabled) {
    return 1;
  }

  JSValue global = JS_GetGlobalObject(ctx);
  JSValue sage = JS_GetPropertyStr(ctx, global, "sage");
  if (JS_IsUndefined(sage) || JS_IsNull(sage) || !JS_IsObject(sage)) {
    JS_FreeValue(ctx, sage);
    JS_FreeValue(ctx, global);
    FILE *out = sage_qjs_log_stream(q);
    fputs("sage[plugin] bootstrap did not define global `sage`\n", out);
    fflush(out);
    q->had_error = 1;
    return 1;
  }

  JSValue emit = JS_GetPropertyStr(ctx, sage, "__emit");
  if (!JS_IsFunction(ctx, emit)) {
    JS_FreeValue(ctx, emit);
    JS_FreeValue(ctx, sage);
    JS_FreeValue(ctx, global);
    FILE *out = sage_qjs_log_stream(q);
    fputs("sage[plugin] bootstrap missing `sage.__emit`\n", out);
    fflush(out);
    q->had_error = 1;
    return 1;
  }

  if (!JS_IsUndefined(q->emit_fn)) {
    JS_FreeValue(ctx, q->emit_fn);
  }
  q->emit_fn = emit; // owned ref

  JS_FreeValue(ctx, sage);
  JS_FreeValue(ctx, global);
  return 0;
}

static int sage_qjs_read_file(const char *path, uint8_t **out_buf,
                              size_t *out_len) {
  *out_buf = NULL;
  *out_len = 0;

  FILE *f = fopen(path, "rb");
  if (!f) {
    return -1;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return -1;
  }
  long n_long = ftell(f);
  if (n_long < 0) {
    fclose(f);
    return -1;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return -1;
  }

  size_t n = (size_t)n_long;
  uint8_t *buf = (uint8_t *)malloc(n + 1);
  if (!buf) {
    fclose(f);
    return -1;
  }

  size_t got = fread(buf, 1, n, f);
  fclose(f);
  if (got != n) {
    free(buf);
    return -1;
  }

  buf[n] = 0;
  *out_buf = buf;
  *out_len = n;
  return 0;
}

int64_t sage_qjs_eval_file(SageQjs *q, const char *path) {
  if (!q || !q->ctx || !path) {
    return 1;
  }
  if (q->disabled) {
    return 1;
  }

  uint8_t *buf = NULL;
  size_t len = 0;
  if (sage_qjs_read_file(path, &buf, &len) != 0) {
    FILE *out = sage_qjs_log_stream(q);
    fprintf(out, "sage[plugin] failed to read plugin: %s\n", path);
    fflush(out);
    q->had_error = 1;
    return 1;
  }

  JSContext *ctx = q->ctx;
  sage_qjs_begin_budget(q, q->load_timeout_ms);
  JSValue val = JS_Eval(ctx, (const char *)buf, len, path, JS_EVAL_TYPE_GLOBAL);
  free(buf);
  if (q->timed_out) {
    FILE *out = sage_qjs_log_stream(q);
    fprintf(out,
            "sage[plugin] plugin load timed out (%s); disabling plugins for session\n",
            path);
    fflush(out);
    q->disabled = 1;
    q->had_error = 1;
  }
  if (JS_IsException(val)) {
    sage_qjs_end_budget(q);
    sage_qjs_dump_exception(q);
    JS_FreeValue(ctx, val);
    return 1;
  }
  JS_FreeValue(ctx, val);
  if (q->disabled) {
    sage_qjs_end_budget(q);
    return 1;
  }
  sage_qjs_drain_jobs(q);
  sage_qjs_end_budget(q);
  if (q->disabled) {
    return 1;
  }
  return 0;
}

static int64_t sage_qjs_emit_event(SageQjs *q, const char *event, JSValue payload) {
  if (!q || !q->ctx || !event) {
    if (q && q->ctx) {
      JS_FreeValue(q->ctx, payload);
    }
    return 1;
  }
  if (q->disabled) {
    if (!JS_IsUndefined(payload)) {
      JS_FreeValue(q->ctx, payload);
    }
    return 1;
  }
  if (JS_IsUndefined(q->emit_fn)) {
    if (!JS_IsUndefined(payload)) {
      JS_FreeValue(q->ctx, payload);
    }
    return 0;
  }

  JSContext *ctx = q->ctx;
  JSValue ev = JS_NewString(ctx, event);
  JSValue argv[2] = {ev, payload};
  sage_qjs_begin_budget(q, q->event_timeout_ms);
  JSValue ret = JS_Call(ctx, q->emit_fn, JS_UNDEFINED, 2, argv);
  JS_FreeValue(ctx, ev);
  JS_FreeValue(ctx, payload);
  if (q->timed_out) {
    FILE *out = sage_qjs_log_stream(q);
    fprintf(out,
            "sage[plugin] event timed out (%s); disabling plugins for session\n",
            event);
    fflush(out);
    q->disabled = 1;
    q->had_error = 1;
  }
  if (JS_IsException(ret)) {
    sage_qjs_end_budget(q);
    sage_qjs_dump_exception(q);
    JS_FreeValue(ctx, ret);
    return 1;
  }
  JS_FreeValue(ctx, ret);
  if (q->disabled) {
    sage_qjs_end_budget(q);
    return 1;
  }
  sage_qjs_drain_jobs(q);
  sage_qjs_end_budget(q);
  return q->disabled ? 1 : 0;
}

int64_t sage_qjs_emit_open(SageQjs *q, const char *path, int64_t tab,
                           int64_t tab_count) {
  if (!q || !q->ctx) {
    return 1;
  }
  JSContext *ctx = q->ctx;
  JSValue payload = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, payload, "path", JS_NewString(ctx, path ? path : ""));
  JS_SetPropertyStr(ctx, payload, "tab", JS_NewInt64(ctx, tab));
  JS_SetPropertyStr(ctx, payload, "tab_count", JS_NewInt64(ctx, tab_count));
  return sage_qjs_emit_event(q, "open", payload);
}

int64_t sage_qjs_emit_tab_change(SageQjs *q, int64_t from, int64_t to,
                                 int64_t tab_count) {
  if (!q || !q->ctx) {
    return 1;
  }
  JSContext *ctx = q->ctx;
  JSValue payload = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, payload, "from", JS_NewInt64(ctx, from));
  JS_SetPropertyStr(ctx, payload, "to", JS_NewInt64(ctx, to));
  JS_SetPropertyStr(ctx, payload, "tab_count", JS_NewInt64(ctx, tab_count));
  return sage_qjs_emit_event(q, "tab_change", payload);
}

int64_t sage_qjs_emit_search(SageQjs *q, const char *query, int64_t regex,
                             int64_t ignore_case) {
  if (!q || !q->ctx) {
    return 1;
  }
  JSContext *ctx = q->ctx;
  JSValue payload = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, payload, "query",
                    JS_NewString(ctx, query ? query : ""));
  JS_SetPropertyStr(ctx, payload, "regex", JS_NewBool(ctx, regex != 0));
  JS_SetPropertyStr(ctx, payload, "ignore_case",
                    JS_NewBool(ctx, ignore_case != 0));
  return sage_qjs_emit_event(q, "search", payload);
}

int64_t sage_qjs_emit_copy(SageQjs *q, int64_t bytes) {
  if (!q || !q->ctx) {
    return 1;
  }
  JSContext *ctx = q->ctx;
  JSValue payload = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, payload, "bytes", JS_NewInt64(ctx, bytes));
  return sage_qjs_emit_event(q, "copy", payload);
}

int64_t sage_qjs_emit_quit(SageQjs *q) {
  if (!q || !q->ctx) {
    return 1;
  }
  return sage_qjs_emit_event(q, "quit", JS_UNDEFINED);
}
