/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-config-file.h
 *
 * Free-form `key=value` config-file map. Parses a text file into a flat
 * string map and serves typed getters that fall back to a caller-supplied
 * default for any missing (or unparseable) key.
 *
 * This is the open-vocabulary counterpart to `<axl/axl-config.h>`
 * AxlConfig, which is descriptor-bound — AxlConfig validates each key
 * against a fixed `AxlConfigDesc[]`, types it, auto-applies it via
 * offsetof, and REJECTS unknown keys. Use AxlConfig for a known, typed
 * option set (e.g. a server's tunables). Use AxlConfigFile when the keys
 * are not known at compile time — a `softbmc.cfg`-style file where modules
 * invent their own `prefix.key` names and read them with a runtime default.
 *
 * File format:
 *   # comments start with '#'; blank lines are ignored
 *   mode=handoff
 *   boot_timeout=30
 *   ec.poll_interval_ms=5000        # the dot is just a naming convention
 *
 * ASCII, case-sensitive keys/values. The value is everything after the
 * first `=` to end of line, trimmed of surrounding whitespace; there is no
 * quoting. The map is flat — a `prefix.key` convention is the caller's, not
 * the loader's.
 */

#ifndef AXL_CONFIG_FILE_H
#define AXL_CONFIG_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A free-form `key=value` string map. Opaque; create with
 *        `axl_config_file_load` / `_new`, free with `_free`.
 */
typedef struct AxlConfigFile AxlConfigFile;

/**
 * @brief Load a `key=value` file into a flat string map.
 *
 * Reads @p path via `<axl/axl-fs.h>` and parses it: lines are split on the
 * first `=`, the key and value are trimmed of surrounding whitespace, `#`
 * comment lines and blank lines are skipped, and a line with no `=` is
 * ignored. A later assignment of the same key overrides an earlier one.
 *
 * A missing or unreadable file is NOT an error — it yields an empty map, so
 * every lookup returns its caller default. NULL is returned only on out of
 * memory (or a NULL @p path).
 *
 * @return new map (possibly empty), or NULL on OOM / NULL @p path.
 */
AxlConfigFile *
axl_config_file_load(
    const char *path   ///< file path (e.g. "FS0:\\softbmc.cfg")
);

/**
 * @brief Parse `key=value` text already in memory into the map.
 *
 * Same line grammar as axl_config_file_load: lines are split on the first
 * `=`, key and value are trimmed of surrounding whitespace, `#` comment
 * lines and blank lines are skipped, and a line with no `=` is ignored. A
 * later assignment of the same key overrides an earlier one within @p text,
 * and overrides any value already present in @p cf.
 *
 * For a caller whose config text lives somewhere other than a filesystem
 * (e.g. an NVRAM variable) -- the in-memory counterpart of
 * axl_config_file_load. @p text is a NUL-terminated UTF-8 string with lines
 * separated by `\n` (a trailing `\r` before the `\n` is stripped, same as
 * the file loader). @p cf is NOT cleared first; parse into a freshly
 * created axl_config_file_new() when a clean parse is wanted.
 *
 * @return AXL_OK on success, AXL_ERR on NULL @p cf / @p text.
 */
int
axl_config_file_parse_string(
    AxlConfigFile *cf,     ///< map to parse into
    const char    *text    ///< key=value text (NUL-terminated, '\n'-separated lines)
);

/**
 * @brief Create an empty map (set-only / all-defaults).
 *
 * @return new empty map, or NULL on OOM.
 */
AxlConfigFile *
axl_config_file_new(void);

/**
 * @brief Free a config-file map. NULL-safe.
 */
void
axl_config_file_free(
    AxlConfigFile *cf   ///< map to free
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlConfigFile, axl_config_file_free)
#endif

/**
 * @brief Get a string value, or @p def if the key is absent.
 *
 * The returned pointer is owned by the map and valid until the key is
 * overwritten (`axl_config_file_set`) or the map is freed. @p def is
 * returned as-is (the map does not copy it).
 *
 * @return the stored value, or @p def if @p key is not present.
 */
const char *
axl_config_file_get(
    AxlConfigFile *cf,    ///< map (NULL → returns @p def)
    const char    *key,   ///< key to look up
    const char    *def    ///< default if absent (may be NULL)
);

/**
 * @brief Get a value parsed as an unsigned integer, or @p def.
 *
 * Accepts decimal or `0x`-prefixed hex. Parsing is strict: the ENTIRE
 * value must be the number — a trailing unit or inline comment (e.g.
 * `30s`, `5 # note`) does NOT parse and yields @p def. Returns @p def
 * when the key is absent OR its value does not parse as a non-negative
 * integer.
 *
 * @return the parsed value, or @p def.
 */
uint64_t
axl_config_file_get_uint(
    AxlConfigFile *cf,    ///< map (NULL → returns @p def)
    const char    *key,   ///< key to look up
    uint64_t       def    ///< default if absent / unparseable
);

/**
 * @brief Get a value parsed as a signed integer, or @p def.
 *
 * Accepts an optional leading `-`/`+`, then decimal or `0x` hex. Parsing
 * is strict (the whole value must be the number; trailing characters
 * yield @p def). Returns @p def when the key is absent OR its value does
 * not parse.
 *
 * @return the parsed value, or @p def.
 */
int64_t
axl_config_file_get_int(
    AxlConfigFile *cf,    ///< map (NULL → returns @p def)
    const char    *key,   ///< key to look up
    int64_t        def    ///< default if absent / unparseable
);

/**
 * @brief Get a value parsed as a boolean, or @p def.
 *
 * Accepts (case-insensitive) `true`/`false`, `yes`/`no`, `on`/`off`,
 * `1`/`0`. Returns @p def when the key is absent OR its value is none of
 * these.
 *
 * @return the parsed boolean, or @p def.
 */
bool
axl_config_file_get_bool(
    AxlConfigFile *cf,    ///< map (NULL → returns @p def)
    const char    *key,   ///< key to look up
    bool           def    ///< default if absent / unparseable
);

/**
 * @brief Set (create or update) a key's value.
 *
 * Both @p key and @p value are copied into the map. An empty-string value
 * is allowed; setting a key that exists overwrites it.
 *
 * @return AXL_OK on success, AXL_ERR on bad arguments or OOM.
 */
int
axl_config_file_set(
    AxlConfigFile *cf,     ///< map
    const char    *key,    ///< key to set
    const char    *value   ///< value to store (copied; "" allowed)
);

/**
 * @brief Serialize the map as `key=value` text and write it to @p path.
 *
 * Writes one `key=value\n` line per entry via `<axl/axl-fs.h>`. Entry
 * order is unspecified (the map is unordered). Round-trips through
 * `axl_config_file_load`. Comments are not preserved (the load step
 * discards them).
 *
 * @return AXL_OK on success, AXL_ERR on bad arguments, OOM, or write
 *     failure.
 */
int
axl_config_file_save(
    AxlConfigFile *cf,    ///< map
    const char    *path   ///< destination file path
);

/**
 * @brief Serialize the map as `key=value` text into a caller buffer.
 *
 * Same line grammar as axl_config_file_save: one `key=value\n` line per
 * entry, entry order unspecified (the map is unordered). Round-trips
 * through axl_config_file_parse_string.
 *
 * For a caller whose config text lives somewhere other than a filesystem
 * (e.g. an NVRAM variable) -- the in-memory counterpart of
 * axl_config_file_save. Truncation is reported, not silently accepted: if
 * the serialized text (including the trailing NUL) would not fit in @p cap
 * bytes, AXL_ERR is returned and @p buf is left unmodified.
 *
 * @return AXL_OK on success (@p buf is NUL-terminated); AXL_ERR on NULL
 *     @p cf / @p buf, zero @p cap, or the text not fitting.
 */
int
axl_config_file_to_string(
    AxlConfigFile *cf,    ///< map to serialize
    char          *buf,   ///< [out] destination buffer
    size_t         cap    ///< capacity of @p buf in bytes
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONFIG_FILE_H */
