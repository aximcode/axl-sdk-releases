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
 *
 * A second grammar, OpenSSH `sshd_config(5)` (`Keyword args` or
 * `Keyword=args`, quoting, case-insensitive keywords, first value wins), is
 * opt-in per map via axl_config_file_new_syntax(#AXL_CONFIG_FILE_OPENSSH).
 * Such a map also records every directive in file order with its line
 * number (axl_config_file_entry_at()), so a caller can validate each
 * occurrence and name the line it refuses. axl_config_file_read() and
 * axl_config_file_parse_bytes() report a missing file, an unreadable one
 * and a syntax error as distinct outcomes. Everything above describes the
 * default key=value grammar, which those additions leave unchanged.
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
 *        `axl_config_file_load` / `_new` / `_new_syntax`, free with `_free`.
 */
typedef struct AxlConfigFile AxlConfigFile;

/**
 * @brief Line-length cap shared by both grammars, in bytes.
 *
 * A line of this many bytes or more, not counting its LF or CRLF
 * terminator, is too long: the key=value grammar skips it, the OpenSSH
 * grammar refuses it ("line too long"). For OpenSSH input in UTF-16LE the
 * count is of the line after conversion to UTF-8.
 */
#define AXL_CONFIG_FILE_LINE_MAX  1024u

/**
 * @brief The line grammar every parse into a map uses; fixed at creation.
 */
typedef enum {
    AXL_CONFIG_FILE_KEY_VALUE = 0, ///< `key=value`, as described in the file comment (the default)
    AXL_CONFIG_FILE_OPENSSH   = 1, ///< OpenSSH sshd_config(5): `Keyword args` or `Keyword=args`
} AxlConfigFileSyntax;

/**
 * @brief One directive of an OpenSSH-syntax parse, in file order.
 *
 * Owned by the map. Repeated keywords are kept, each with its own line, so
 * a caller can apply OpenSSH's rules (first value wins; every occurrence
 * validated). axl_config_file_entry_at() states how long the struct and
 * the strings it points to stay valid -- they differ.
 */
typedef struct {
    const char        *keyword; ///< as written, case kept; compare with axl_strcasecmp(). Never NULL or empty
    size_t             argc;    ///< arguments after the keyword; 0 when the line has none
    const char *const *argv;    ///< @a argc arguments, quotes and escapes removed; each non-NULL, possibly "" (from `""`)
    size_t             line;    ///< 1-based line number within the parse that produced it
} AxlConfigFileEntry;

/**
 * @brief Where an OpenSSH-syntax parse stopped, and why.
 *
 * Every outcome other than a syntax error -- success, a missing or
 * unreadable file, NULL or empty arguments, OOM -- leaves @a line 0 and
 * @a reason "", so `line != 0` alone tells a syntax error apart from the
 * other #AXL_INVALID causes.
 */
typedef struct {
    size_t      line;   ///< 1-based line of the first error; 0 when there was none
    const char *reason; ///< static string, never NULL: "unterminated quoted string",
                        ///< "line too long", "missing keyword", "invalid UTF-16" or
                        ///< "NUL byte"; "" when @a line is 0
} AxlConfigFileError;

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
 * The map this returns always uses the key=value grammar. For OpenSSH
 * syntax, or to tell a missing file from an unreadable one, create the map
 * with axl_config_file_new_syntax() and fill it with axl_config_file_read().
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
 * On a map created with #AXL_CONFIG_FILE_OPENSSH this applies that grammar
 * instead, all or nothing exactly as axl_config_file_parse_bytes() does,
 * and returns AXL_ERR on a syntax error or OOM -- without saying which or
 * where; a caller that must report the error uses
 * axl_config_file_parse_bytes(). Being NUL-terminated, @p text cannot
 * carry UTF-16: it is taken as UTF-8 (a leading BOM dropped), and UTF-16
 * input needs axl_config_file_parse_bytes().
 *
 * @return AXL_OK on success, AXL_ERR on NULL @p cf / @p text, or, on an
 *     OpenSSH map only, a syntax error or OOM.
 */
int
axl_config_file_parse_string(
    AxlConfigFile *cf,     ///< map to parse into
    const char    *text    ///< key=value text (NUL-terminated, '\n'-separated lines)
);

/**
 * @brief Create an empty map (set-only / all-defaults).
 *
 * Same as `axl_config_file_new_syntax(AXL_CONFIG_FILE_KEY_VALUE)`.
 *
 * @return new empty map, or NULL on OOM.
 */
AxlConfigFile *
axl_config_file_new(void);

/**
 * @brief Create an empty map that parses with @p syntax.
 *
 * `axl_config_file_new()` is `axl_config_file_new_syntax(AXL_CONFIG_FILE_KEY_VALUE)`,
 * and a key=value map behaves exactly as the rest of this header describes:
 * no encoding detection, no BOM handling, no entries.
 *
 * **OpenSSH syntax, per line.** The tokenizer follows OpenSSH 9.9p1's
 * `argv_split()` with comments on:
 * - leading spaces and tabs are skipped; an empty line, a whitespace-only
 *   line and a line whose first non-blank byte is `#` are ignored;
 * - the keyword is the first token, quoted and escaped like an argument
 *   (below), ending at an unquoted space, tab or `=` -- so `"Port" 22` is
 *   `Port`. One optional `=`, with spaces or tabs on either side,
 *   separates it from the arguments. An empty keyword (`=x`, or `""`) is
 *   "missing keyword";
 * - the rest splits into arguments on spaces and tabs. A double or single
 *   quote opens a group that runs to the matching quote, may start or end
 *   mid-argument, and is removed (`""` is one empty argument); an unclosed
 *   one is "unterminated quoted string". A backslash before a double
 *   quote, a single quote or another backslash (anywhere), or before a
 *   space (outside quotes), yields that character; any other backslash is
 *   literal, so `fs0:\keys` stays as written. A `#` at the start of an
 *   argument outside quotes ends the line; elsewhere (`22#x`) it is an
 *   ordinary character;
 * - lines end in LF or CRLF; a CR anywhere else is whitespace. A line of
 *   #AXL_CONFIG_FILE_LINE_MAX bytes or more is "line too long".
 *
 * **Encodings** (OpenSSH maps only). Input starting with `FF FE` is
 * UTF-16LE and is converted to UTF-8 before any of the above; an odd byte
 * count or an unpaired surrogate is "invalid UTF-16" on the line it falls
 * on. Otherwise input is UTF-8 and a leading `EF BB BF` is dropped. A NUL
 * byte in UTF-8 input, or an encoded U+0000 in UTF-16LE, is "NUL byte" on
 * the line it falls on -- refused, never read as whitespace or an end of
 * text. Line numbers count the converted text. UTF-8 is NOT validated
 * (OpenSSH does not validate it either); UTF-16BE and BOM-less UTF-16 are
 * not detected. There is no size cap beyond memory: a file too large to
 * hold is #AXL_NO_RESOURCES.
 *
 * **All or nothing.** A parse that hits a syntax error changes nothing in
 * the map -- neither entries nor values -- and reports the FIRST error's
 * line. Nothing is applied until the whole input has tokenized.
 *
 * **The map** holds, per keyword compared case-insensitively, the FIRST
 * occurrence's arguments joined by one space ("" for a keyword with none),
 * stored under the keyword as first written -- so the typed getters read
 * OpenSSH's first-wins value. First wins across parses too: a keyword
 * already in the map, from an earlier parse or axl_config_file_set(), is
 * not overwritten by a parse. Joining loses argument boundaries
 * (`"a b" c` reads back as `a b c`); a caller that needs them reads the
 * entries.
 *
 * @return new empty map, or NULL on OOM or an unknown @p syntax.
 */
AxlConfigFile *
axl_config_file_new_syntax(
    AxlConfigFileSyntax syntax   ///< grammar for every parse into this map
);

/**
 * @brief Read @p path into @p cf with its grammar, reporting what went wrong.
 *
 * The counterpart of axl_config_file_load() for a caller that must tell a
 * missing file from an unreadable or malformed one, and that works on a map
 * the caller created (so it may be an OpenSSH map). Entries and map values
 * from earlier parses are kept.
 *
 * **Missing versus unreadable.** #AXL_NOT_FOUND means only that the volume
 * resolved and the file system reported no such file (a missing directory
 * on the way included). Everything else that stops the read is
 * #AXL_IO_ERROR: an unknown or unmapped volume, a directory at @p path, an
 * open or read failure. The split is deliberately conservative -- a caller
 * may treat a missing file as "use the defaults", and an unreadable one
 * must not reach that branch.
 *
 * A syntax error changes nothing in @p cf (see axl_config_file_new_syntax(),
 * "All or nothing") and @p err names the line. A key=value map never
 * reports one: its grammar skips what it cannot read, exactly as
 * axl_config_file_load() does.
 *
 * @return #AXL_OK; #AXL_NOT_FOUND as above (@p cf unchanged);
 *     #AXL_IO_ERROR as above (@p cf unchanged); #AXL_INVALID on a NULL
 *     @p cf, a NULL or empty ("") @p path (@p err line 0), or a syntax
 *     error (@p err filled); #AXL_NO_RESOURCES on OOM, after which the
 *     entries are unchanged but the map may hold some of this parse's
 *     first-occurrence values.
 */
AXL_WARN_UNUSED AxlStatus
axl_config_file_read(
    AxlConfigFile      *cf,    ///< map to parse into
    const char         *path,  ///< file to read
    AxlConfigFileError *err    ///< [out] the error, or NULL; line 0 on success
);

/**
 * @brief Parse @p len bytes already in memory into @p cf with its grammar.
 *
 * Same grammar, encodings and error contract as axl_config_file_read(),
 * minus the file. @p data need not be NUL-terminated (UTF-16 input holds
 * NUL bytes); @p len == 0 is an empty parse and returns #AXL_OK. On a
 * key=value map the bytes are parsed as axl_config_file_load() parses file
 * bytes.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL @p cf, a NULL @p data with a
 *     non-zero @p len (@p err line 0), or a syntax error (@p err filled);
 *     #AXL_NO_RESOURCES on OOM, with the same caveat as
 *     axl_config_file_read().
 */
AXL_WARN_UNUSED AxlStatus
axl_config_file_parse_bytes(
    AxlConfigFile      *cf,    ///< map to parse into
    const void         *data,  ///< file bytes
    size_t              len,   ///< byte count
    AxlConfigFileError *err    ///< [out] the error, or NULL; line 0 on success
);

/**
 * @brief Number of entries recorded by OpenSSH-syntax parses into @p cf,
 *     summed over every successful parse (0 for a key=value map, whose
 *     grammar keeps no order). axl_config_file_set() adds no entry.
 *
 * @return the entry count; 0 for a NULL @p cf.
 */
size_t
axl_config_file_entry_count(
    AxlConfigFile *cf   ///< map (NULL -> 0)
);

/**
 * @brief The @p index-th entry in parse order.
 *
 * Entries from successive parses follow one another, and each parse
 * restarts its @a line numbering at 1. A caller parsing several sources
 * into one map records axl_config_file_entry_count() before each parse to
 * know which index range came from which source.
 *
 * **Lifetimes differ.** The returned #AxlConfigFileEntry pointer is valid
 * until the next parse into @p cf -- successful or not -- or
 * axl_config_file_free(). The strings it points to (@a keyword, the
 * @a argv array and each @a argv element) belong to @p cf and stay valid
 * until axl_config_file_free(). axl_config_file_set() invalidates neither.
 *
 * @return the entry, or NULL for a NULL @p cf or when @p index >=
 *     axl_config_file_entry_count().
 */
const AxlConfigFileEntry *
axl_config_file_entry_at(
    AxlConfigFile *cf,     ///< map
    size_t         index   ///< 0-based
);

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
 * Keys compare case-sensitively on a key=value map and case-insensitively
 * on an OpenSSH map (where the value is the first occurrence's arguments,
 * joined by one space). The same holds for every typed getter below.
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
 * Deliberately lenient, on an OpenSSH map too: sshd_config(5) accepts only
 * `yes`/`no`, so a caller enforcing that validates the entries
 * (axl_config_file_entry_at()) rather than relying on this getter.
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
 * is allowed; setting a key that exists overwrites it. On an OpenSSH map
 * "exists" compares case-insensitively, the overwritten key keeps its
 * stored spelling, and the recorded entries are not touched.
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
 * The serializer writes key=value only, so on an OpenSSH map this writes
 * nothing and returns AXL_ERR.
 *
 * @return AXL_OK on success, AXL_ERR on bad arguments, an OpenSSH map,
 *     OOM, or write failure.
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
 * On an OpenSSH map this returns AXL_ERR and leaves @p buf unmodified:
 * the serializer writes key=value only.
 *
 * @return AXL_OK on success (@p buf is NUL-terminated); AXL_ERR on NULL
 *     @p cf / @p buf, zero @p cap, an OpenSSH map, or the text not
 *     fitting.
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
