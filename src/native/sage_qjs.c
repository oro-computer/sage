#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/random.h>
#endif
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#include "quickjs.h"

typedef struct SageQjs SageQjs;

typedef struct SageQjsProc {
  pid_t pid;
  int stdout_fd;
  int stderr_fd;
  uint8_t *stdout_buf;
  size_t stdout_len;
  size_t stdout_cap;
  uint8_t *stderr_buf;
  size_t stderr_len;
  size_t stderr_cap;
  size_t max_bytes;
  uint64_t deadline_ns;
  int exited;
  int exit_code;
  int term_signal;
  int timed_out;
  int killed;
  int truncated;
  JSValue resolve_fn;
  JSValue reject_fn;
} SageQjsProc;

typedef struct SageQjsHeaderPair {
  char *name;
  char *value;
} SageQjsHeaderPair;

typedef struct SageQjsFetch {
  uint64_t id;

  pthread_t thread;
  int thread_started;
  atomic_int done;
  atomic_int cancelled;

  // Request.
  char *req_url;
  char *req_method;
  struct curl_slist *req_headers;
  uint8_t *req_body;
  size_t req_body_len;
  uint32_t timeout_ms;
  size_t max_bytes;
  int follow_redirects;

  // Response.
  long status;
  char *status_text;
  char *effective_url;
  SageQjsHeaderPair *resp_headers;
  size_t resp_headers_len;
  size_t resp_headers_cap;
  uint8_t *resp_body;
  size_t resp_body_len;
  size_t resp_body_cap;

  int truncated;
  char *err;

  JSValue resolve_fn;
  JSValue reject_fn;
} SageQjsFetch;

typedef struct SageQjsPlugin {
  SageQjs *host;
  JSRuntime *rt;
  JSContext *ctx;
  JSValue emit_fn;
  JSValue cmd_fn;
  char *module_root;
  SageQjsProc *procs;
  size_t procs_len;
  size_t procs_cap;
  SageQjsFetch **fetches;
  size_t fetches_len;
  size_t fetches_cap;
  char *fs_data_dir;
  char *path;
  uint32_t load_timeout_ms;
  uint32_t event_timeout_ms;
  uint64_t deadline_ns;
  int timed_out;
  int disabled;
} SageQjsPlugin;

typedef struct SageQjsBuiltinModule {
  char *name;
  char *source;
  size_t source_len;
} SageQjsBuiltinModule;

struct SageQjs {
  SageQjsPlugin *plugins;
  size_t plugins_len;
  size_t plugins_cap;

  uint64_t next_fetch_id;

  char **exec_cmds;
  size_t exec_cmds_len;
  size_t exec_cmds_cap;
  size_t exec_cmds_read;

  char **fs_allow_read;
  size_t fs_allow_read_len;
  size_t fs_allow_read_cap;

  int verbose;
  int disabled;
  int had_error;

  uint32_t load_timeout_ms;
  uint32_t event_timeout_ms;
  uint64_t mem_limit_bytes;
  uint64_t stack_limit_bytes;

  char *log_path;
  FILE *log_file;
  int log_stderr;

  char *bootstrap_source;

  SageQjsBuiltinModule *builtin_modules;
  size_t builtin_modules_len;
  size_t builtin_modules_cap;
};

static void sage_qjs_plugin_disable(SageQjsPlugin *p, const char *why);
static int sage_qjs_enqueue_exec_cmd(SageQjs *q, const char *cmd);
static int sage_qjs_fs_allow_read_add(SageQjs *q, const char *path);
static FILE *sage_qjs_log_stream(SageQjs *q);
static int sage_qjs_path_has_prefix(const char *path, const char *prefix);
static int sage_qjs_read_file(const char *path, uint8_t **out_buf,
                              size_t *out_len);

static const char *sage_qjs_plugin_fs_data_dir(SageQjsPlugin *p);
static int sage_qjs_fs_is_allowed_read(SageQjsPlugin *p, const char *real_path);

static char *sage_qjs_module_normalize(JSContext *ctx,
                                       const char *module_base_name,
                                       const char *module_name, void *opaque);
static JSModuleDef *sage_qjs_module_loader(JSContext *ctx,
                                           const char *module_name, void *opaque,
                                           JSValueConst attributes);

static void sage_qjs_proc_free(JSContext *ctx, SageQjsProc *pr);
static void sage_qjs_fetch_free(JSContext *ctx, SageQjsFetch *f);

static pthread_once_t sage_qjs_curl_once = PTHREAD_ONCE_INIT;
static void sage_qjs_curl_global_init_once(void) {
  (void)curl_global_init(CURL_GLOBAL_DEFAULT);
}

static uint64_t sage_qjs_now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static int sage_qjs_read_fd_all_exact(int fd, uint8_t *buf, size_t len) {
  if (fd < 0 || (!buf && len != 0)) {
    return -1;
  }
  size_t off = 0;
  while (off < len) {
    ssize_t r = read(fd, buf + off, len - off);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (r == 0) {
      return -1;
    }
    off += (size_t)r;
  }
  return 0;
}

static int sage_qjs_random_bytes(uint8_t *buf, size_t len) {
  if (!buf && len != 0) {
    return -1;
  }

  size_t off = 0;

#ifdef __linux__
  while (off < len) {
    ssize_t n = getrandom(buf + off, len - off, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == ENOSYS) {
        break;
      }
      return -1;
    }
    if (n == 0) {
      break;
    }
    off += (size_t)n;
  }
  if (off == len) {
    return 0;
  }
#endif

  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  int rc = sage_qjs_read_fd_all_exact(fd, buf + off, len - off);
  close(fd);
  return rc;
}

static char *sage_qjs_realpath_owned(const char *path) {
  if (!path || !*path) {
    return NULL;
  }
  size_t cap = 4096;
#ifdef PATH_MAX
  if (PATH_MAX > 0) {
    cap = (size_t)PATH_MAX;
  }
#endif
  char *out = (char *)malloc(cap);
  if (!out) {
    return NULL;
  }
  if (!realpath(path, out)) {
    free(out);
    return NULL;
  }
  return out;
}

static int sage_qjs_is_sage_module(const char *name) {
  return name && strncmp(name, "sage:", 5) == 0;
}

static const char *sage_qjs_builtin_module_source(const SageQjs *q,
                                                  const char *name,
                                                  size_t *out_len) {
  if (out_len) {
    *out_len = 0;
  }
  if (!q || !name || !*name) {
    return NULL;
  }
  for (size_t i = 0; i < q->builtin_modules_len; i++) {
    SageQjsBuiltinModule *m = &q->builtin_modules[i];
    if (!m->name || !m->source) {
      continue;
    }
    if (strcmp(m->name, name) == 0) {
      if (out_len) {
        *out_len = m->source_len;
      }
      return m->source;
    }
  }
  return NULL;
}

static char *sage_qjs_dirname_owned(const char *path) {
  if (!path || !*path) {
    return NULL;
  }
  const char *slash = strrchr(path, '/');
  size_t n = 0;
  if (slash) {
    n = (size_t)(slash - path);
    // Keep "/" for root paths.
    if (n == 0 && path[0] == '/') {
      n = 1;
    }
  }
  char *out = (char *)malloc(n + 1);
  if (!out) {
    return NULL;
  }
  if (n > 0) {
    memcpy(out, path, n);
  }
  out[n] = '\0';
  return out;
}

static char *sage_qjs_module_normalize(JSContext *ctx,
                                       const char *module_base_name,
                                       const char *module_name,
                                       void *opaque) {
  (void)module_base_name;
  SageQjsPlugin *p = (SageQjsPlugin *)opaque;

  if (!module_name || !*module_name) {
    JS_ThrowReferenceError(ctx, "invalid module specifier");
    return NULL;
  }

  if (sage_qjs_is_sage_module(module_name)) {
    return js_strdup(ctx, module_name);
  }

  // Only allow relative imports for filesystem modules.
  if (module_name[0] != '.') {
    JS_ThrowReferenceError(ctx, "unsupported module specifier '%s'", module_name);
    return NULL;
  }

  if (!module_base_name || !*module_base_name) {
    if (p && p->path) {
      module_base_name = p->path;
    } else {
      JS_ThrowReferenceError(ctx, "missing module base for '%s'", module_name);
      return NULL;
    }
  }

  if (sage_qjs_is_sage_module(module_base_name)) {
    JS_ThrowReferenceError(ctx, "relative import from '%s' is not allowed", module_base_name);
    return NULL;
  }

  if (!p || !p->module_root) {
    JS_ThrowReferenceError(ctx, "plugin module root unavailable");
    return NULL;
  }

  char *base_dir = sage_qjs_dirname_owned(module_base_name);
  if (!base_dir) {
    JS_ThrowInternalError(ctx, "module normalize: dirname failed");
    return NULL;
  }

  size_t need = strlen(base_dir) + 1 + strlen(module_name) + 1;
  if (need > 8192) {
    free(base_dir);
    JS_ThrowRangeError(ctx, "module normalize: path too long");
    return NULL;
  }

  char *joined = (char *)malloc(need);
  if (!joined) {
    free(base_dir);
    JS_ThrowOutOfMemory(ctx);
    return NULL;
  }
  if (base_dir[0]) {
    size_t base_len = strlen(base_dir);
    size_t mod_len = strlen(module_name);
    memcpy(joined, base_dir, base_len);
    joined[base_len] = '/';
    memcpy(joined + base_len + 1, module_name, mod_len);
    joined[base_len + 1 + mod_len] = '\0';
  } else {
    memcpy(joined, module_name, strlen(module_name) + 1);
  }
  free(base_dir);

  char *rp = sage_qjs_realpath_owned(joined);
  free(joined);
  if (!rp) {
    int err = errno;
    JS_ThrowReferenceError(ctx, "could not resolve module '%s' (errno=%d: %s)",
                           module_name, err, strerror(err));
    return NULL;
  }

  if (!sage_qjs_path_has_prefix(rp, p->module_root)) {
    JS_ThrowReferenceError(ctx, "module import escapes plugin root");
    free(rp);
    return NULL;
  }

  char *out = js_strdup(ctx, rp);
  free(rp);
  if (!out) {
    JS_ThrowOutOfMemory(ctx);
    return NULL;
  }
  return out;
}

static JSModuleDef *sage_qjs_module_loader(JSContext *ctx,
                                           const char *module_name,
                                           void *opaque,
                                           JSValueConst attributes) {
  (void)attributes;
  SageQjsPlugin *p = (SageQjsPlugin *)opaque;

  if (!module_name || !*module_name) {
    JS_ThrowReferenceError(ctx, "invalid module name");
    return NULL;
  }

  if (sage_qjs_is_sage_module(module_name)) {
    size_t src_len = 0;
    SageQjs *q = p ? p->host : NULL;
    const char *src = sage_qjs_builtin_module_source(q, module_name, &src_len);
    if (!src) {
      JS_ThrowReferenceError(ctx, "unknown builtin module '%s'", module_name);
      return NULL;
    }
    JSValue func_val =
        JS_Eval(ctx, src, src_len, module_name,
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func_val)) {
      return NULL;
    }
    JSModuleDef *m = JS_VALUE_GET_PTR(func_val);
    JS_FreeValue(ctx, func_val);
    return m;
  }

  if (!p || !p->module_root) {
    JS_ThrowReferenceError(ctx, "plugin module root unavailable");
    return NULL;
  }
  if (!sage_qjs_path_has_prefix(module_name, p->module_root)) {
    JS_ThrowReferenceError(ctx, "module import escapes plugin root");
    return NULL;
  }

  uint8_t *buf = NULL;
  size_t len = 0;
  if (sage_qjs_read_file(module_name, &buf, &len) != 0) {
    JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
    return NULL;
  }

  JSValue func_val = JS_Eval(ctx, (const char *)buf, len, module_name,
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  free(buf);

  if (JS_IsException(func_val)) {
    return NULL;
  }

  // The module is already referenced by the runtime; free the wrapper value.
  JSModuleDef *m = JS_VALUE_GET_PTR(func_val);
  JS_FreeValue(ctx, func_val);
  return m;
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

static void sage_qjs_begin_budget(SageQjsPlugin *p, uint32_t ms) {
  if (!p || p->disabled) {
    return;
  }
  p->timed_out = 0;
  if (ms == 0) {
    p->deadline_ns = 0;
    return;
  }
  uint64_t now = sage_qjs_now_ns();
  if (now == 0) {
    p->deadline_ns = 0;
    return;
  }
  p->deadline_ns = now + ((uint64_t)ms * 1000000ull);
}

static void sage_qjs_end_budget(SageQjsPlugin *p) {
  if (!p) {
    return;
  }
  p->deadline_ns = 0;
}

static int sage_qjs_interrupt_handler(JSRuntime *rt, void *opaque) {
  (void)rt;
  SageQjsPlugin *p = (SageQjsPlugin *)opaque;
  if (!p || p->disabled) {
    return 0;
  }
  if (p->deadline_ns == 0) {
    return 0;
  }
  uint64_t now = sage_qjs_now_ns();
  if (now == 0) {
    return 0;
  }
  if (now >= p->deadline_ns) {
    p->timed_out = 1;
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

static char *sage_qjs_default_plugin_state_root(void) {
  const char *xdg = getenv("XDG_STATE_HOME");
  if (xdg && *xdg) {
    size_t n = strlen(xdg) + strlen("/sage/plugins") + 1;
    char *out = (char *)malloc(n);
    if (!out) {
      return NULL;
    }
    snprintf(out, n, "%s/sage/plugins", xdg);
    return out;
  }

  const char *home = getenv("HOME");
  if (home && *home) {
    size_t n = strlen(home) + strlen("/.local/state/sage/plugins") + 1;
    char *out = (char *)malloc(n);
    if (!out) {
      return NULL;
    }
    snprintf(out, n, "%s/.local/state/sage/plugins", home);
    return out;
  }

  return NULL;
}

static char *sage_qjs_default_plugin_state_root_tmp(void) {
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) {
    tmp = "/tmp";
  }
  size_t n = strlen(tmp) + strlen("/sage/plugins") + 1;
  char *out = (char *)malloc(n);
  if (!out) {
    return NULL;
  }
  snprintf(out, n, "%s/sage/plugins", tmp);
  return out;
}

static char *sage_qjs_sanitize_plugin_id(const char *path) {
  const char *base = path ? path : "";
  const char *slash = strrchr(base, '/');
  if (slash) {
    base = slash + 1;
  }
  const char *bslash = strrchr(base, '\\');
  if (bslash) {
    base = bslash + 1;
  }

  size_t len = strlen(base);
  if (len >= 3 && base[len - 3] == '.' && base[len - 2] == 'j' &&
      base[len - 1] == 's') {
    len -= 3;
  }
  if (len == 0) {
    return strdup("plugin");
  }

  // Keep identifiers short (directory name).
  const size_t MAX_ID = 96;
  if (len > MAX_ID) {
    len = MAX_ID;
  }

  char *out = (char *)malloc(len + 1);
  if (!out) {
    return NULL;
  }
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)base[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
      out[i] = (char)c;
    } else {
      out[i] = '_';
    }
  }
  out[len] = '\0';
  return out;
}

static const char *sage_qjs_plugin_fs_data_dir(SageQjsPlugin *p) {
  if (!p) {
    return NULL;
  }
  if (p->fs_data_dir) {
    return p->fs_data_dir;
  }

  char *root = sage_qjs_default_plugin_state_root();
  if (!root) {
    return NULL;
  }
  char *id = sage_qjs_sanitize_plugin_id(p->path);
  if (!id) {
    free(root);
    return NULL;
  }

  // Ensure state root exists.
  if (sage_qjs_mkdir_p(root, 0700) != 0) {
    char *tmp_root = sage_qjs_default_plugin_state_root_tmp();
    if (!tmp_root || sage_qjs_mkdir_p(tmp_root, 0700) != 0) {
      free(tmp_root);
      free(root);
      free(id);
      return NULL;
    }
    free(root);
    root = tmp_root;
  }

  size_t n = strlen(root) + 1 + strlen(id) + 1;
  char *dir = (char *)malloc(n);
  if (!dir) {
    free(root);
    free(id);
    return NULL;
  }
  snprintf(dir, n, "%s/%s", root, id);

  if (sage_qjs_mkdir_p(dir, 0700) != 0) {
    free(root);
    free(id);
    free(dir);
    return NULL;
  }

  // Canonicalize for prefix checks (realpath resolves symlinks).
  // If this fails, keep the non-canonical path; openat-based data access still works.
  char *rp = sage_qjs_realpath_owned(dir);
  if (rp) {
    free(dir);
    dir = rp;
  }

  p->fs_data_dir = dir;
  free(root);
  free(id);
  return p->fs_data_dir;
}

static int sage_qjs_validate_rel_path(const char *rel) {
  if (!rel || !*rel) {
    return -1;
  }
  if (rel[0] == '/') {
    return -1;
  }
  // Reject backslashes to avoid platform/path surprises.
  if (strchr(rel, '\\')) {
    return -1;
  }

  const char *seg = rel;
  size_t seg_len = 0;
  for (const char *p = rel; ; p++) {
    char c = *p;
    if (c == '/' || c == '\0') {
      if (seg_len == 0) {
        return -1;
      }
      if (seg_len == 1 && seg[0] == '.') {
        return -1;
      }
      if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
        return -1;
      }
      if (c == '\0') {
        break;
      }
      seg = p + 1;
      seg_len = 0;
      continue;
    }
    seg_len++;
  }

  return 0;
}

static char *sage_qjs_join_data_path(SageQjsPlugin *p, const char *rel) {
  if (!p || !rel) {
    return NULL;
  }
  if (sage_qjs_validate_rel_path(rel) != 0) {
    return NULL;
  }

  const char *dir = sage_qjs_plugin_fs_data_dir(p);
  if (!dir) {
    return NULL;
  }

  size_t n = strlen(dir) + 1 + strlen(rel) + 1;
  if (n > 8192) {
    return NULL;
  }
  char *out = (char *)malloc(n);
  if (!out) {
    return NULL;
  }
  snprintf(out, n, "%s/%s", dir, rel);
  return out;
}

static int sage_qjs_path_has_prefix(const char *path, const char *prefix) {
  if (!path || !prefix) {
    return 0;
  }
  size_t n = strlen(prefix);
  if (n == 0) {
    return 0;
  }
  if (strncmp(path, prefix, n) != 0) {
    return 0;
  }
  char c = path[n];
  return (c == '\0' || c == '/');
}

static int sage_qjs_fs_is_allowed_read(SageQjsPlugin *p, const char *real_path) {
  if (!p || !real_path) {
    return 0;
  }

  const char *data_dir = sage_qjs_plugin_fs_data_dir(p);
  if (data_dir && sage_qjs_path_has_prefix(real_path, data_dir)) {
    return 1;
  }

  SageQjs *q = p->host;
  if (!q || !q->fs_allow_read) {
    return 0;
  }

  for (size_t i = 0; i < q->fs_allow_read_len; i++) {
    const char *a = q->fs_allow_read[i];
    if (a && strcmp(a, real_path) == 0) {
      return 1;
    }
  }

  return 0;
}

static int sage_qjs_fs_allow_read_add(SageQjs *q, const char *path) {
  if (!q || !path || !*path) {
    return -1;
  }
  if (q->disabled) {
    return -1;
  }

  const size_t MAX_ALLOW = 4096;
  if (q->fs_allow_read_len >= MAX_ALLOW) {
    q->had_error = 1;
    return -1;
  }

  char *rp = sage_qjs_realpath_owned(path);
  if (!rp) {
    int err = errno;
    FILE *out = sage_qjs_log_stream(q);
    fprintf(out,
            "sage[plugin] fs allow: realpath failed for '%s' (errno=%d: %s)\n",
            path, err, strerror(err));
    fflush(out);
    q->had_error = 1;
    return -1;
  }

  for (size_t i = 0; i < q->fs_allow_read_len; i++) {
    const char *a = q->fs_allow_read[i];
    if (a && strcmp(a, rp) == 0) {
      free(rp);
      return 0;
    }
  }

  if (q->fs_allow_read_len >= q->fs_allow_read_cap) {
    size_t new_cap = q->fs_allow_read_cap ? (q->fs_allow_read_cap * 2) : 32;
    if (new_cap > MAX_ALLOW) {
      new_cap = MAX_ALLOW;
    }
    char **new_ptr = (char **)realloc(q->fs_allow_read, new_cap * sizeof(char *));
    if (!new_ptr) {
      free(rp);
      q->had_error = 1;
      return -1;
    }
    q->fs_allow_read = new_ptr;
    q->fs_allow_read_cap = new_cap;
  }

  q->fs_allow_read[q->fs_allow_read_len++] = rp;
  return 0;
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
  if (!q) {
    return stderr;
  }
  if (q->log_stderr) {
    return stderr;
  }

  FILE *f = sage_qjs_log_file(q);
  if (f) {
    return f;
  }

  /*
   * If we can't open a log file, avoid corrupting the TUI by default.
   * Users can opt into stderr via SAGE_PLUGIN_LOG_STDERR=1.
   */
  if (!q->log_file) {
    q->log_file = fopen("/dev/null", "a");
    if (q->log_file) {
      setvbuf(q->log_file, NULL, _IOLBF, 0);
    }
  }
  return q->log_file ? q->log_file : stderr;
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
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  SageQjs *q = p ? p->host : NULL;
  FILE *out = sage_qjs_log_stream(q);
  if (q) {
    q->had_error = 1;
  }
  JSValue exc = JS_GetException(ctx);

  if (p && p->path) {
    fprintf(out, "sage[plugin:%s] exception: ", p->path);
  } else {
    fputs("sage[plugin] exception: ", out);
  }
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

static void sage_qjs_dump_exception(SageQjsPlugin *p) {
  if (!p || !p->ctx) {
    return;
  }
  SageQjs *q = p->host;
  int verbose = q ? q->verbose : 0;
  sage_qjs_dump_exception_ctx(p->ctx, verbose);
}

static void sage_qjs_drain_jobs(SageQjsPlugin *p) {
  if (!p || !p->rt) {
    return;
  }
  if (p->disabled) {
    return;
  }
  JSContext *job_ctx = NULL;
  int iters = 0;
  while (iters < 1024) {
    int rc = JS_ExecutePendingJob(p->rt, &job_ctx);
    if (rc <= 0) {
      if (rc < 0 && job_ctx) {
        SageQjsPlugin *jp = (SageQjsPlugin *)JS_GetContextOpaque(job_ctx);
        SageQjs *q = jp ? jp->host : NULL;
        int verbose = q ? q->verbose : 0;
        sage_qjs_dump_exception_ctx(job_ctx, verbose);
      }
      break;
    }
    iters++;
  }
  if (p->timed_out) {
    sage_qjs_plugin_disable(p, "timeout while draining jobs");
  }
}

static int sage_qjs_ascii_lower(int c) {
  if (c >= 'A' && c <= 'Z') {
    return c + 32;
  }
  return c;
}

static int sage_qjs_streq_ci(const char *a, const char *b) {
  if (!a || !b) {
    return 0;
  }
  while (*a && *b) {
    int ca = sage_qjs_ascii_lower((unsigned char)*a++);
    int cb = sage_qjs_ascii_lower((unsigned char)*b++);
    if (ca != cb) {
      return 0;
    }
  }
  return *a == '\0' && *b == '\0';
}

static int sage_qjs_console_level_from_str(const char *s, int *ok) {
  if (ok) {
    *ok = 0;
  }
  if (!s || !*s) {
    return 0;
  }
  if (sage_qjs_streq_ci(s, "silent") || sage_qjs_streq_ci(s, "none") ||
      sage_qjs_streq_ci(s, "off")) {
    if (ok) {
      *ok = 1;
    }
    return -1;
  }
  if (sage_qjs_streq_ci(s, "error")) {
    if (ok) {
      *ok = 1;
    }
    return 0;
  }
  if (sage_qjs_streq_ci(s, "warn") || sage_qjs_streq_ci(s, "warning")) {
    if (ok) {
      *ok = 1;
    }
    return 1;
  }
  if (sage_qjs_streq_ci(s, "info") || sage_qjs_streq_ci(s, "log")) {
    if (ok) {
      *ok = 1;
    }
    return 2;
  }
  if (sage_qjs_streq_ci(s, "verbose")) {
    if (ok) {
      *ok = 1;
    }
    return 3;
  }
  if (sage_qjs_streq_ci(s, "debug")) {
    if (ok) {
      *ok = 1;
    }
    return 4;
  }

  errno = 0;
  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (errno == 0 && end && end != s && *end == '\0') {
    if (v < -1) {
      v = -1;
    }
    if (v > 4) {
      v = 4;
    }
    if (ok) {
      *ok = 1;
    }
    return (int)v;
  }

  return 0;
}

static int sage_qjs_console_threshold(SageQjs *q) {
  const char *s = getenv("SAGE_CONSOLE_LEVEL");
  if (!s || !*s) {
    return (q && q->verbose) ? 4 : 1;
  }
  int ok = 0;
  int v = sage_qjs_console_level_from_str(s, &ok);
  if (!ok) {
    return (q && q->verbose) ? 4 : 1;
  }
  return v;
}

static const char *sage_qjs_console_level_name(int lvl) {
  switch (lvl) {
    case 0: return "error";
    case 1: return "warn";
    case 2: return "info";
    case 3: return "verbose";
    case 4: return "debug";
    default: return "log";
  }
}

static JSValue js_sage_console(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  (void)this_val;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  SageQjs *q = p ? p->host : NULL;
  if (!q || q->disabled) {
    return JS_UNDEFINED;
  }
  if (argc < 1) {
    return JS_UNDEFINED;
  }

  const char *lvl_s = JS_ToCString(ctx, argv[0]);
  if (!lvl_s) {
    return JS_EXCEPTION;
  }
  int ok = 0;
  int lvl = sage_qjs_console_level_from_str(lvl_s, &ok);
  JS_FreeCString(ctx, lvl_s);
  if (!ok) {
    lvl = 2;
  }
  int th = sage_qjs_console_threshold(q);
  if (th < 0 || lvl > th) {
    return JS_UNDEFINED;
  }

  FILE *out = sage_qjs_log_stream(q);
  if (p && p->path) {
    fprintf(out, "sage[console:%s:%s]", sage_qjs_console_level_name(lvl), p->path);
  } else {
    fprintf(out, "sage[console:%s]", sage_qjs_console_level_name(lvl));
  }
  for (int i = 1; i < argc; i++) {
    fputc(' ', out);
    sage_qjs_print_value_to(ctx, argv[i], out);
  }
  fputc('\n', out);
  fflush(out);
  return JS_UNDEFINED;
}

static JSValue js_sage_log(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  (void)this_val;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  SageQjs *q = p ? p->host : NULL;
  if (!q || !q->verbose) {
    return JS_UNDEFINED;
  }

  FILE *out = sage_qjs_log_stream(q);
  if (p && p->path) {
    fprintf(out, "sage[js:%s]", p->path);
  } else {
    fputs("sage[js]", out);
  }
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
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  SageQjs *q = p ? p->host : NULL;
  int verbose = q ? q->verbose : 0;
  FILE *out = sage_qjs_log_stream(q);
  if (q) {
    q->had_error = 1;
  }

  JSValue exc = (argc >= 1) ? JS_DupValue(ctx, argv[0]) : JS_GetException(ctx);
  if (p && p->path) {
    fprintf(out, "sage[plugin:%s] error: ", p->path);
  } else {
    fputs("sage[plugin] error: ", out);
  }
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

static JSValue js_sage_exec(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  (void)this_val;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  SageQjs *q = p ? p->host : NULL;
  if (!q || q->disabled) {
    return JS_NewInt32(ctx, 1);
  }

  if (argc < 1) {
    return JS_NewInt32(ctx, 1);
  }

  const char *s = JS_ToCString(ctx, argv[0]);
  if (!s) {
    return JS_NewInt32(ctx, 1);
  }

  // Trim leading whitespace and an optional ':' prefix for convenience.
  const char *cmd = s;
  while (*cmd == ' ' || *cmd == '\t' || *cmd == '\r' || *cmd == '\n') {
    cmd++;
  }
  if (*cmd == ':') {
    cmd++;
    while (*cmd == ' ' || *cmd == '\t' || *cmd == '\r' || *cmd == '\n') {
      cmd++;
    }
  }

  size_t n = strlen(cmd);
  if (n == 0) {
    JS_FreeCString(ctx, s);
    return JS_NewInt32(ctx, 0);
  }

  // Keep this bounded to avoid untrusted plugins consuming lots of memory.
  if (n > 4096) {
    JS_FreeCString(ctx, s);
    q->had_error = 1;
    return JS_NewInt32(ctx, 1);
  }

  int rc = sage_qjs_enqueue_exec_cmd(q, cmd);
  JS_FreeCString(ctx, s);
  return JS_NewInt32(ctx, rc == 0 ? 0 : 1);
}

static JSValue js_sage_env_get(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) {
    return JS_UNDEFINED;
  }
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) {
    return JS_EXCEPTION;
  }
  const char *val = getenv(name);
  JS_FreeCString(ctx, name);
  if (!val) {
    return JS_UNDEFINED;
  }
  return JS_NewString(ctx, val);
}

static JSValue js_sage_env_set(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  (void)this_val;
  if (argc < 2) {
    return JS_NewInt32(ctx, 1);
  }
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) {
    return JS_NewInt32(ctx, 1);
  }
  const char *value = JS_ToCString(ctx, argv[1]);
  if (!value) {
    JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, 1);
  }
  int overwrite = 1;
  if (argc >= 3) {
    int b = JS_ToBool(ctx, argv[2]);
    if (b >= 0) {
      overwrite = b ? 1 : 0;
    }
  }
  int rc = setenv(name, value, overwrite);
  JS_FreeCString(ctx, name);
  JS_FreeCString(ctx, value);
  return JS_NewInt32(ctx, rc == 0 ? 0 : 1);
}

static JSValue js_sage_env_unset(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) {
    return JS_NewInt32(ctx, 1);
  }
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) {
    return JS_NewInt32(ctx, 1);
  }
  int rc = unsetenv(name);
  JS_FreeCString(ctx, name);
  return JS_NewInt32(ctx, rc == 0 ? 0 : 1);
}

static JSValue js_sage_app_version(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  return JS_NewString(ctx, "0.1.0");
}

static JSValue js_sage_qjs_version(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  return JS_NewString(ctx, JS_GetVersion());
}

static JSValue js_sage_crypto_random_bytes(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "__sage_crypto_random_bytes(len)");
  }

  int64_t len64 = 0;
  if (JS_ToInt64(ctx, &len64, argv[0]) != 0) {
    return JS_EXCEPTION;
  }
  if (len64 < 0) {
    return JS_ThrowRangeError(ctx, "__sage_crypto_random_bytes: len must be >= 0");
  }

  // Keep this bounded; plugins are untrusted.
  if (len64 > (1024 * 1024)) {
    return JS_ThrowRangeError(ctx, "__sage_crypto_random_bytes: len too large");
  }
  size_t len = (size_t)len64;
  if (len == 0) {
    return JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);
  }

  uint8_t *buf = (uint8_t *)malloc(len);
  if (!buf) {
    return JS_ThrowOutOfMemory(ctx);
  }
  if (sage_qjs_random_bytes(buf, len) != 0) {
    free(buf);
    return JS_ThrowInternalError(ctx, "__sage_crypto_random_bytes: failed");
  }

  // Use `ArrayBufferCopy` so bytes count against the QuickJS memory limit.
  JSValue ab = JS_NewArrayBufferCopy(ctx, buf, len);
  free(buf);
  return ab;
}

static JSValue js_sage_performance_now(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  uint64_t ns = sage_qjs_now_ns();
  double ms = (double)ns / 1000000.0;
  return JS_NewFloat64(ctx, ms);
}

static JSValue js_sage_process_pid(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  return JS_NewInt64(ctx, (int64_t)getpid());
}

static JSValue js_sage_process_ppid(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  return JS_NewInt64(ctx, (int64_t)getppid());
}

static JSValue js_sage_process_cwd(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  char buf[4096];
  if (!getcwd(buf, sizeof(buf))) {
    int err = errno;
    return JS_ThrowInternalError(ctx, "process.cwd: getcwd failed (errno=%d: %s)",
                                err, strerror(err));
  }
  return JS_NewString(ctx, buf);
}

static int sage_qjs_fd_set_nonblock(int fd) {
  if (fd < 0) {
    return -1;
  }
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return -1;
  }
  return 0;
}

static int sage_qjs_proc_buf_append(uint8_t **buf, size_t *len, size_t *cap,
                                    const uint8_t *src, size_t n,
                                    size_t max_total, int *truncated) {
  if (!buf || !len || !cap || !src) {
    return -1;
  }
  if (*len >= max_total) {
    if (truncated) {
      *truncated = 1;
    }
    return 0;
  }
  size_t avail = max_total - *len;
  if (n > avail) {
    n = avail;
    if (truncated) {
      *truncated = 1;
    }
  }
  if (n == 0) {
    return 0;
  }
  size_t need = *len + n;
  if (need > *cap) {
    size_t new_cap = *cap ? (*cap * 2) : 8192;
    if (new_cap < need) {
      new_cap = need;
    }
    if (new_cap > max_total) {
      new_cap = max_total;
    }
    uint8_t *new_buf = (uint8_t *)realloc(*buf, new_cap);
    if (!new_buf) {
      return -1;
    }
    *buf = new_buf;
    *cap = new_cap;
  }
  memcpy(*buf + *len, src, n);
  *len += n;
  return 0;
}

static void sage_qjs_proc_read_fd(SageQjsProc *pr, int which_fd) {
  if (!pr) {
    return;
  }
  int *fdp = which_fd == 0 ? &pr->stdout_fd : &pr->stderr_fd;
  uint8_t **bufp = which_fd == 0 ? &pr->stdout_buf : &pr->stderr_buf;
  size_t *lenp = which_fd == 0 ? &pr->stdout_len : &pr->stderr_len;
  size_t *capp = which_fd == 0 ? &pr->stdout_cap : &pr->stderr_cap;
  int fd = *fdp;
  if (fd < 0) {
    return;
  }

  uint8_t tmp[4096];
  while (true) {
    ssize_t r = read(fd, tmp, sizeof(tmp));
    if (r > 0) {
      if (sage_qjs_proc_buf_append(bufp, lenp, capp, tmp, (size_t)r, pr->max_bytes,
                                   &pr->truncated) != 0) {
        pr->truncated = 1;
        break;
      }
      continue;
    }
    if (r == 0) {
      close(fd);
      *fdp = -1;
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    close(fd);
    *fdp = -1;
    break;
  }
}

static int sage_qjs_plugin_procs_push(SageQjsPlugin *p, const SageQjsProc *src) {
  if (!p || !src) {
    return -1;
  }
  if (p->procs_len >= p->procs_cap) {
    size_t new_cap = p->procs_cap ? (p->procs_cap * 2) : 4;
    SageQjsProc *new_ptr =
        (SageQjsProc *)realloc(p->procs, new_cap * sizeof(SageQjsProc));
    if (!new_ptr) {
      return -1;
    }
    p->procs = new_ptr;
    p->procs_cap = new_cap;
  }
  p->procs[p->procs_len++] = *src;
  return 0;
}

static int sage_qjs_plugin_fetches_push(SageQjsPlugin *p, SageQjsFetch *f) {
  if (!p || !f) {
    return -1;
  }
  if (p->fetches_len >= p->fetches_cap) {
    size_t new_cap = p->fetches_cap ? (p->fetches_cap * 2) : 4;
    SageQjsFetch **new_ptr =
        (SageQjsFetch **)realloc(p->fetches, new_cap * sizeof(SageQjsFetch *));
    if (!new_ptr) {
      return -1;
    }
    p->fetches = new_ptr;
    p->fetches_cap = new_cap;
  }
  p->fetches[p->fetches_len++] = f;
  return 0;
}

static void sage_qjs_fetch_headers_clear(SageQjsFetch *f) {
  if (!f || !f->resp_headers) {
    return;
  }
  for (size_t i = 0; i < f->resp_headers_len; i++) {
    free(f->resp_headers[i].name);
    free(f->resp_headers[i].value);
    f->resp_headers[i].name = NULL;
    f->resp_headers[i].value = NULL;
  }
  free(f->resp_headers);
  f->resp_headers = NULL;
  f->resp_headers_len = 0;
  f->resp_headers_cap = 0;
}

static int sage_qjs_fetch_headers_push(SageQjsFetch *f, const char *name,
                                       const char *value) {
  if (!f || !name || !*name || !value) {
    return -1;
  }
  if (f->resp_headers_len >= f->resp_headers_cap) {
    size_t new_cap = f->resp_headers_cap ? (f->resp_headers_cap * 2) : 16;
    SageQjsHeaderPair *new_ptr = (SageQjsHeaderPair *)realloc(
        f->resp_headers, new_cap * sizeof(SageQjsHeaderPair));
    if (!new_ptr) {
      return -1;
    }
    f->resp_headers = new_ptr;
    f->resp_headers_cap = new_cap;
  }
  char *n = strdup(name);
  if (!n) {
    return -1;
  }
  char *v = strdup(value);
  if (!v) {
    free(n);
    return -1;
  }
  SageQjsHeaderPair *p = &f->resp_headers[f->resp_headers_len++];
  p->name = n;
  p->value = v;
  return 0;
}

static char *sage_qjs_trim_ws_inplace(char *s) {
  if (!s) {
    return s;
  }
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
    s++;
  }
  size_t n = strlen(s);
  while (n > 0) {
    char c = s[n - 1];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      s[n - 1] = '\0';
      n--;
      continue;
    }
    break;
  }
  return s;
}

static size_t sage_qjs_curl_write_cb(char *ptr, size_t size, size_t nmemb,
                                     void *userdata) {
  SageQjsFetch *f = (SageQjsFetch *)userdata;
  if (!f || !ptr) {
    return 0;
  }
  if (atomic_load_explicit(&f->cancelled, memory_order_relaxed)) {
    return 0;
  }
  size_t n = size * nmemb;
  if (n == 0) {
    return 0;
  }
  if (sage_qjs_proc_buf_append(&f->resp_body, &f->resp_body_len, &f->resp_body_cap,
                               (const uint8_t *)ptr, n, f->max_bytes,
                               &f->truncated) != 0) {
    f->truncated = 1;
    return 0;
  }
  if (f->truncated) {
    return 0;
  }
  return n;
}

static size_t sage_qjs_curl_header_cb(char *buffer, size_t size, size_t nmemb,
                                      void *userdata) {
  SageQjsFetch *f = (SageQjsFetch *)userdata;
  if (!f || !buffer) {
    return 0;
  }
  if (atomic_load_explicit(&f->cancelled, memory_order_relaxed)) {
    return 0;
  }
  size_t n = size * nmemb;
  if (n == 0) {
    return 0;
  }
  char *line = (char *)malloc(n + 1);
  if (!line) {
    return 0;
  }
  memcpy(line, buffer, n);
  line[n] = '\0';

  // Strip CRLF.
  while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) {
    line[n - 1] = '\0';
    n--;
  }
  if (n == 0) {
    free(line);
    return size * nmemb;
  }

  // New response block (redirects): clear previously captured headers.
  if (strncmp(line, "HTTP/", 5) == 0) {
    sage_qjs_fetch_headers_clear(f);
    free(f->status_text);
    f->status_text = NULL;
    char *sp1 = strchr(line, ' ');
    if (sp1) {
      char *sp2 = strchr(sp1 + 1, ' ');
      if (sp2 && sp2[1]) {
        char *reason = sage_qjs_trim_ws_inplace(sp2 + 1);
        if (reason && *reason) {
          f->status_text = strdup(reason);
        }
      }
    }
    free(line);
    return size * nmemb;
  }

  char *colon = strchr(line, ':');
  if (!colon) {
    free(line);
    return size * nmemb;
  }
  *colon = '\0';
  char *name = sage_qjs_trim_ws_inplace(line);
  char *value = sage_qjs_trim_ws_inplace(colon + 1);
  if (name && *name) {
    (void)sage_qjs_fetch_headers_push(f, name, value ? value : "");
  }
  free(line);
  return size * nmemb;
}

static int sage_qjs_curl_xferinfo(void *clientp, curl_off_t dltotal,
                                  curl_off_t dlnow, curl_off_t ultotal,
                                  curl_off_t ulnow) {
  (void)dltotal;
  (void)dlnow;
  (void)ultotal;
  (void)ulnow;
  SageQjsFetch *f = (SageQjsFetch *)clientp;
  if (!f) {
    return 0;
  }
  return atomic_load_explicit(&f->cancelled, memory_order_relaxed) ? 1 : 0;
}

static void *sage_qjs_fetch_thread_main(void *opaque) {
  SageQjsFetch *f = (SageQjsFetch *)opaque;
  if (!f) {
    return NULL;
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    f->err = strdup("fetch: curl init failed");
    atomic_store_explicit(&f->done, 1, memory_order_release);
    return NULL;
  }

  char errbuf[CURL_ERROR_SIZE];
  errbuf[0] = 0;

  curl_easy_setopt(curl, CURLOPT_URL, f->req_url ? f->req_url : "");
  const char *method = f->req_method ? f->req_method : "GET";
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(curl, CURLOPT_NOBODY, strcmp(method, "HEAD") == 0 ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, f->req_headers);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, f->follow_redirects ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)f->timeout_ms);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sage_qjs_curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, sage_qjs_curl_header_cb);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, f);

  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, sage_qjs_curl_xferinfo);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, f);

  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

  if (f->req_body && f->req_body_len > 0) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, f->req_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)f->req_body_len);
  } else {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, NULL);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
  }

  CURLcode rc = curl_easy_perform(curl);
  if (atomic_load_explicit(&f->cancelled, memory_order_relaxed)) {
    f->err = strdup("fetch: aborted");
  } else if (f->truncated) {
    f->err = strdup("fetch: response too large");
  } else if (rc != CURLE_OK) {
    const char *m = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    if (m && *m) {
      size_t need = strlen(m) + 16;
      char *e = (char *)malloc(need);
      if (e) {
        snprintf(e, need, "fetch: %s", m);
        f->err = e;
      } else {
        f->err = strdup("fetch: failed");
      }
    } else {
      f->err = strdup("fetch: failed");
    }
  }

  long status = 0;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  f->status = status;

  char *eff = NULL;
  (void)curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
  if (eff && *eff) {
    f->effective_url = strdup(eff);
  }

  curl_easy_cleanup(curl);

  atomic_store_explicit(&f->done, 1, memory_order_release);
  return NULL;
}

static char sage_qjs_ascii_upper(char c) {
  if (c >= 'a' && c <= 'z') {
    return (char)(c - 32);
  }
  return c;
}

static char *sage_qjs_strdup_upper_ascii_token(const char *s, size_t len) {
  if (!s || len == 0 || len > 32) {
    return NULL;
  }
  char *out = (char *)malloc(len + 1);
  if (!out) {
    return NULL;
  }
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c <= 32 || c >= 127) { // no whitespace / CTLs / non-ASCII
      free(out);
      return NULL;
    }
    out[i] = sage_qjs_ascii_upper((char)c);
  }
  out[len] = '\0';
  return out;
}

static int sage_qjs_fetch_add_header(SageQjsFetch *f, const char *name,
                                     const char *value) {
  if (!f || !name || !*name || !value) {
    return -1;
  }
  size_t nlen = strlen(name);
  size_t vlen = strlen(value);
  if (nlen == 0 || nlen > 1024 || vlen > 8192) {
    return -1;
  }
  size_t need = nlen + 2 + vlen + 1;
  char *line = (char *)malloc(need);
  if (!line) {
    return -1;
  }
  snprintf(line, need, "%s: %s", name, value);
  struct curl_slist *next = curl_slist_append(f->req_headers, line);
  free(line);
  if (!next) {
    return -1;
  }
  f->req_headers = next;
  return 0;
}

static int sage_qjs_fetch_parse_headers(JSContext *ctx, SageQjsFetch *f,
                                        JSValueConst headers_val) {
  if (!ctx || !f) {
    return -1;
  }
  if (JS_IsUndefined(headers_val) || JS_IsNull(headers_val)) {
    return 0;
  }

  if (JS_IsArray(headers_val)) {
    uint32_t len = 0;
    JSValue len_v = JS_GetPropertyStr(ctx, headers_val, "length");
    if (!JS_IsException(len_v)) {
      (void)JS_ToUint32(ctx, &len, len_v);
    }
    JS_FreeValue(ctx, len_v);

    for (uint32_t i = 0; i < len; i++) {
      JSValue pair = JS_GetPropertyUint32(ctx, headers_val, i);
      if (JS_IsException(pair)) {
        return -1;
      }

      JSValue name_v = JS_GetPropertyUint32(ctx, pair, 0);
      JSValue value_v = JS_GetPropertyUint32(ctx, pair, 1);
      if (JS_IsException(name_v) || JS_IsException(value_v)) {
        JS_FreeValue(ctx, name_v);
        JS_FreeValue(ctx, value_v);
        JS_FreeValue(ctx, pair);
        return -1;
      }

      const char *name = JS_ToCString(ctx, name_v);
      const char *value = JS_ToCString(ctx, value_v);
      if (name && *name && value) {
        (void)sage_qjs_fetch_add_header(f, name, value);
      }
      if (name) JS_FreeCString(ctx, name);
      if (value) JS_FreeCString(ctx, value);
      JS_FreeValue(ctx, name_v);
      JS_FreeValue(ctx, value_v);
      JS_FreeValue(ctx, pair);
    }
    return 0;
  }

  if (JS_IsObject(headers_val)) {
    JSPropertyEnum *tab = NULL;
    uint32_t tab_len = 0;
    int rc = JS_GetOwnPropertyNames(ctx, &tab, &tab_len, headers_val,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);
    if (rc != 0) {
      return -1;
    }
    for (uint32_t i = 0; i < tab_len; i++) {
      JSAtom atom = tab[i].atom;
      const char *name = JS_AtomToCString(ctx, atom);
      JSValue value_v = JS_GetProperty(ctx, headers_val, atom);
      const char *value = JS_ToCString(ctx, value_v);
      if (name && *name && value) {
        (void)sage_qjs_fetch_add_header(f, name, value);
      }
      if (name) JS_FreeCString(ctx, name);
      if (value) JS_FreeCString(ctx, value);
      JS_FreeValue(ctx, value_v);
    }
    JS_FreePropertyEnum(ctx, tab, tab_len);
    return 0;
  }

  return -1;
}

static int sage_qjs_fetch_parse_body(JSContext *ctx, SageQjsFetch *f,
                                     JSValueConst body_val) {
  if (!ctx || !f) {
    return -1;
  }
  if (JS_IsUndefined(body_val) || JS_IsNull(body_val)) {
    return 0;
  }

  if (JS_IsString(body_val)) {
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, body_val);
    if (!s) {
      return -1;
    }
    if (n > (16 * 1024 * 1024)) {
      JS_FreeCString(ctx, s);
      return -1;
    }
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) {
      JS_FreeCString(ctx, s);
      return -1;
    }
    memcpy(buf, s, n);
    JS_FreeCString(ctx, s);
    f->req_body = buf;
    f->req_body_len = n;
    return 0;
  }

  size_t ab_len = 0;
  uint8_t *ab = JS_GetArrayBuffer(ctx, &ab_len, body_val);
  if (ab) {
    if (ab_len > (16 * 1024 * 1024)) {
      return -1;
    }
    uint8_t *buf = (uint8_t *)malloc(ab_len);
    if (!buf) {
      return -1;
    }
    memcpy(buf, ab, ab_len);
    f->req_body = buf;
    f->req_body_len = ab_len;
    return 0;
  }

  if (JS_GetTypedArrayType(body_val) >= 0) {
    size_t byte_offset = 0;
    size_t byte_length = 0;
    size_t bpe = 0;
    JSValue buf_obj =
        JS_GetTypedArrayBuffer(ctx, body_val, &byte_offset, &byte_length, &bpe);
    (void)bpe;
    if (JS_IsException(buf_obj)) {
      return -1;
    }
    size_t backing_len = 0;
    uint8_t *backing = JS_GetArrayBuffer(ctx, &backing_len, buf_obj);
    if (!backing || byte_offset + byte_length > backing_len) {
      JS_FreeValue(ctx, buf_obj);
      return -1;
    }
    if (byte_length > (16 * 1024 * 1024)) {
      JS_FreeValue(ctx, buf_obj);
      return -1;
    }
    uint8_t *buf = (uint8_t *)malloc(byte_length);
    if (!buf) {
      JS_FreeValue(ctx, buf_obj);
      return -1;
    }
    memcpy(buf, backing + byte_offset, byte_length);
    JS_FreeValue(ctx, buf_obj);
    f->req_body = buf;
    f->req_body_len = byte_length;
    return 0;
  }

  return -1;
}

static JSValue js_sage_fetch(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  (void)this_val;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  if (!p || !p->host || p->disabled) {
    return JS_ThrowInternalError(ctx, "__sage_fetch: plugins disabled");
  }
  SageQjs *q = p->host;
  pthread_once(&sage_qjs_curl_once, sage_qjs_curl_global_init_once);

  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "__sage_fetch(url, [opts])");
  }

  size_t url_len = 0;
  const char *url = JS_ToCStringLen(ctx, &url_len, argv[0]);
  if (!url) {
    return JS_EXCEPTION;
  }
  if (url_len == 0 || url_len > 8192) {
    JS_FreeCString(ctx, url);
    return JS_ThrowRangeError(ctx, "__sage_fetch: invalid url");
  }

  SageQjsFetch *f = (SageQjsFetch *)calloc(1, sizeof(*f));
  if (!f) {
    JS_FreeCString(ctx, url);
    return JS_ThrowOutOfMemory(ctx);
  }

  f->id = q->next_fetch_id++;
  atomic_init(&f->done, 0);
  atomic_init(&f->cancelled, 0);
  f->resolve_fn = JS_UNDEFINED;
  f->reject_fn = JS_UNDEFINED;
  f->thread_started = 0;
  f->req_url = strdup(url);
  JS_FreeCString(ctx, url);
  if (!f->req_url) {
    free(f);
    return JS_ThrowOutOfMemory(ctx);
  }
  f->req_method = strdup("GET");
  if (!f->req_method) {
    free(f->req_url);
    free(f);
    return JS_ThrowOutOfMemory(ctx);
  }
  f->req_headers = NULL;
  f->req_body = NULL;
  f->req_body_len = 0;
  f->timeout_ms = 30000;
  f->max_bytes = 16 * 1024 * 1024;
  f->follow_redirects = 1;

  if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
    if (!JS_IsObject(argv[1])) {
      sage_qjs_fetch_free(ctx, f);
      return JS_ThrowTypeError(ctx, "__sage_fetch: opts must be an object");
    }

    JSValue method_v = JS_GetPropertyStr(ctx, argv[1], "method");
    if (JS_IsException(method_v)) {
      sage_qjs_fetch_free(ctx, f);
      return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(method_v) && !JS_IsNull(method_v)) {
      size_t mlen = 0;
      const char *m = JS_ToCStringLen(ctx, &mlen, method_v);
      if (!m) {
        JS_FreeValue(ctx, method_v);
        sage_qjs_fetch_free(ctx, f);
        return JS_EXCEPTION;
      }
      char *upper = sage_qjs_strdup_upper_ascii_token(m, mlen);
      JS_FreeCString(ctx, m);
      if (!upper) {
        JS_FreeValue(ctx, method_v);
        sage_qjs_fetch_free(ctx, f);
        return JS_ThrowTypeError(ctx, "__sage_fetch: invalid method");
      }
      free(f->req_method);
      f->req_method = upper;
    }
    JS_FreeValue(ctx, method_v);

    JSValue follow_v = JS_GetPropertyStr(ctx, argv[1], "followRedirects");
    if (!JS_IsException(follow_v) && !JS_IsUndefined(follow_v) &&
        !JS_IsNull(follow_v)) {
      f->follow_redirects = JS_ToBool(ctx, follow_v) ? 1 : 0;
    }
    JS_FreeValue(ctx, follow_v);

    JSValue timeout_v = JS_GetPropertyStr(ctx, argv[1], "timeoutMs");
    if (!JS_IsException(timeout_v) && !JS_IsUndefined(timeout_v) &&
        !JS_IsNull(timeout_v)) {
      int64_t t = 0;
      if (JS_ToInt64(ctx, &t, timeout_v) == 0) {
        if (t < 0) t = 0;
        if (t > 10 * 60 * 1000) t = 10 * 60 * 1000;
        f->timeout_ms = (uint32_t)t;
      }
    }
    JS_FreeValue(ctx, timeout_v);

    JSValue max_v = JS_GetPropertyStr(ctx, argv[1], "maxBytes");
    if (!JS_IsException(max_v) && !JS_IsUndefined(max_v) && !JS_IsNull(max_v)) {
      int64_t mb = 0;
      if (JS_ToInt64(ctx, &mb, max_v) == 0) {
        if (mb <= 0) mb = 1;
        if (mb > (64 * 1024 * 1024)) mb = (64 * 1024 * 1024);
        f->max_bytes = (size_t)mb;
      }
    }
    JS_FreeValue(ctx, max_v);

    JSValue headers_v = JS_GetPropertyStr(ctx, argv[1], "headers");
    if (JS_IsException(headers_v)) {
      sage_qjs_fetch_free(ctx, f);
      return JS_EXCEPTION;
    }
    if (sage_qjs_fetch_parse_headers(ctx, f, headers_v) != 0) {
      JS_FreeValue(ctx, headers_v);
      sage_qjs_fetch_free(ctx, f);
      return JS_ThrowTypeError(ctx, "__sage_fetch: invalid headers");
    }
    JS_FreeValue(ctx, headers_v);

    JSValue body_v = JS_GetPropertyStr(ctx, argv[1], "body");
    if (JS_IsException(body_v)) {
      sage_qjs_fetch_free(ctx, f);
      return JS_EXCEPTION;
    }
    if (sage_qjs_fetch_parse_body(ctx, f, body_v) != 0) {
      JS_FreeValue(ctx, body_v);
      sage_qjs_fetch_free(ctx, f);
      return JS_ThrowTypeError(ctx, "__sage_fetch: invalid body");
    }
    JS_FreeValue(ctx, body_v);
  }

  if (f->req_body && f->req_body_len > 0 &&
      (strcmp(f->req_method, "GET") == 0 || strcmp(f->req_method, "HEAD") == 0)) {
    sage_qjs_fetch_free(ctx, f);
    return JS_ThrowTypeError(ctx, "__sage_fetch: GET/HEAD cannot have a body");
  }

  JSValue resolving_funcs[2];
  JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
  if (JS_IsException(promise)) {
    sage_qjs_fetch_free(ctx, f);
    return promise;
  }
  f->resolve_fn = resolving_funcs[0];
  f->reject_fn = resolving_funcs[1];

  if (sage_qjs_plugin_fetches_push(p, f) != 0) {
    JSValue e = JS_NewPlainError(ctx, "fetch: out of memory");
    JS_Call(ctx, f->reject_fn, JS_UNDEFINED, 1, (JSValueConst *)&e);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, f->resolve_fn);
    JS_FreeValue(ctx, f->reject_fn);
    f->resolve_fn = JS_UNDEFINED;
    f->reject_fn = JS_UNDEFINED;
    sage_qjs_fetch_free(NULL, f);
    return promise;
  }

  int thr_rc = pthread_create(&f->thread, NULL, sage_qjs_fetch_thread_main, f);
  if (thr_rc != 0) {
    // Remove last pushed fetch.
    if (p->fetches_len > 0 && p->fetches[p->fetches_len - 1] == f) {
      p->fetches_len--;
    }
    JSValue e = JS_NewPlainError(ctx, "fetch: thread create failed");
    JS_Call(ctx, f->reject_fn, JS_UNDEFINED, 1, (JSValueConst *)&e);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, f->resolve_fn);
    JS_FreeValue(ctx, f->reject_fn);
    f->resolve_fn = JS_UNDEFINED;
    f->reject_fn = JS_UNDEFINED;
    sage_qjs_fetch_free(NULL, f);
    return promise;
  }
  f->thread_started = 1;

  JS_DefinePropertyValueStr(ctx, promise, "sageFetchId",
                            JS_NewInt64(ctx, (int64_t)f->id),
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  return promise;
}

static JSValue js_sage_fetch_abort(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)this_val;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  if (!p || p->disabled) {
    return JS_NewBool(ctx, false);
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "__sage_fetch_abort(id)");
  }
  int64_t id_i64 = 0;
  if (JS_ToInt64(ctx, &id_i64, argv[0]) != 0 || id_i64 <= 0) {
    return JS_NewBool(ctx, false);
  }
  uint64_t id = (uint64_t)id_i64;
  for (size_t i = 0; i < p->fetches_len; i++) {
    SageQjsFetch *f = p->fetches[i];
    if (f && f->id == id) {
      atomic_store_explicit(&f->cancelled, 1, memory_order_relaxed);
      return JS_NewBool(ctx, true);
    }
  }
  return JS_NewBool(ctx, false);
}

static JSValue js_sage_process_exec(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  (void)this_val;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  if (!p || !p->host || p->disabled) {
    return JS_ThrowInternalError(ctx, "process.exec: plugins disabled");
  }

  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "process.exec(cmd, [timeoutMs], [maxBytes])");
  }

  size_t cmd_len = 0;
  const char *cmd = JS_ToCStringLen(ctx, &cmd_len, argv[0]);
  if (!cmd) {
    return JS_EXCEPTION;
  }
  if (cmd_len == 0 || cmd_len > 8192) {
    JS_FreeCString(ctx, cmd);
    return JS_ThrowRangeError(ctx, "process.exec: invalid cmd");
  }

  int64_t timeout_ms = 30000;
  if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
    (void)JS_ToInt64(ctx, &timeout_ms, argv[1]);
  }
  if (timeout_ms < 0) {
    timeout_ms = 0;
  }
  if (timeout_ms > 10 * 60 * 1000) {
    timeout_ms = 10 * 60 * 1000;
  }

  int64_t max_bytes_i64 = 1024 * 1024;
  if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
    (void)JS_ToInt64(ctx, &max_bytes_i64, argv[2]);
  }
  if (max_bytes_i64 <= 0) {
    max_bytes_i64 = 1;
  }
  if (max_bytes_i64 > (16 * 1024 * 1024)) {
    max_bytes_i64 = 16 * 1024 * 1024;
  }
  size_t max_bytes = (size_t)max_bytes_i64;

  JSValue resolving_funcs[2];
  JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
  if (JS_IsException(promise)) {
    JS_FreeCString(ctx, cmd);
    return promise;
  }

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (out_pipe[1] >= 0) close(out_pipe[1]);
    if (err_pipe[0] >= 0) close(err_pipe[0]);
    if (err_pipe[1] >= 0) close(err_pipe[1]);
    JSValue e = JS_NewPlainError(ctx, "process.exec: pipe failed");
    JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, (JSValueConst *)&e);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    JS_FreeCString(ctx, cmd);
    return promise;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    JSValue e = JS_NewPlainError(ctx, "process.exec: fork failed");
    JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, (JSValueConst *)&e);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    JS_FreeCString(ctx, cmd);
    return promise;
  }

  if (pid == 0) {
    // Child.
    (void)dup2(out_pipe[1], STDOUT_FILENO);
    (void)dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }

  // Parent.
  close(out_pipe[1]);
  close(err_pipe[1]);
  (void)fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
  (void)fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);
  (void)sage_qjs_fd_set_nonblock(out_pipe[0]);
  (void)sage_qjs_fd_set_nonblock(err_pipe[0]);

  SageQjsProc pr;
  memset(&pr, 0, sizeof(pr));
  pr.pid = pid;
  pr.stdout_fd = out_pipe[0];
  pr.stderr_fd = err_pipe[0];
  pr.stdout_buf = NULL;
  pr.stdout_len = 0;
  pr.stdout_cap = 0;
  pr.stderr_buf = NULL;
  pr.stderr_len = 0;
  pr.stderr_cap = 0;
  pr.max_bytes = max_bytes;
  pr.deadline_ns = timeout_ms ? (sage_qjs_now_ns() + ((uint64_t)timeout_ms * 1000000ull)) : 0;
  pr.exited = 0;
  pr.exit_code = 0;
  pr.term_signal = 0;
  pr.timed_out = 0;
  pr.killed = 0;
  pr.truncated = 0;
  pr.resolve_fn = resolving_funcs[0];
  pr.reject_fn = resolving_funcs[1];

  if (sage_qjs_plugin_procs_push(p, &pr) != 0) {
    (void)kill(pid, SIGKILL);
    close(pr.stdout_fd);
    close(pr.stderr_fd);
    JSValue e = JS_NewPlainError(ctx, "process.exec: out of memory");
    JS_Call(ctx, pr.reject_fn, JS_UNDEFINED, 1, (JSValueConst *)&e);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, pr.resolve_fn);
    JS_FreeValue(ctx, pr.reject_fn);
  }

  JS_FreeCString(ctx, cmd);
  return promise;
}

static void sage_qjs_proc_complete(SageQjsPlugin *p, SageQjsProc *pr) {
  if (!p || !p->ctx || !pr) {
    return;
  }
  JSContext *ctx = p->ctx;

  // Build result payload.
  JSValue stdout_s =
      JS_NewStringLen(ctx, pr->stdout_buf ? (const char *)pr->stdout_buf : "",
                      pr->stdout_len);
  JSValue stderr_s =
      JS_NewStringLen(ctx, pr->stderr_buf ? (const char *)pr->stderr_buf : "",
                      pr->stderr_len);

  JSValue code_v = JS_NewInt64(ctx, (int64_t)pr->exit_code);
  JSValue timed_v = JS_NewBool(ctx, pr->timed_out != 0);
  JSValue trunc_v = JS_NewBool(ctx, pr->truncated != 0);
  JSValue sig_v = JS_NewInt64(ctx, (int64_t)pr->term_signal);

  int is_err = (pr->timed_out != 0) || (pr->truncated != 0);
  JSValue cb = is_err ? pr->reject_fn : pr->resolve_fn;
  if (JS_IsUndefined(cb)) {
    // Nothing we can do; just cleanup.
    JS_FreeValue(ctx, stdout_s);
    JS_FreeValue(ctx, stderr_s);
    JS_FreeValue(ctx, code_v);
    JS_FreeValue(ctx, timed_v);
    JS_FreeValue(ctx, trunc_v);
    JS_FreeValue(ctx, sig_v);
    return;
  }

  JSValue arg0;
  if (is_err) {
    const char *msg =
        (pr->timed_out != 0) ? "process.exec: timed out" : "process.exec: output truncated";
    arg0 = JS_NewPlainError(ctx, "%s", msg);
    JS_SetPropertyStr(ctx, arg0, "code", JS_DupValue(ctx, code_v));
    JS_SetPropertyStr(ctx, arg0, "stdout", JS_DupValue(ctx, stdout_s));
    JS_SetPropertyStr(ctx, arg0, "stderr", JS_DupValue(ctx, stderr_s));
    JS_SetPropertyStr(ctx, arg0, "timedOut", JS_DupValue(ctx, timed_v));
    JS_SetPropertyStr(ctx, arg0, "truncated", JS_DupValue(ctx, trunc_v));
    JS_SetPropertyStr(ctx, arg0, "signal", JS_DupValue(ctx, sig_v));
  } else {
    arg0 = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, arg0, "code", JS_DupValue(ctx, code_v));
    JS_SetPropertyStr(ctx, arg0, "stdout", JS_DupValue(ctx, stdout_s));
    JS_SetPropertyStr(ctx, arg0, "stderr", JS_DupValue(ctx, stderr_s));
    JS_SetPropertyStr(ctx, arg0, "timedOut", JS_DupValue(ctx, timed_v));
    JS_SetPropertyStr(ctx, arg0, "truncated", JS_DupValue(ctx, trunc_v));
    JS_SetPropertyStr(ctx, arg0, "signal", JS_DupValue(ctx, sig_v));
  }

  // We duplicated these into arg0; free the originals before calling into JS.
  JS_FreeValue(ctx, stdout_s);
  JS_FreeValue(ctx, stderr_s);
  JS_FreeValue(ctx, code_v);
  JS_FreeValue(ctx, timed_v);
  JS_FreeValue(ctx, trunc_v);
  JS_FreeValue(ctx, sig_v);

  JSValue argv0[1] = {arg0};
  sage_qjs_begin_budget(p, p->event_timeout_ms);
  JSValue call_rc = JS_Call(ctx, cb, JS_UNDEFINED, 1, argv0);
  if (p->timed_out) {
    if (JS_IsException(call_rc)) {
      sage_qjs_dump_exception(p);
    }
    JS_FreeValue(ctx, call_rc);
    JS_FreeValue(ctx, arg0);
    sage_qjs_end_budget(p);
    sage_qjs_plugin_disable(p, "timeout while resolving promise");
    return;
  }
  if (JS_IsException(call_rc)) {
    sage_qjs_dump_exception(p);
    JS_FreeValue(ctx, call_rc);
    JS_FreeValue(ctx, arg0);
    sage_qjs_end_budget(p);
    sage_qjs_plugin_disable(p, "promise resolve/reject threw");
    return;
  }
  JS_FreeValue(ctx, call_rc);
  JS_FreeValue(ctx, arg0);

  sage_qjs_drain_jobs(p);
  sage_qjs_end_budget(p);
}

static void sage_qjs_fetch_complete(SageQjsPlugin *p, SageQjsFetch *f) {
  if (!p || !p->ctx || !f) {
    return;
  }
  JSContext *ctx = p->ctx;

  int is_err = (f->err != NULL);
  JSValue cb = is_err ? f->reject_fn : f->resolve_fn;
  if (JS_IsUndefined(cb)) {
    return;
  }

  JSValue arg0;
  if (is_err) {
    arg0 = JS_NewPlainError(ctx, "%s", f->err ? f->err : "fetch: failed");
    if (atomic_load_explicit(&f->cancelled, memory_order_relaxed)) {
      JS_SetPropertyStr(ctx, arg0, "name", JS_NewString(ctx, "AbortError"));
    }
    JS_SetPropertyStr(ctx, arg0, "status", JS_NewInt64(ctx, (int64_t)f->status));
    const char *u = f->effective_url ? f->effective_url : (f->req_url ? f->req_url : "");
    JS_SetPropertyStr(ctx, arg0, "url", JS_NewString(ctx, u));
    JS_SetPropertyStr(ctx, arg0, "truncated", JS_NewBool(ctx, f->truncated != 0));
  } else {
    arg0 = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, arg0, "status", JS_NewInt64(ctx, (int64_t)f->status));
    JS_SetPropertyStr(ctx, arg0, "statusText",
                      JS_NewString(ctx, f->status_text ? f->status_text : ""));
    const char *u = f->effective_url ? f->effective_url : (f->req_url ? f->req_url : "");
    JS_SetPropertyStr(ctx, arg0, "url", JS_NewString(ctx, u));

    JSValue headers = JS_NewArray(ctx);
    for (size_t i = 0; i < f->resp_headers_len; i++) {
      SageQjsHeaderPair *hp = &f->resp_headers[i];
      JSValue pair = JS_NewArray(ctx);
      JS_SetPropertyUint32(ctx, pair, 0,
                           JS_NewString(ctx, hp->name ? hp->name : ""));
      JS_SetPropertyUint32(ctx, pair, 1,
                           JS_NewString(ctx, hp->value ? hp->value : ""));
      JS_SetPropertyUint32(ctx, headers, (uint32_t)i, pair);
    }
    JS_SetPropertyStr(ctx, arg0, "headers", headers);

    JSValue body = JS_UNDEFINED;
    if (f->resp_body && f->resp_body_len > 0) {
      // Copy into JS-managed memory so the QuickJS memory limit applies.
      body = JS_NewArrayBufferCopy(ctx, f->resp_body, f->resp_body_len);
      if (JS_IsException(body)) {
        // Best-effort: clear the exception and return an empty body.
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
        body = JS_UNDEFINED;
        f->truncated = 1;
      }
      free(f->resp_body);
      f->resp_body = NULL;
      f->resp_body_len = 0;
      f->resp_body_cap = 0;
    } else {
      // Prefer an actual ArrayBuffer for consistency, but fall back to
      // undefined on allocation failure.
      JSValue ab0 = JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);
      if (!JS_IsException(ab0)) {
        body = ab0;
      } else {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
        body = JS_UNDEFINED;
      }
    }
    JS_SetPropertyStr(ctx, arg0, "body", body);
    JS_SetPropertyStr(ctx, arg0, "truncated", JS_NewBool(ctx, f->truncated != 0));
  }

  JSValue argv0[1] = {arg0};
  sage_qjs_begin_budget(p, p->event_timeout_ms);
  JSValue call_rc = JS_Call(ctx, cb, JS_UNDEFINED, 1, argv0);
  if (p->timed_out) {
    if (JS_IsException(call_rc)) {
      sage_qjs_dump_exception(p);
    }
    JS_FreeValue(ctx, call_rc);
    JS_FreeValue(ctx, arg0);
    sage_qjs_end_budget(p);
    sage_qjs_plugin_disable(p, "timeout while resolving promise");
    return;
  }
  if (JS_IsException(call_rc)) {
    sage_qjs_dump_exception(p);
    JS_FreeValue(ctx, call_rc);
    JS_FreeValue(ctx, arg0);
    sage_qjs_end_budget(p);
    sage_qjs_plugin_disable(p, "promise resolve/reject threw");
    return;
  }
  JS_FreeValue(ctx, call_rc);
  JS_FreeValue(ctx, arg0);

  sage_qjs_drain_jobs(p);
  sage_qjs_end_budget(p);
}

static void sage_qjs_plugin_poll_fetches(SageQjsPlugin *p) {
  if (!p || !p->ctx || p->disabled) {
    return;
  }
  if (!p->fetches || p->fetches_len == 0) {
    return;
  }

  size_t w = 0;
  for (size_t i = 0; i < p->fetches_len; i++) {
    SageQjsFetch *f = p->fetches[i];
    if (!f) {
      continue;
    }
    if (!atomic_load_explicit(&f->done, memory_order_acquire)) {
      p->fetches[w++] = f;
      continue;
    }

    if (f->thread_started) {
      (void)pthread_join(f->thread, NULL);
      f->thread_started = 0;
    }

    sage_qjs_fetch_complete(p, f);
    if (p->disabled) {
      return;
    }
    sage_qjs_fetch_free(p->ctx, f);
  }
  p->fetches_len = w;
}

static void sage_qjs_plugin_poll_procs(SageQjsPlugin *p) {
  if (!p || !p->ctx || p->disabled) {
    return;
  }
  if (!p->procs || p->procs_len == 0) {
    return;
  }

  uint64_t now = sage_qjs_now_ns();
  size_t w = 0;
  for (size_t i = 0; i < p->procs_len; i++) {
    SageQjsProc pr = p->procs[i];

    sage_qjs_proc_read_fd(&pr, 0);
    sage_qjs_proc_read_fd(&pr, 1);

    if (!pr.exited && !pr.killed) {
      if ((pr.deadline_ns != 0) && (now != 0) && (now > pr.deadline_ns)) {
        (void)kill(pr.pid, SIGKILL);
        pr.killed = 1;
        pr.timed_out = 1;
      }
      if (pr.truncated) {
        (void)kill(pr.pid, SIGKILL);
        pr.killed = 1;
      }
    }

    if (!pr.exited) {
      int st = 0;
      pid_t wpid = waitpid(pr.pid, &st, WNOHANG);
      if (wpid == pr.pid) {
        pr.exited = 1;
        if (WIFEXITED(st)) {
          pr.exit_code = WEXITSTATUS(st);
        } else if (WIFSIGNALED(st)) {
          int sig = WTERMSIG(st);
          pr.term_signal = sig;
          pr.exit_code = 128 + sig;
        } else {
          pr.exit_code = 1;
        }
      }
    }

    if (pr.exited) {
      // Drain any remaining output after exit.
      sage_qjs_proc_read_fd(&pr, 0);
      sage_qjs_proc_read_fd(&pr, 1);
    }

    if (pr.exited && pr.stdout_fd < 0 && pr.stderr_fd < 0) {
      sage_qjs_proc_complete(p, &pr);
      if (p->disabled) {
        // Plugin disabled while resolving (plugin_close cleared procs).
        return;
      }
      sage_qjs_proc_free(p->ctx, &pr);
      continue;
    }

    p->procs[w++] = pr;
  }
  p->procs_len = w;
}

static size_t sage_qjs_fs_max_bytes(JSContext *ctx, int argc,
                                    JSValueConst *argv, int idx, size_t def) {
  const size_t HARD_MAX = 4 * 1024 * 1024;
  if (argc <= idx || JS_IsUndefined(argv[idx]) || JS_IsNull(argv[idx])) {
    return def;
  }
  int64_t v = 0;
  if (JS_ToInt64(ctx, &v, argv[idx]) != 0) {
    return 0;
  }
  if (v <= 0) {
    return 0;
  }
  size_t n = (size_t)v;
  if (n > HARD_MAX) {
    n = HARD_MAX;
  }
  return n;
}

static int sage_qjs_read_fd_bounded(int fd, size_t max_bytes, uint8_t **out_buf,
                                    size_t *out_len) {
  if (!out_buf || !out_len || fd < 0 || max_bytes == 0) {
    return -1;
  }
  *out_buf = NULL;
  *out_len = 0;

  struct stat st;
  if (fstat(fd, &st) != 0) {
    return -1;
  }
  // Only allow regular files to avoid blocking on pipes/devices.
  if (!S_ISREG(st.st_mode)) {
    return -1;
  }
  if (st.st_size > 0 && (uint64_t)st.st_size > (uint64_t)max_bytes) {
    return -2;
  }

  size_t cap = 0;
  if (st.st_size > 0) {
    cap = (size_t)st.st_size;
  }
  if (cap == 0) {
    cap = max_bytes < 8192 ? max_bytes : 8192;
  }
  if (cap == 0) {
    return -2;
  }

  uint8_t *buf = (uint8_t *)malloc(cap);
  if (!buf) {
    return -1;
  }

  size_t len = 0;
  while (len < max_bytes) {
    size_t avail = cap - len;
    if (avail == 0) {
      if (cap >= max_bytes) {
        break;
      }
      size_t new_cap = cap * 2;
      if (new_cap > max_bytes) {
        new_cap = max_bytes;
      }
      if (new_cap <= cap) {
        break;
      }
      uint8_t *new_buf = (uint8_t *)realloc(buf, new_cap);
      if (!new_buf) {
        free(buf);
        return -1;
      }
      buf = new_buf;
      cap = new_cap;
      avail = cap - len;
      if (avail == 0) {
        break;
      }
    }

    ssize_t r = read(fd, buf + len, avail);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      free(buf);
      return -1;
    }
    if (r == 0) {
      break;
    }
    len += (size_t)r;
  }

  // If we hit the cap, check for more data.
  if (len >= max_bytes) {
    uint8_t tmp = 0;
    ssize_t r2 = read(fd, &tmp, 1);
    if (r2 < 0 && errno == EINTR) {
      do {
        r2 = read(fd, &tmp, 1);
      } while (r2 < 0 && errno == EINTR);
    }
    if (r2 > 0) {
      free(buf);
      return -2;
    }
  }

  *out_buf = buf;
  *out_len = len;
  return 0;
}

static int sage_qjs_write_fd_all(int fd, const uint8_t *buf, size_t len) {
  if (fd < 0) {
    return -1;
  }
  size_t off = 0;
  while (off < len) {
    ssize_t w = write(fd, buf + off, len - off);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (w == 0) {
      return -1;
    }
    off += (size_t)w;
  }
  return 0;
}

static int sage_qjs_open_data_fd(SageQjsPlugin *p, const char *rel, int flags,
                                 mode_t mode, int create_dirs) {
  if (!p || !rel) {
    return -1;
  }
  if (sage_qjs_validate_rel_path(rel) != 0) {
    return -1;
  }

  const char *data_dir = sage_qjs_plugin_fs_data_dir(p);
  if (!data_dir) {
    return -1;
  }

  int base_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#ifdef O_NOFOLLOW
  base_flags |= O_NOFOLLOW;
#endif
  int dirfd = open(data_dir, base_flags);
  if (dirfd < 0) {
    return -1;
  }

  char *tmp = strdup(rel);
  if (!tmp) {
    close(dirfd);
    return -1;
  }

  char *save = NULL;
  char *seg = strtok_r(tmp, "/", &save);
  while (seg) {
    char *next = strtok_r(NULL, "/", &save);
    if (next) {
      int dflags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#ifdef O_NOFOLLOW
      dflags |= O_NOFOLLOW;
#endif
      int nextfd = openat(dirfd, seg, dflags);
      if (nextfd < 0 && create_dirs && errno == ENOENT) {
        if (mkdirat(dirfd, seg, 0700) != 0 && errno != EEXIST) {
          free(tmp);
          close(dirfd);
          return -1;
        }
        nextfd = openat(dirfd, seg, dflags);
      }
      if (nextfd < 0) {
        free(tmp);
        close(dirfd);
        return -1;
      }
      close(dirfd);
      dirfd = nextfd;
      seg = next;
      continue;
    }

    int oflags = flags | O_CLOEXEC;
#ifdef O_NOFOLLOW
    oflags |= O_NOFOLLOW;
#endif
    int fd = openat(dirfd, seg, oflags, mode);
    free(tmp);
    close(dirfd);
    return fd;
  }

  free(tmp);
  close(dirfd);
  return -1;
}

static JSValue js_sage_fs_data_dir(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  const char *dir = sage_qjs_plugin_fs_data_dir(p);
  if (!dir) {
    return JS_ThrowInternalError(ctx, "sage:fs: no data dir");
  }
  return JS_NewString(ctx, dir);
}

static JSValue js_sage_fs_exists(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  (void)this_val;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  if (!p || argc < 1) {
    return JS_FALSE;
  }

  const char *s = JS_ToCString(ctx, argv[0]);
  if (!s) {
    return JS_FALSE;
  }

  char *rp = sage_qjs_realpath_owned(s);
  JS_FreeCString(ctx, s);
  if (!rp) {
    return JS_FALSE;
  }

  int allowed = sage_qjs_fs_is_allowed_read(p, rp);
  free(rp);
  return allowed ? JS_TRUE : JS_FALSE;
}

static JSValue js_sage_fs_read_text(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)this_val;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  if (!p || argc < 1) {
    return JS_ThrowTypeError(ctx, "sage:fs.readText(path, [maxBytes])");
  }

  size_t max_bytes = sage_qjs_fs_max_bytes(ctx, argc, argv, 1, 256 * 1024);
  if (max_bytes == 0) {
    return JS_ThrowRangeError(ctx, "sage:fs.readText: invalid maxBytes");
  }

  const char *s = JS_ToCString(ctx, argv[0]);
  if (!s) {
    return JS_EXCEPTION;
  }

  char *rp = sage_qjs_realpath_owned(s);
  JS_FreeCString(ctx, s);
  if (!rp) {
    int err = errno;
    return JS_ThrowInternalError(ctx, "sage:fs.readText: realpath failed (errno=%d: %s)", err,
                                strerror(err));
  }

  if (!sage_qjs_fs_is_allowed_read(p, rp)) {
    free(rp);
    return JS_ThrowInternalError(ctx, "sage:fs.readText: access denied");
  }

  int fd = open(rp, O_RDONLY | O_CLOEXEC);
  free(rp);
  if (fd < 0) {
    return JS_ThrowInternalError(ctx, "sage:fs.readText: open failed");
  }

  uint8_t *buf = NULL;
  size_t len = 0;
  int rc = sage_qjs_read_fd_bounded(fd, max_bytes, &buf, &len);
  close(fd);

  if (rc == -2) {
    free(buf);
    return JS_ThrowRangeError(ctx, "sage:fs.readText: file too large");
  }
  if (rc != 0) {
    free(buf);
    return JS_ThrowInternalError(ctx, "sage:fs.readText: read failed");
  }

  JSValue out = JS_NewStringLen(ctx, (const char *)buf, len);
  free(buf);
  return out;
}

static JSValue js_sage_fs_read_bytes(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  (void)this_val;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  if (!p || argc < 1) {
    return JS_ThrowTypeError(ctx, "sage:fs.readBytes(path, [maxBytes])");
  }

  size_t max_bytes = sage_qjs_fs_max_bytes(ctx, argc, argv, 1, 256 * 1024);
  if (max_bytes == 0) {
    return JS_ThrowRangeError(ctx, "sage:fs.readBytes: invalid maxBytes");
  }

  const char *s = JS_ToCString(ctx, argv[0]);
  if (!s) {
    return JS_EXCEPTION;
  }

  char *rp = sage_qjs_realpath_owned(s);
  JS_FreeCString(ctx, s);
  if (!rp) {
    int err = errno;
    return JS_ThrowInternalError(ctx, "sage:fs.readBytes: realpath failed (errno=%d: %s)", err,
                                strerror(err));
  }

  if (!sage_qjs_fs_is_allowed_read(p, rp)) {
    free(rp);
    return JS_ThrowInternalError(ctx, "sage:fs.readBytes: access denied");
  }

  int fd = open(rp, O_RDONLY | O_CLOEXEC);
  free(rp);
  if (fd < 0) {
    return JS_ThrowInternalError(ctx, "sage:fs.readBytes: open failed");
  }

  uint8_t *buf = NULL;
  size_t len = 0;
  int rc = sage_qjs_read_fd_bounded(fd, max_bytes, &buf, &len);
  close(fd);

  if (rc == -2) {
    free(buf);
    return JS_ThrowRangeError(ctx, "sage:fs.readBytes: file too large");
  }
  if (rc != 0) {
    free(buf);
    return JS_ThrowInternalError(ctx, "sage:fs.readBytes: read failed");
  }

  JSValue out = JS_NewArrayBufferCopy(ctx, buf, len);
  free(buf);
  return out;
}

static JSValue js_sage_fs_read_data_text(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  (void)this_val;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  if (!p || argc < 1) {
    return JS_ThrowTypeError(ctx, "sage:fs.readDataText(name, [maxBytes])");
  }

  size_t max_bytes = sage_qjs_fs_max_bytes(ctx, argc, argv, 1, 256 * 1024);
  if (max_bytes == 0) {
    return JS_ThrowRangeError(ctx, "sage:fs.readDataText: invalid maxBytes");
  }

  const char *rel = JS_ToCString(ctx, argv[0]);
  if (!rel) {
    return JS_EXCEPTION;
  }

  int fd = sage_qjs_open_data_fd(p, rel, O_RDONLY, 0600, 0);
  JS_FreeCString(ctx, rel);
  if (fd < 0) {
    return JS_ThrowInternalError(ctx, "sage:fs.readDataText: open failed");
  }

  uint8_t *buf = NULL;
  size_t len = 0;
  int rc = sage_qjs_read_fd_bounded(fd, max_bytes, &buf, &len);
  close(fd);

  if (rc == -2) {
    free(buf);
    return JS_ThrowRangeError(ctx, "sage:fs.readDataText: file too large");
  }
  if (rc != 0) {
    free(buf);
    return JS_ThrowInternalError(ctx, "sage:fs.readDataText: read failed");
  }

  JSValue out = JS_NewStringLen(ctx, (const char *)buf, len);
  free(buf);
  return out;
}

static JSValue js_sage_fs_read_data_bytes(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  (void)this_val;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  if (!p || argc < 1) {
    return JS_ThrowTypeError(ctx, "sage:fs.readDataBytes(name, [maxBytes])");
  }

  size_t max_bytes = sage_qjs_fs_max_bytes(ctx, argc, argv, 1, 256 * 1024);
  if (max_bytes == 0) {
    return JS_ThrowRangeError(ctx, "sage:fs.readDataBytes: invalid maxBytes");
  }

  const char *rel = JS_ToCString(ctx, argv[0]);
  if (!rel) {
    return JS_EXCEPTION;
  }

  int fd = sage_qjs_open_data_fd(p, rel, O_RDONLY, 0600, 0);
  JS_FreeCString(ctx, rel);
  if (fd < 0) {
    return JS_ThrowInternalError(ctx, "sage:fs.readDataBytes: open failed");
  }

  uint8_t *buf = NULL;
  size_t len = 0;
  int rc = sage_qjs_read_fd_bounded(fd, max_bytes, &buf, &len);
  close(fd);

  if (rc == -2) {
    free(buf);
    return JS_ThrowRangeError(ctx, "sage:fs.readDataBytes: file too large");
  }
  if (rc != 0) {
    free(buf);
    return JS_ThrowInternalError(ctx, "sage:fs.readDataBytes: read failed");
  }

  JSValue out = JS_NewArrayBufferCopy(ctx, buf, len);
  free(buf);
  return out;
}

static JSValue js_sage_fs_write_data_text(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  (void)this_val;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  if (!p || argc < 2) {
    return JS_NewInt32(ctx, 1);
  }

  const char *rel = JS_ToCString(ctx, argv[0]);
  if (!rel) {
    return JS_NewInt32(ctx, 1);
  }

  size_t n = 0;
  const char *text = JS_ToCStringLen(ctx, &n, argv[1]);
  if (!text) {
    JS_FreeCString(ctx, rel);
    return JS_NewInt32(ctx, 1);
  }

  const size_t WRITE_MAX = 4 * 1024 * 1024;
  if (n > WRITE_MAX) {
    JS_FreeCString(ctx, rel);
    JS_FreeCString(ctx, text);
    return JS_NewInt32(ctx, 1);
  }

  int append = 0;
  if (argc >= 3 && JS_ToBool(ctx, argv[2])) {
    append = 1;
  }

  int flags = O_WRONLY | O_CREAT;
  if (append) {
    flags |= O_APPEND;
  } else {
    flags |= O_TRUNC;
  }
  int fd = sage_qjs_open_data_fd(p, rel, flags, 0600, 1);
  JS_FreeCString(ctx, rel);
  if (fd < 0) {
    JS_FreeCString(ctx, text);
    return JS_NewInt32(ctx, 1);
  }

  int rc = sage_qjs_write_fd_all(fd, (const uint8_t *)text, n);
  close(fd);
  JS_FreeCString(ctx, text);
  return JS_NewInt32(ctx, rc == 0 ? 0 : 1);
}

static JSValue js_sage_fs_write_data_bytes(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  (void)this_val;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  if (!p || argc < 2) {
    return JS_NewInt32(ctx, 1);
  }

  const char *rel = JS_ToCString(ctx, argv[0]);
  if (!rel) {
    return JS_NewInt32(ctx, 1);
  }

  size_t n = 0;
  uint8_t *bytes = NULL;
  JSValue buf_obj = JS_UNDEFINED;

  if (JS_IsArrayBuffer(argv[1])) {
    bytes = JS_GetArrayBuffer(ctx, &n, argv[1]);
  } else {
    int t = JS_GetTypedArrayType(argv[1]);
    if (t == -1) {
      JS_FreeCString(ctx, rel);
      return JS_NewInt32(ctx, 1);
    }
    size_t off = 0, bpe = 0;
    buf_obj = JS_GetTypedArrayBuffer(ctx, argv[1], &off, &n, &bpe);
    if (JS_IsException(buf_obj)) {
      JS_FreeCString(ctx, rel);
      return JS_EXCEPTION;
    }
    size_t cap = 0;
    uint8_t *base = JS_GetArrayBuffer(ctx, &cap, buf_obj);
    if (base && off <= cap && n <= (cap - off)) {
      bytes = base + off;
    } else {
      bytes = NULL;
    }
  }

  const size_t WRITE_MAX = 4 * 1024 * 1024;
  if (!bytes || n > WRITE_MAX) {
    JS_FreeCString(ctx, rel);
    if (!JS_IsUndefined(buf_obj) && !JS_IsException(buf_obj)) {
      JS_FreeValue(ctx, buf_obj);
    }
    return JS_NewInt32(ctx, 1);
  }

  int append = 0;
  if (argc >= 3 && JS_ToBool(ctx, argv[2])) {
    append = 1;
  }

  int flags = O_WRONLY | O_CREAT;
  if (append) {
    flags |= O_APPEND;
  } else {
    flags |= O_TRUNC;
  }
  int fd = sage_qjs_open_data_fd(p, rel, flags, 0600, 1);
  JS_FreeCString(ctx, rel);
  if (fd < 0) {
    if (!JS_IsUndefined(buf_obj) && !JS_IsException(buf_obj)) {
      JS_FreeValue(ctx, buf_obj);
    }
    return JS_NewInt32(ctx, 1);
  }

  int rc = sage_qjs_write_fd_all(fd, bytes, n);
  close(fd);
  if (!JS_IsUndefined(buf_obj) && !JS_IsException(buf_obj)) {
    JS_FreeValue(ctx, buf_obj);
  }
  return JS_NewInt32(ctx, rc == 0 ? 0 : 1);
}

static JSValue js_sage_fs_list_data(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  SageQjsPlugin *p = (SageQjsPlugin *)JS_GetContextOpaque(ctx);
  const char *dir = sage_qjs_plugin_fs_data_dir(p);
  if (!dir) {
    return JS_ThrowInternalError(ctx, "sage:fs.listData: no data dir");
  }

  DIR *d = opendir(dir);
  if (!d) {
    return JS_ThrowInternalError(ctx, "sage:fs.listData: opendir failed");
  }

  JSValue arr = JS_NewArray(ctx);
  uint32_t idx = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    const char *name = ent->d_name;
    if (!name || !*name) {
      continue;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, name));
  }
  closedir(d);
  return arr;
}

static int sage_qjs_define_host_api(SageQjsPlugin *p) {
  if (!p || !p->ctx) {
    return -1;
  }
  JSContext *ctx = p->ctx;
  JSValue global = JS_GetGlobalObject(ctx);

  JS_SetPropertyStr(ctx, global, "__sage_console",
                    JS_NewCFunction(ctx, js_sage_console, "__sage_console", 1));
  JS_SetPropertyStr(ctx, global, "__sage_log",
                    JS_NewCFunction(ctx, js_sage_log, "__sage_log", 1));
  JS_SetPropertyStr(ctx, global, "__sage_report_exception",
                    JS_NewCFunction(ctx, js_sage_report_exception,
                                    "__sage_report_exception", 1));
  JS_SetPropertyStr(ctx, global, "__sage_exec",
                    JS_NewCFunction(ctx, js_sage_exec, "__sage_exec", 1));
  JS_SetPropertyStr(ctx, global, "__sage_env_get",
                    JS_NewCFunction(ctx, js_sage_env_get, "__sage_env_get", 1));
  JS_SetPropertyStr(ctx, global, "__sage_env_set",
                    JS_NewCFunction(ctx, js_sage_env_set, "__sage_env_set", 3));
  JS_SetPropertyStr(ctx, global, "__sage_env_unset",
                    JS_NewCFunction(ctx, js_sage_env_unset, "__sage_env_unset",
                                    1));
  JS_SetPropertyStr(ctx, global, "__sage_app_version",
                    JS_NewCFunction(ctx, js_sage_app_version,
                                    "__sage_app_version", 0));
  JS_SetPropertyStr(ctx, global, "__sage_qjs_version",
                    JS_NewCFunction(ctx, js_sage_qjs_version, "__sage_qjs_version",
                                    0));
  JS_SetPropertyStr(ctx, global, "__sage_crypto_random_bytes",
                    JS_NewCFunction(ctx, js_sage_crypto_random_bytes,
                                    "__sage_crypto_random_bytes", 1));
  JS_SetPropertyStr(ctx, global, "__sage_performance_now",
                    JS_NewCFunction(ctx, js_sage_performance_now,
                                    "__sage_performance_now", 0));
  JS_SetPropertyStr(ctx, global, "__sage_process_pid",
                    JS_NewCFunction(ctx, js_sage_process_pid,
                                    "__sage_process_pid", 0));
  JS_SetPropertyStr(ctx, global, "__sage_process_ppid",
                    JS_NewCFunction(ctx, js_sage_process_ppid,
                                    "__sage_process_ppid", 0));
  JS_SetPropertyStr(ctx, global, "__sage_process_cwd",
                    JS_NewCFunction(ctx, js_sage_process_cwd,
                                    "__sage_process_cwd", 0));
  JS_SetPropertyStr(ctx, global, "__sage_process_exec",
                    JS_NewCFunction(ctx, js_sage_process_exec,
                                    "__sage_process_exec", 3));
  JS_SetPropertyStr(ctx, global, "__sage_fetch",
                    JS_NewCFunction(ctx, js_sage_fetch, "__sage_fetch", 2));
  JS_SetPropertyStr(ctx, global, "__sage_fetch_abort",
                    JS_NewCFunction(ctx, js_sage_fetch_abort,
                                    "__sage_fetch_abort", 1));
  JS_SetPropertyStr(ctx, global, "__sage_fs_data_dir",
                    JS_NewCFunction(ctx, js_sage_fs_data_dir,
                                    "__sage_fs_data_dir", 0));
  JS_SetPropertyStr(ctx, global, "__sage_fs_exists",
                    JS_NewCFunction(ctx, js_sage_fs_exists, "__sage_fs_exists",
                                    1));
  JS_SetPropertyStr(ctx, global, "__sage_fs_read_text",
                    JS_NewCFunction(ctx, js_sage_fs_read_text,
                                    "__sage_fs_read_text", 2));
  JS_SetPropertyStr(ctx, global, "__sage_fs_read_bytes",
                    JS_NewCFunction(ctx, js_sage_fs_read_bytes,
                                    "__sage_fs_read_bytes", 2));
  JS_SetPropertyStr(ctx, global, "__sage_fs_read_data_text",
                    JS_NewCFunction(ctx, js_sage_fs_read_data_text,
                                    "__sage_fs_read_data_text", 2));
  JS_SetPropertyStr(ctx, global, "__sage_fs_read_data_bytes",
                    JS_NewCFunction(ctx, js_sage_fs_read_data_bytes,
                                    "__sage_fs_read_data_bytes", 2));
  JS_SetPropertyStr(ctx, global, "__sage_fs_write_data_text",
                    JS_NewCFunction(ctx, js_sage_fs_write_data_text,
                                    "__sage_fs_write_data_text", 3));
  JS_SetPropertyStr(ctx, global, "__sage_fs_write_data_bytes",
                    JS_NewCFunction(ctx, js_sage_fs_write_data_bytes,
                                    "__sage_fs_write_data_bytes", 3));
  JS_SetPropertyStr(ctx, global, "__sage_fs_list_data",
                    JS_NewCFunction(ctx, js_sage_fs_list_data,
                                    "__sage_fs_list_data", 0));

  JS_FreeValue(ctx, global);
  return 0;
}

SageQjs *sage_qjs_new(int64_t verbose) {
  SageQjs *q = (SageQjs *)calloc(1, sizeof(SageQjs));
  if (!q) {
    return NULL;
  }

  q->verbose = (verbose != 0);
  q->plugins = NULL;
  q->plugins_len = 0;
  q->plugins_cap = 0;
  q->next_fetch_id = 1;
  q->exec_cmds = NULL;
  q->exec_cmds_len = 0;
  q->exec_cmds_cap = 0;
  q->exec_cmds_read = 0;
  q->fs_allow_read = NULL;
  q->fs_allow_read_len = 0;
  q->fs_allow_read_cap = 0;
  q->load_timeout_ms = sage_qjs_env_u32("SAGE_PLUGIN_LOAD_TIMEOUT_MS", 500);
  q->event_timeout_ms = sage_qjs_env_u32("SAGE_PLUGIN_EVENT_TIMEOUT_MS", 50);
  q->disabled = 0;
  q->had_error = 0;
  q->log_path = sage_qjs_default_log_path();
  q->log_file = NULL;
  q->log_stderr = (sage_qjs_env_u64("SAGE_PLUGIN_LOG_STDERR", 0) != 0);
  q->bootstrap_source = NULL;
  q->builtin_modules = NULL;
  q->builtin_modules_len = 0;
  q->builtin_modules_cap = 0;

  uint64_t mem_mb = sage_qjs_env_u64("SAGE_PLUGIN_MEM_LIMIT_MB", 64);
  q->mem_limit_bytes = mem_mb ? (mem_mb * 1024ull * 1024ull) : 0;

  uint64_t stack_kb = sage_qjs_env_u64("SAGE_PLUGIN_STACK_LIMIT_KB", 1024);
  q->stack_limit_bytes = stack_kb ? (stack_kb * 1024ull) : 0;

  if (q->verbose && q->log_path) {
    fprintf(stderr, "sage[plugin] log: %s\n", q->log_path);
    fflush(stderr);
  }
  return q;
}

int64_t sage_qjs_add_builtin_module(SageQjs *q, const uint8_t *name_ptr,
                                    int64_t name_len, const uint8_t *src_ptr,
                                    int64_t src_len) {
  if (!q || !name_ptr || !src_ptr) {
    return 1;
  }
  if (q->disabled) {
    return 1;
  }
  if (name_len <= 0 || src_len <= 0) {
    q->had_error = 1;
    return 1;
  }
  if (name_len > 4096 || src_len > (16 * 1024 * 1024)) {
    q->had_error = 1;
    return 1;
  }
  if (name_len < 5 || memcmp(name_ptr, "sage:", 5) != 0) {
    q->had_error = 1;
    return 1;
  }
  for (int64_t i = 0; i < name_len; i++) {
    if (name_ptr[i] == 0) {
      q->had_error = 1;
      return 1;
    }
  }

  size_t nlen = (size_t)name_len;
  size_t slen = (size_t)src_len;

  char *name = (char *)malloc(nlen + 1);
  if (!name) {
    q->had_error = 1;
    return 1;
  }
  memcpy(name, name_ptr, nlen);
  name[nlen] = '\0';

  char *src = (char *)malloc(slen + 1);
  if (!src) {
    free(name);
    q->had_error = 1;
    return 1;
  }
  memcpy(src, src_ptr, slen);
  src[slen] = '\0';

  // Replace existing entry, if present.
  for (size_t i = 0; i < q->builtin_modules_len; i++) {
    SageQjsBuiltinModule *m = &q->builtin_modules[i];
    if (!m->name) {
      continue;
    }
    if (strcmp(m->name, name) == 0) {
      free(name);
      free(m->source);
      m->source = src;
      m->source_len = slen;
      return 0;
    }
  }

  // Append.
  if (q->builtin_modules_len >= q->builtin_modules_cap) {
    size_t new_cap = q->builtin_modules_cap ? (q->builtin_modules_cap * 2) : 8;
    SageQjsBuiltinModule *new_ptr = (SageQjsBuiltinModule *)realloc(
        q->builtin_modules, new_cap * sizeof(SageQjsBuiltinModule));
    if (!new_ptr) {
      free(name);
      free(src);
      q->had_error = 1;
      return 1;
    }
    q->builtin_modules = new_ptr;
    q->builtin_modules_cap = new_cap;
  }

  SageQjsBuiltinModule *m = &q->builtin_modules[q->builtin_modules_len++];
  memset(m, 0, sizeof(*m));
  m->name = name;
  m->source = src;
  m->source_len = slen;
  return 0;
}

static void sage_qjs_proc_free(JSContext *ctx, SageQjsProc *pr) {
  if (!pr) {
    return;
  }
  if (pr->pid > 0 && !pr->exited && !pr->killed) {
    (void)kill(pr->pid, SIGKILL);
    pr->killed = 1;
  }
  if (pr->stdout_fd >= 0) {
    close(pr->stdout_fd);
    pr->stdout_fd = -1;
  }
  if (pr->stderr_fd >= 0) {
    close(pr->stderr_fd);
    pr->stderr_fd = -1;
  }
  free(pr->stdout_buf);
  pr->stdout_buf = NULL;
  pr->stdout_len = 0;
  pr->stdout_cap = 0;
  free(pr->stderr_buf);
  pr->stderr_buf = NULL;
  pr->stderr_len = 0;
  pr->stderr_cap = 0;
  if (ctx) {
    if (!JS_IsUndefined(pr->resolve_fn)) {
      JS_FreeValue(ctx, pr->resolve_fn);
      pr->resolve_fn = JS_UNDEFINED;
    }
    if (!JS_IsUndefined(pr->reject_fn)) {
      JS_FreeValue(ctx, pr->reject_fn);
      pr->reject_fn = JS_UNDEFINED;
    }
  }
  pr->pid = -1;
}

static void sage_qjs_fetch_free(JSContext *ctx, SageQjsFetch *f) {
  if (!f) {
    return;
  }

  curl_slist_free_all(f->req_headers);
  f->req_headers = NULL;

  free(f->req_url);
  f->req_url = NULL;
  free(f->req_method);
  f->req_method = NULL;
  free(f->req_body);
  f->req_body = NULL;
  f->req_body_len = 0;

  free(f->status_text);
  f->status_text = NULL;
  free(f->effective_url);
  f->effective_url = NULL;
  sage_qjs_fetch_headers_clear(f);

  free(f->resp_body);
  f->resp_body = NULL;
  f->resp_body_len = 0;
  f->resp_body_cap = 0;

  free(f->err);
  f->err = NULL;

  if (ctx) {
    if (!JS_IsUndefined(f->resolve_fn)) {
      JS_FreeValue(ctx, f->resolve_fn);
      f->resolve_fn = JS_UNDEFINED;
    }
    if (!JS_IsUndefined(f->reject_fn)) {
      JS_FreeValue(ctx, f->reject_fn);
      f->reject_fn = JS_UNDEFINED;
    }
  }

  free(f);
}

static void sage_qjs_plugin_clear_fetches(SageQjsPlugin *p) {
  if (!p || !p->fetches) {
    return;
  }
  JSContext *ctx = p->ctx;
  for (size_t i = 0; i < p->fetches_len; i++) {
    SageQjsFetch *f = p->fetches[i];
    if (f) {
      atomic_store_explicit(&f->cancelled, 1, memory_order_relaxed);
    }
  }
  for (size_t i = 0; i < p->fetches_len; i++) {
    SageQjsFetch *f = p->fetches[i];
    if (!f) {
      continue;
    }
    if (f->thread_started) {
      (void)pthread_join(f->thread, NULL);
      f->thread_started = 0;
    }
    sage_qjs_fetch_free(ctx, f);
  }
  free(p->fetches);
  p->fetches = NULL;
  p->fetches_len = 0;
  p->fetches_cap = 0;
}

static void sage_qjs_plugin_clear_procs(SageQjsPlugin *p) {
  if (!p || !p->procs) {
    return;
  }
  JSContext *ctx = p->ctx;
  for (size_t i = 0; i < p->procs_len; i++) {
    sage_qjs_proc_free(ctx, &p->procs[i]);
  }
  free(p->procs);
  p->procs = NULL;
  p->procs_len = 0;
  p->procs_cap = 0;
}

static void sage_qjs_plugin_close(SageQjsPlugin *p) {
  if (!p) {
    return;
  }

  sage_qjs_plugin_clear_fetches(p);
  sage_qjs_plugin_clear_procs(p);

  free(p->module_root);
  p->module_root = NULL;

  free(p->fs_data_dir);
  p->fs_data_dir = NULL;

  if (p->ctx) {
    if (!JS_IsUndefined(p->emit_fn)) {
      JS_FreeValue(p->ctx, p->emit_fn);
      p->emit_fn = JS_UNDEFINED;
    }
    if (!JS_IsUndefined(p->cmd_fn)) {
      JS_FreeValue(p->ctx, p->cmd_fn);
      p->cmd_fn = JS_UNDEFINED;
    }
    JS_FreeContext(p->ctx);
    p->ctx = NULL;
  }
  if (p->rt) {
    JS_FreeRuntime(p->rt);
    p->rt = NULL;
  }
  p->deadline_ns = 0;
  p->timed_out = 0;
}

static void sage_qjs_plugin_disable(SageQjsPlugin *p, const char *why) {
  if (!p) {
    return;
  }
  SageQjs *q = p->host;
  FILE *out = sage_qjs_log_stream(q);
  if (why && *why) {
    if (p->path && *p->path) {
      fprintf(out, "sage[plugin] %s (%s); disabling plugin\n", why, p->path);
    } else {
      fprintf(out, "sage[plugin] %s; disabling plugin\n", why);
    }
  } else {
    if (p->path && *p->path) {
      fprintf(out, "sage[plugin] disabling plugin (%s)\n", p->path);
    } else {
      fputs("sage[plugin] disabling plugin\n", out);
    }
  }
  fflush(out);

  p->disabled = 1;
  if (q) {
    q->had_error = 1;
  }
  sage_qjs_plugin_close(p);
}

static int sage_qjs_plugin_init_runtime(SageQjsPlugin *p) {
  if (!p) {
    return -1;
  }
  SageQjs *q = p->host;
  if (!q) {
    return -1;
  }

  p->load_timeout_ms = q->load_timeout_ms;
  p->event_timeout_ms = q->event_timeout_ms;
  p->deadline_ns = 0;
  p->timed_out = 0;
  p->disabled = 0;
  p->emit_fn = JS_UNDEFINED;
  p->cmd_fn = JS_UNDEFINED;
  p->module_root = NULL;
  p->procs = NULL;
  p->procs_len = 0;
  p->procs_cap = 0;
  p->fetches = NULL;
  p->fetches_len = 0;
  p->fetches_cap = 0;
  p->fs_data_dir = NULL;

  p->rt = JS_NewRuntime();
  if (!p->rt) {
    return -1;
  }

  JS_SetModuleLoaderFunc2(p->rt, sage_qjs_module_normalize, sage_qjs_module_loader,
                          NULL, p);

  JS_SetInterruptHandler(p->rt, sage_qjs_interrupt_handler, p);
  JS_SetCanBlock(p->rt, false);
  if (q->mem_limit_bytes) {
    JS_SetMemoryLimit(p->rt, (size_t)q->mem_limit_bytes);
  }
  if (q->stack_limit_bytes) {
    JS_SetMaxStackSize(p->rt, (size_t)q->stack_limit_bytes);
  }

  p->ctx = JS_NewContext(p->rt);
  if (!p->ctx) {
    JS_FreeRuntime(p->rt);
    p->rt = NULL;
    return -1;
  }

  JS_SetContextOpaque(p->ctx, p);
  if (sage_qjs_define_host_api(p) != 0) {
    sage_qjs_plugin_close(p);
    return -1;
  }

  return 0;
}

static int sage_qjs_plugin_capture_emit(SageQjsPlugin *p) {
  if (!p || !p->ctx) {
    return -1;
  }

  JSContext *ctx = p->ctx;
  JSValue global = JS_GetGlobalObject(ctx);

  // Bootstrap API: `globalThis.__sage_emit` / `globalThis.__sage_cmd`.
  JSValue emit = JS_GetPropertyStr(ctx, global, "__sage_emit");
  if (!JS_IsFunction(ctx, emit)) {
    JS_FreeValue(ctx, emit);
    JS_FreeValue(ctx, global);
    return -1;
  }

  JSValue cmd = JS_GetPropertyStr(ctx, global, "__sage_cmd");
  if (JS_IsFunction(ctx, cmd)) {
    if (!JS_IsUndefined(p->cmd_fn)) {
      JS_FreeValue(ctx, p->cmd_fn);
    }
    p->cmd_fn = cmd; // owned ref
  } else {
    JS_FreeValue(ctx, cmd);
    p->cmd_fn = JS_UNDEFINED;
  }

  if (!JS_IsUndefined(p->emit_fn)) {
    JS_FreeValue(ctx, p->emit_fn);
  }
  p->emit_fn = emit; // owned ref

  JS_FreeValue(ctx, global);
  return 0;
}

static int sage_qjs_plugin_eval_bootstrap(SageQjsPlugin *p) {
  if (!p || !p->ctx) {
    return -1;
  }
  SageQjs *q = p->host;
  if (!q || !q->bootstrap_source) {
    return -1;
  }

  JSContext *ctx = p->ctx;
  sage_qjs_begin_budget(p, p->load_timeout_ms);
  JSValue val = JS_Eval(ctx, q->bootstrap_source, strlen(q->bootstrap_source),
                        "<sage-bootstrap>", JS_EVAL_TYPE_GLOBAL);

  if (p->timed_out) {
    JS_FreeValue(ctx, val);
    sage_qjs_end_budget(p);
    sage_qjs_plugin_disable(p, "bootstrap timed out");
    return -1;
  }
  if (JS_IsException(val)) {
    sage_qjs_end_budget(p);
    sage_qjs_dump_exception(p);
    JS_FreeValue(ctx, val);
    sage_qjs_plugin_disable(p, "bootstrap threw");
    return -1;
  }
  JS_FreeValue(ctx, val);
  if (p->disabled) {
    sage_qjs_end_budget(p);
    return -1;
  }

  sage_qjs_drain_jobs(p);
  sage_qjs_end_budget(p);
  if (p->disabled) {
    return -1;
  }

  if (sage_qjs_plugin_capture_emit(p) != 0) {
    sage_qjs_plugin_disable(p, "bootstrap missing emit handler");
    return -1;
  }

  return 0;
}

void sage_qjs_free(SageQjs *q) {
  if (!q) {
    return;
  }
  if (q->plugins) {
    for (size_t i = 0; i < q->plugins_len; i++) {
      SageQjsPlugin *p = &q->plugins[i];
      sage_qjs_plugin_close(p);
      free(p->path);
      p->path = NULL;
    }
    free(q->plugins);
    q->plugins = NULL;
    q->plugins_len = 0;
    q->plugins_cap = 0;
  }
  if (q->exec_cmds) {
    for (size_t i = q->exec_cmds_read; i < q->exec_cmds_len; i++) {
      free(q->exec_cmds[i]);
    }
    free(q->exec_cmds);
    q->exec_cmds = NULL;
    q->exec_cmds_len = 0;
    q->exec_cmds_cap = 0;
    q->exec_cmds_read = 0;
  }
  if (q->fs_allow_read) {
    for (size_t i = 0; i < q->fs_allow_read_len; i++) {
      free(q->fs_allow_read[i]);
    }
    free(q->fs_allow_read);
    q->fs_allow_read = NULL;
    q->fs_allow_read_len = 0;
    q->fs_allow_read_cap = 0;
  }
  if (q->log_file) {
    fclose(q->log_file);
    q->log_file = NULL;
  }
  free(q->log_path);
  q->log_path = NULL;
  free(q->bootstrap_source);
  q->bootstrap_source = NULL;
  if (q->builtin_modules) {
    for (size_t i = 0; i < q->builtin_modules_len; i++) {
      SageQjsBuiltinModule *m = &q->builtin_modules[i];
      free(m->name);
      m->name = NULL;
      free(m->source);
      m->source = NULL;
      m->source_len = 0;
    }
    free(q->builtin_modules);
    q->builtin_modules = NULL;
    q->builtin_modules_len = 0;
    q->builtin_modules_cap = 0;
  }
  free(q);
}

static int sage_qjs_enqueue_exec_cmd(SageQjs *q, const char *cmd) {
  if (!q || !cmd) {
    return -1;
  }
  if (q->disabled) {
    return -1;
  }

  // Keep the queue bounded to avoid untrusted plugin memory blowups.
  const size_t MAX_CMDS = 256;
  size_t queued = 0;
  if (q->exec_cmds_len >= q->exec_cmds_read) {
    queued = q->exec_cmds_len - q->exec_cmds_read;
  }
  if (queued >= MAX_CMDS) {
    q->had_error = 1;
    return -1;
  }

  // Compact occasionally so the queue doesn't grow unbounded over time.
  if (q->exec_cmds_read > 0 && q->exec_cmds_read == q->exec_cmds_len) {
    q->exec_cmds_len = 0;
    q->exec_cmds_read = 0;
  } else if (q->exec_cmds_read > 0 && q->exec_cmds_read > (q->exec_cmds_cap / 2)) {
    size_t remain = q->exec_cmds_len - q->exec_cmds_read;
    for (size_t i = 0; i < remain; i++) {
      q->exec_cmds[i] = q->exec_cmds[q->exec_cmds_read + i];
    }
    q->exec_cmds_len = remain;
    q->exec_cmds_read = 0;
  }

  if (q->exec_cmds_len >= q->exec_cmds_cap) {
    size_t new_cap = q->exec_cmds_cap ? (q->exec_cmds_cap * 2) : 16;
    char **new_ptr = (char **)realloc(q->exec_cmds, new_cap * sizeof(char *));
    if (!new_ptr) {
      q->had_error = 1;
      return -1;
    }
    q->exec_cmds = new_ptr;
    q->exec_cmds_cap = new_cap;
  }

  size_t n = strlen(cmd);
  char *copy = (char *)malloc(n + 1);
  if (!copy) {
    q->had_error = 1;
    return -1;
  }
  memcpy(copy, cmd, n);
  copy[n] = '\0';
  q->exec_cmds[q->exec_cmds_len++] = copy;
  return 0;
}

int64_t sage_qjs_take_exec_cmd(SageQjs *q, uint8_t *out, int64_t cap) {
  if (!q || !out || cap <= 0) {
    return 0;
  }
  if (!q->exec_cmds || q->exec_cmds_read >= q->exec_cmds_len) {
    return 0;
  }

  char *s = q->exec_cmds[q->exec_cmds_read++];
  if (!s) {
    return 0;
  }

  size_t n = strlen(s);
  if ((int64_t)n > cap) {
    // Put it back so the caller can retry with a bigger buffer.
    q->exec_cmds_read--;
    return -(int64_t)n;
  }

  memcpy(out, s, n);
  if ((int64_t)n < cap) {
    out[n] = 0;
  }
  free(s);
  q->exec_cmds[q->exec_cmds_read - 1] = NULL;

  // Reset when fully drained.
  if (q->exec_cmds_read >= q->exec_cmds_len) {
    q->exec_cmds_len = 0;
    q->exec_cmds_read = 0;
  }
  return (int64_t)n;
}

int64_t sage_qjs_command(SageQjs *q, const char *name, const char *args) {
  if (!q || !name) {
    return 0;
  }
  if (q->disabled) {
    return 0;
  }

  int handled = 0;
  const char *a = args ? args : "";

  for (size_t i = 0; i < q->plugins_len; i++) {
    SageQjsPlugin *p = &q->plugins[i];
    if (!p->ctx || p->disabled) {
      continue;
    }
    if (JS_IsUndefined(p->cmd_fn)) {
      continue;
    }

    JSContext *ctx = p->ctx;
    JSValue n = JS_NewString(ctx, name);
    JSValue av = JS_NewString(ctx, a);
    JSValue argv[2] = {n, av};
    sage_qjs_begin_budget(p, p->event_timeout_ms);
    JSValue ret = JS_Call(ctx, p->cmd_fn, JS_UNDEFINED, 2, argv);
    JS_FreeValue(ctx, n);
    JS_FreeValue(ctx, av);

    if (p->timed_out) {
      if (JS_IsException(ret)) {
        sage_qjs_dump_exception(p);
      }
      JS_FreeValue(ctx, ret);
      sage_qjs_end_budget(p);
      sage_qjs_plugin_disable(p, "command timed out");
      continue;
    }

    if (JS_IsException(ret)) {
      sage_qjs_end_budget(p);
      sage_qjs_dump_exception(p);
      JS_FreeValue(ctx, ret);
      sage_qjs_plugin_disable(p, "command threw");
      continue;
    }

    if (JS_ToBool(ctx, ret)) {
      handled = 1;
    }
    JS_FreeValue(ctx, ret);

    if (p->disabled) {
      sage_qjs_end_budget(p);
      continue;
    }

    sage_qjs_drain_jobs(p);
    sage_qjs_end_budget(p);
  }

  return handled ? 1 : 0;
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
  if (q->plugins) {
    for (size_t i = 0; i < q->plugins_len; i++) {
      SageQjsPlugin *p = &q->plugins[i];
      p->load_timeout_ms = q->load_timeout_ms;
      p->event_timeout_ms = q->event_timeout_ms;
    }
  }
}

int64_t sage_qjs_reserve_plugins(SageQjs *q, int64_t count) {
  if (!q) {
    return 1;
  }
  if (q->disabled) {
    return 1;
  }
  if (count <= 0) {
    return 0;
  }
  if (q->plugins_len != 0) {
    // Plugin runtimes store opaque pointers to SageQjsPlugin; reallocating the
    // backing array after creation would invalidate those pointers.
    q->had_error = 1;
    return 1;
  }
  size_t need = (size_t)count;
  if (need <= q->plugins_cap) {
    return 0;
  }
  SageQjsPlugin *new_ptr = (SageQjsPlugin *)realloc(q->plugins, need * sizeof(SageQjsPlugin));
  if (!new_ptr) {
    q->had_error = 1;
    return 1;
  }
  q->plugins = new_ptr;
  q->plugins_cap = need;
  return 0;
}

void sage_qjs_set_limits(SageQjs *q, int64_t mem_limit_bytes,
                         int64_t stack_limit_bytes) {
  if (!q) {
    return;
  }
  if (mem_limit_bytes >= 0) {
    q->mem_limit_bytes = (uint64_t)mem_limit_bytes;
    if (q->plugins) {
      for (size_t i = 0; i < q->plugins_len; i++) {
        SageQjsPlugin *p = &q->plugins[i];
        if (p->rt) {
          JS_SetMemoryLimit(p->rt, (size_t)q->mem_limit_bytes);
        }
      }
    }
  }
  if (stack_limit_bytes >= 0) {
    q->stack_limit_bytes = (uint64_t)stack_limit_bytes;
    if (q->plugins) {
      for (size_t i = 0; i < q->plugins_len; i++) {
        SageQjsPlugin *p = &q->plugins[i];
        if (p->rt) {
          JS_SetMaxStackSize(p->rt, (size_t)q->stack_limit_bytes);
        }
      }
    }
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

int64_t sage_qjs_allow_fs_read_path(SageQjs *q, const char *path) {
  if (!q || !path || !*path) {
    return 1;
  }
  if (sage_qjs_fs_allow_read_add(q, path) != 0) {
    q->had_error = 1;
    return 1;
  }
  return 0;
}

int64_t sage_qjs_poll(SageQjs *q) {
  if (!q) {
    return 0;
  }
  if (q->disabled) {
    return 0;
  }
  for (size_t i = 0; i < q->plugins_len; i++) {
    SageQjsPlugin *p = &q->plugins[i];
    if (!p->ctx || p->disabled) {
      continue;
    }
    sage_qjs_plugin_poll_procs(p);
    sage_qjs_plugin_poll_fetches(p);
  }
  return 0;
}

int64_t sage_qjs_eval_bootstrap(SageQjs *q, const char *source) {
  if (!q || !source) {
    return 1;
  }
  if (q->disabled) {
    return 1;
  }

  free(q->bootstrap_source);
  q->bootstrap_source = NULL;

  if (!*source) {
    q->had_error = 1;
    return 1;
  }

  q->bootstrap_source = strdup(source);
  if (!q->bootstrap_source) {
    q->had_error = 1;
    return 1;
  }

  // Validate bootstrap once so we can fail fast before loading plugins.
  SageQjsPlugin tmp;
  memset(&tmp, 0, sizeof(tmp));
  tmp.host = q;
  tmp.path = "<bootstrap>";
  if (sage_qjs_plugin_init_runtime(&tmp) != 0) {
    q->had_error = 1;
    return 1;
  }
  int boot_rc = sage_qjs_plugin_eval_bootstrap(&tmp);
  sage_qjs_plugin_close(&tmp);
  if (boot_rc != 0) {
    q->had_error = 1;
    q->disabled = 1;
    return 1;
  }
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
  if (!q || !path) {
    return 1;
  }
  if (q->disabled) {
    return 1;
  }
  if (!q->bootstrap_source) {
    FILE *out = sage_qjs_log_stream(q);
    fputs("sage[plugin] bootstrap not initialized; skipping plugin load\n", out);
    fflush(out);
    q->had_error = 1;
    return 1;
  }

  // Allocate a plugin slot.
  if (q->plugins_len >= q->plugins_cap) {
    size_t new_cap = q->plugins_cap ? (q->plugins_cap * 2) : 8;
    SageQjsPlugin *new_ptr =
        (SageQjsPlugin *)realloc(q->plugins, new_cap * sizeof(SageQjsPlugin));
    if (!new_ptr) {
      q->had_error = 1;
      return 1;
    }
    q->plugins = new_ptr;
    q->plugins_cap = new_cap;
  }

  SageQjsPlugin *p = &q->plugins[q->plugins_len++];
  memset(p, 0, sizeof(*p));
  p->host = q;
  p->emit_fn = JS_UNDEFINED;
  p->path = strdup(path);
  if (!p->path) {
    q->had_error = 1;
    return 1;
  }

  if (sage_qjs_plugin_init_runtime(p) != 0) {
    q->had_error = 1;
    return 1;
  }

  if (sage_qjs_plugin_eval_bootstrap(p) != 0) {
    q->had_error = 1;
    return 1;
  }

  uint8_t *buf = NULL;
  size_t len = 0;
  if (sage_qjs_read_file(path, &buf, &len) != 0) {
    FILE *out = sage_qjs_log_stream(q);
    fprintf(out, "sage[plugin] failed to read plugin: %s\n", path);
    fflush(out);
    q->had_error = 1;
    sage_qjs_plugin_disable(p, "failed to read plugin");
    return 1;
  }

  // Compute a canonical plugin module root for safe relative imports.
  if (!p->module_root && p->path && *p->path) {
    char *rp = sage_qjs_realpath_owned(p->path);
    if (rp) {
      p->module_root = sage_qjs_dirname_owned(rp);
      free(rp);
    }
  }

  JSContext *ctx = p->ctx;
  sage_qjs_begin_budget(p, p->load_timeout_ms);
  JSValue val = JS_Eval(ctx, (const char *)buf, len, path,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  free(buf);

  if (p->timed_out) {
    JS_FreeValue(ctx, val);
    sage_qjs_end_budget(p);
    sage_qjs_plugin_disable(p, "plugin load timed out");
    return 1;
  }
  if (JS_IsException(val)) {
    sage_qjs_end_budget(p);
    sage_qjs_dump_exception(p);
    JS_FreeValue(ctx, val);
    sage_qjs_plugin_disable(p, "plugin threw during load");
    return 1;
  }
  val = JS_EvalFunction(ctx, val);
  if (p->timed_out) {
    if (JS_IsException(val)) {
      sage_qjs_dump_exception(p);
    }
    JS_FreeValue(ctx, val);
    sage_qjs_end_budget(p);
    sage_qjs_plugin_disable(p, "plugin load timed out");
    return 1;
  }
  if (JS_IsException(val)) {
    sage_qjs_end_budget(p);
    sage_qjs_dump_exception(p);
    JS_FreeValue(ctx, val);
    sage_qjs_plugin_disable(p, "plugin threw during load");
    return 1;
  }

  if (p->disabled) {
    JS_FreeValue(ctx, val);
    sage_qjs_end_budget(p);
    return 1;
  }

  sage_qjs_drain_jobs(p);
  if (!p->disabled && JS_IsPromise(val)) {
    JSPromiseStateEnum st = JS_PromiseState(ctx, val);
    if (st == JS_PROMISE_PENDING) {
      JS_FreeValue(ctx, val);
      sage_qjs_end_budget(p);
      sage_qjs_plugin_disable(p, "plugin initialization is still pending (top-level await)");
      return 1;
    }
  }
  JS_FreeValue(ctx, val);
  sage_qjs_end_budget(p);
  if (p->disabled) {
    return 1;
  }
  return 0;
}

static void sage_qjs_plugin_emit_event(SageQjsPlugin *p, const char *event,
                                       JSValue payload) {
  if (!p || !p->ctx || !event) {
    return;
  }
  if (p->disabled) {
    if (!JS_IsUndefined(payload)) {
      JS_FreeValue(p->ctx, payload);
    }
    return;
  }
  if (JS_IsUndefined(p->emit_fn)) {
    if (!JS_IsUndefined(payload)) {
      JS_FreeValue(p->ctx, payload);
    }
    return;
  }

  JSContext *ctx = p->ctx;
  JSValue ev = JS_NewString(ctx, event);
  JSValue argv[2] = {ev, payload};
  sage_qjs_begin_budget(p, p->event_timeout_ms);
  JSValue ret = JS_Call(ctx, p->emit_fn, JS_UNDEFINED, 2, argv);
  JS_FreeValue(ctx, ev);
  if (!JS_IsUndefined(payload)) {
    JS_FreeValue(ctx, payload);
  }

  if (p->timed_out) {
    if (JS_IsException(ret)) {
      sage_qjs_dump_exception(p);
    }
    JS_FreeValue(ctx, ret);
    sage_qjs_end_budget(p);
    sage_qjs_plugin_disable(p, "event timed out");
    return;
  }

  if (JS_IsException(ret)) {
    sage_qjs_end_budget(p);
    sage_qjs_dump_exception(p);
    JS_FreeValue(ctx, ret);
    sage_qjs_plugin_disable(p, "event threw");
    return;
  }
  JS_FreeValue(ctx, ret);

  if (p->disabled) {
    sage_qjs_end_budget(p);
    return;
  }
  sage_qjs_drain_jobs(p);
  sage_qjs_end_budget(p);
}

int64_t sage_qjs_emit_open(SageQjs *q, const char *path, int64_t tab,
                           int64_t tab_count) {
  if (!q) {
    return 1;
  }
  if (q->disabled) {
    return 0;
  }

  for (size_t i = 0; i < q->plugins_len; i++) {
    SageQjsPlugin *p = &q->plugins[i];
    if (!p->ctx || p->disabled) {
      continue;
    }
    JSContext *ctx = p->ctx;
    JSValue payload = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, payload, "path",
                      JS_NewString(ctx, path ? path : ""));
    JS_SetPropertyStr(ctx, payload, "tab", JS_NewInt64(ctx, tab));
    JS_SetPropertyStr(ctx, payload, "tab_count", JS_NewInt64(ctx, tab_count));
    sage_qjs_plugin_emit_event(p, "open", payload);
  }
  return 0;
}

int64_t sage_qjs_emit_tab_change(SageQjs *q, int64_t from, int64_t to,
                                 int64_t tab_count) {
  if (!q) {
    return 1;
  }
  if (q->disabled) {
    return 0;
  }

  for (size_t i = 0; i < q->plugins_len; i++) {
    SageQjsPlugin *p = &q->plugins[i];
    if (!p->ctx || p->disabled) {
      continue;
    }
    JSContext *ctx = p->ctx;
    JSValue payload = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, payload, "from", JS_NewInt64(ctx, from));
    JS_SetPropertyStr(ctx, payload, "to", JS_NewInt64(ctx, to));
    JS_SetPropertyStr(ctx, payload, "tab_count", JS_NewInt64(ctx, tab_count));
    sage_qjs_plugin_emit_event(p, "tab_change", payload);
  }
  return 0;
}

int64_t sage_qjs_emit_search(SageQjs *q, const char *query, int64_t regex,
                             int64_t ignore_case) {
  if (!q) {
    return 1;
  }
  if (q->disabled) {
    return 0;
  }

  for (size_t i = 0; i < q->plugins_len; i++) {
    SageQjsPlugin *p = &q->plugins[i];
    if (!p->ctx || p->disabled) {
      continue;
    }
    JSContext *ctx = p->ctx;
    JSValue payload = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, payload, "query",
                      JS_NewString(ctx, query ? query : ""));
    JS_SetPropertyStr(ctx, payload, "regex", JS_NewBool(ctx, regex != 0));
    JS_SetPropertyStr(ctx, payload, "ignore_case",
                      JS_NewBool(ctx, ignore_case != 0));
    sage_qjs_plugin_emit_event(p, "search", payload);
  }
  return 0;
}

int64_t sage_qjs_emit_copy(SageQjs *q, int64_t bytes) {
  if (!q) {
    return 1;
  }
  if (q->disabled) {
    return 0;
  }

  for (size_t i = 0; i < q->plugins_len; i++) {
    SageQjsPlugin *p = &q->plugins[i];
    if (!p->ctx || p->disabled) {
      continue;
    }
    JSContext *ctx = p->ctx;
    JSValue payload = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, payload, "bytes", JS_NewInt64(ctx, bytes));
    sage_qjs_plugin_emit_event(p, "copy", payload);
  }
  return 0;
}

int64_t sage_qjs_emit_quit(SageQjs *q) {
  if (!q) {
    return 1;
  }
  if (q->disabled) {
    return 0;
  }

  for (size_t i = 0; i < q->plugins_len; i++) {
    SageQjsPlugin *p = &q->plugins[i];
    if (!p->ctx || p->disabled) {
      continue;
    }
    // Note: emit frees the payload when it's non-undefined; so pass undefined
    // and leave ownership untouched.
    sage_qjs_plugin_emit_event(p, "quit", JS_UNDEFINED);
  }
  return 0;
}
