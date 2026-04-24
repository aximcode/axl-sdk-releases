# AXL Configuration Framework Design

**Status: SHIPPED.** The unified `AxlConfig` framework described below
is implemented in `include/axl/axl-config.h` and `src/util/axl-config.c`,
and the separate `axl_args_*` API has been removed — every tool, test,
and example now uses a single `AxlConfigDesc` descriptor table for
both configuration and command-line parsing. This document is preserved
as the original design rationale.

## Problem (original)

AXL originally had two separate systems for parsing user input into
typed values:

1. **`axl_args_parse`** — command-line arguments (`-p 8080`, `--verbose`)
2. **`axl_http_client_set`** — string key-value config (`"timeout.ms"`, `"30000"`)

Both solved the same underlying problem: declare valid options, parse
strings into typed values, provide defaults, validate input. They
shared no code and had different APIs. Adding configuration to a new
AXL type meant writing a new set/get implementation from scratch.

Consumer apps faced the same issue: they defined options for arg
parsing AND for runtime configuration, duplicating descriptors.

## Goals

- **One descriptor table** defines options for both args and config
- **Multiple input sources**: command-line args, key-value set, environment
  variables, config files (future)
- **Type validation** against descriptors — rejects "abc" for uint options
- **Built-in defaults** from descriptors, applied at creation time
- **`--help` generation** from the same descriptors
- **Reusable** across any AXL object or consumer app
- **Cascade**: command-line overrides programmatic overrides defaults
- **Callback** for options that need custom logic (side effects)

## Non-Goals

- Thread safety (UEFI is single-threaded)
- Config file parsing (future — Phase CF2)
- Schema versioning or migration

## Current State

### Types that need configuration today

**Already uses string config:**

- `AxlHttpClient` — `timeout.ms`, `keep.alive`, `max.redirects`,
  `tls.verify`, `header.*`

**Should use string config (immediate candidates):**

- `AxlHttpServer` — currently has `axl_http_server_set_max_connections`,
  `axl_http_server_set_body_limit`, `axl_http_server_set_keep_alive`.
  These should become `axl_http_server_set(s, "max.connections", "16")`
  etc.
- `AxlTcp` / `AxlTcpListener` — timeout is passed per-call now, but
  socket-level options like `nodelay`, `recv.buffer.size`,
  `send.buffer.size` would be useful

**Could benefit in the future:**

- `AxlLoop` — tick interval, max sources, idle behavior
- `AxlLog` — log level, output format, domain filtering. Currently
  uses `AXL_LOG_DOMAIN` macro and compile-time level, but runtime
  config like `axl_log_set("level", "debug")` or
  `axl_log_set("domain.tcp", "warning")` would be powerful
- `AxlBufPool` — growth policy, alignment
- `AxlArena` — block size, growth factor
- `AxlJsonBuilder` — indent style, pretty print options

**Consumer-facing (the biggest win):**

- Any AXL app's own configuration — httpfs's server settings,
  uefi-devkit tool options. The unified system would let consumer
  apps define their options once and populate them from command-line
  args, config files, or programmatic calls.

### Summary

| Type | Current API | Options |
|------|-------------|---------|
| `AxlHttpClient` | `axl_http_client_set(c, key, value)` | timeout.ms, keep.alive, max.redirects, tls.verify, header.* |
| `AxlHttpServer` | typed setters | max.connections, body.limit, keep.alive |
| `AxlTcp` | timeout per-call | timeout.ms, nodelay, recv.buffer.size |
| `AxlLog` | compile-time macros | level, domain filtering, output format |
| `AxlLoop` | none | tick.interval, max.sources |
| `AxlBufPool` | constructor args | growth.policy, alignment |
| `AxlArena` | constructor args | block.size, growth.factor |
| Consumer apps | `axl_config_new` + `axl_config_parse_args` | app-specific flags and values |

### What got unified

`AxlArgs` and `axl_http_client_set`/`get` merged into `AxlConfig`.
The old `AxlOpt` descriptor array was extended with type and default
fields and became `AxlConfigDesc`. `axl_args_*` was removed entirely
once every in-tree consumer migrated.

## Design

### Descriptor Table

Each option is declared once in a static descriptor array:

```c
static const AxlConfigDesc my_opts[] = {
    //  key              type            default   short  description
    { "timeout.ms",     AXL_CFG_UINT,   "10000",  0,     "Per-operation timeout in ms" },
    { "keep.alive",     AXL_CFG_BOOL,   "true",   'k',   "Reuse TCP connections" },
    { "max.redirects",  AXL_CFG_INT,    "5",      0,     "HTTP redirect limit (0=disable)" },
    { "port",           AXL_CFG_UINT,   "8080",   'p',   "Listen port" },
    { "verbose",        AXL_CFG_BOOL,   "false",  'v',   "Verbose output" },
    { "output",         AXL_CFG_STRING, NULL,      'o',   "Output file path" },
    { 0 }
};
```

Option types:

```c
#define AXL_CFG_BOOL    1   // "true"/"false"/"1"/"0"
#define AXL_CFG_INT     2   // signed integer
#define AXL_CFG_UINT    3   // unsigned integer
#define AXL_CFG_STRING  4   // arbitrary string
#define AXL_CFG_MULTI   5   // repeatable string (array)
```

The `short` field (single char) enables command-line flag mapping:
`-p 8080` maps to `"port"` = `"8080"`. Zero means no short flag.

### AxlConfig Object

```c
typedef struct AxlConfig AxlConfig;

/// Callback: called BEFORE descriptor lookup when an option is set.
/// Handles dynamic keys (e.g. "header.*") not in the descriptor table.
///   return  0: accepted, proceed with descriptor lookup + auto-apply
///   return  1: handled by callback (value stored, auto-apply skipped)
///   return -1: rejected
typedef int (*AxlConfigApplyFunc)(void *target, const char *key, const char *value);

/// Create a config object from descriptors.
/// @param descs      option descriptor table (static, not copied)
/// @param apply_fn   callback for custom option handling (NULL for auto-only)
/// @param target     opaque pointer passed to apply_fn (e.g., the owning object)
AxlConfig *axl_config_new(const AxlConfigDesc *descs, AxlConfigApplyFunc apply_fn, void *target);
void       axl_config_free(AxlConfig *cfg);

/// Set an option. Validates type, calls apply_fn if set.
int         axl_config_set(AxlConfig *cfg, const char *key, const char *value);

/// Get an option value as string.
const char *axl_config_get(AxlConfig *cfg, const char *key);

/// Typed getters (convenience, parse from stored string).
bool        axl_config_get_bool(AxlConfig *cfg, const char *key);
int64_t     axl_config_get_int(AxlConfig *cfg, const char *key);
uint64_t    axl_config_get_uint(AxlConfig *cfg, const char *key);

/// Parse command-line arguments into config.
/// Unrecognized args are collected as positional arguments.
int         axl_config_parse_args(AxlConfig *cfg, int argc, char **argv);

/// Get positional argument by index (0-based).
const char *axl_config_pos(AxlConfig *cfg, int index);

/// Get count of positional arguments.
int         axl_config_pos_count(AxlConfig *cfg);

/// Print usage/help from descriptors.
void        axl_config_usage(AxlConfig *cfg, const char *program, const char *synopsis);
```

### Auto-Apply via offsetof (Optional)

For simple struct-field options, descriptors can include an offset
so the config system writes the parsed value directly into the
target struct without a callback:

```c
typedef struct {
    const char *key;
    int         type;
    const char *default_value;
    char        short_flag;
    const char *description;
    size_t      offset;       // offsetof into target struct, or 0
    size_t      field_size;   // sizeof the field (for bounds checking)
} AxlConfigDesc;
```

When `offset != 0`, `axl_config_set` parses the value and writes it
directly to `(uint8_t *)target + offset`. No callback needed.

When `offset == 0`, the callback handles it (or the value is just
stored in the config's internal hash table for later retrieval).

This gives three levels of configuration:

1. **Auto-apply**: descriptor with `offsetof` — zero code per option
2. **Callback**: descriptor + `apply_fn` — custom logic (hash table
   insertion, side effects like socket reconfiguration)
3. **Store-only**: no offset, no callback — value stored in config,
   read back with `axl_config_get`

### Cascade Order

When multiple sources provide the same option:

```
defaults (from descriptor) → programmatic set → command-line args
```

Later sources override earlier ones. Implementation: defaults applied
at `axl_config_new`, then `axl_config_set` overwrites, then
`axl_config_parse_args` overwrites again.

### Integration with Existing Types

#### HTTP Client

```c
// Internal struct embeds AxlConfig
struct AxlHttpClient {
    AxlConfig  *config;
    AxlTcp     *sock;
    char       *connected_host;
    uint16_t    connected_port;
    bool        keep_alive;       // auto-applied from "keep.alive"
    size_t      timeout_ms;       // auto-applied from "timeout.ms"
    int         max_redirects;    // auto-applied from "max.redirects"
    ...
};

static const AxlConfigDesc http_client_descs[] = {
    { "timeout.ms",    AXL_CFG_UINT, "10000", 0, "Per-operation timeout",
      offsetof(AxlHttpClient, timeout_ms), sizeof(size_t) },
    { "keep.alive",    AXL_CFG_BOOL, "true",  0, "Reuse connections",
      offsetof(AxlHttpClient, keep_alive), sizeof(bool) },
    { "max.redirects", AXL_CFG_INT,  "5",     0, "Redirect limit",
      offsetof(AxlHttpClient, max_redirects), sizeof(int) },
    { 0 }
};

static int http_client_apply(void *target, const char *key, const char *value) {
    AxlHttpClient *c = target;
    // Handle "header.*" prefix — goes into hash table, not a struct field
    if (axl_strlen(key) > 7 && axl_strncmp(key, "header.", 7) == 0) {
        axl_hash_table_replace(c->default_headers, key + 7, axl_strdup(value));
        return 1;  // handled — skip auto-apply
    }
    return 0;  // not handled — proceed with descriptor lookup + auto-apply
}

AxlHttpClient *axl_http_client_new(void) {
    AxlHttpClient *c = axl_calloc(1, sizeof(*c));
    c->config = axl_config_new(http_client_descs, http_client_apply, c);
    // defaults auto-applied: c->timeout_ms = 10000, c->keep_alive = true, etc.
    return c;
}

// Public API becomes a thin wrapper:
int axl_http_client_set(AxlHttpClient *c, const char *key, const char *value) {
    return axl_config_set(c->config, key, value);
}
```

#### Consumer App

```c
static const AxlConfigDesc app_opts[] = {
    { "port",    AXL_CFG_UINT,   "8080",  'p', "Listen port" },
    { "host",    AXL_CFG_STRING, "0.0.0.0", 'H', "Bind address" },
    { "verbose", AXL_CFG_BOOL,   "false", 'v', "Verbose output" },
    { "help",    AXL_CFG_BOOL,   "false", 'h', "Show help" },
    { 0 }
};

int main(int argc, char **argv) {
    AxlConfig *cfg = axl_config_new(app_opts, NULL, NULL);
    axl_config_parse_args(cfg, argc, argv);

    if (axl_config_get_bool(cfg, "help")) {
        axl_config_usage(cfg, "myapp", "[options]");
        return 0;
    }

    uint16_t port = (uint16_t)axl_config_get_uint(cfg, "port");
    const char *host = axl_config_get(cfg, "host");
    // ...
}
```

### Migration Path

1. Implement `AxlConfig` core (new module, no existing API changes)
2. Integrate into `AxlHttpClient` — replace internal `set`/`get` with
   `AxlConfig`, keep `axl_http_client_set`/`get` as thin wrappers
3. Integrate into `AxlHttpServer` — replace typed setters
4. Add `axl_config_parse_args` — replaces `axl_args_parse`
5. Deprecate `axl_args_*` API (keep for backward compat, implement
   as wrapper around AxlConfig)
6. Integrate into `AxlLog` for runtime log level control

Steps 1-3 can be done incrementally without breaking existing code.

### Header Layout

```
include/axl/axl-config.h    — AxlConfig public API + AxlConfigDesc
src/util/axl-config.c        — implementation
```

Added to `axl.h` umbrella include.

## Open Questions

1. **Should `AxlConfig` own the target pointer?** Currently it's a
   borrowed pointer (caller manages lifetime). This means the config
   must not outlive the target. Alternative: config copies field
   values into internal storage and the target reads from config
   (no auto-apply via offsetof).

Answer: I like the auto-apply, but if adds a lot of complexity
then maybe not. I leave this one up to you.

2. **Multi-value options** (like `header.*`): should these be first-class
   in the descriptor table, or always handled via callback? The
   `AXL_CFG_MULTI` type could store an array, but the wildcard
   prefix pattern (`header.*`) is different from a repeatable flag.

Answer: No preference as long as we're not restricting functionality

3. **Validation beyond type**: range checks (port 1-65535), enum values
   ("level" must be "debug"|"info"|"warning"|"error"). Could add
   optional `validate_fn` to descriptors, or keep it in the
   apply callback.

Answer: Users can do extra/custom validation in the callback. KISS

4. **Config inheritance**: should a child config (e.g., per-connection
   TCP options) inherit from a parent (server-level options)?
   Useful for HTTP server where each connection gets server defaults
   but can override.

Answer: That's nice-to-have. It can be done easily then by all means
do it.

## Implementation Estimate

- Core `AxlConfig` (new, set, get, typed getters, free): ~200 lines
- `axl_config_parse_args`: ~100 lines (reuse logic from `axl_args_parse`)
- Auto-apply via offsetof: ~50 lines
- `axl_config_usage` (help generation): ~40 lines
- HTTP client integration: ~50 lines (simplification)
- HTTP server integration: ~50 lines
- Tests: ~100 lines

Total: ~600 lines new code, ~200 lines removed from HTTP client.
