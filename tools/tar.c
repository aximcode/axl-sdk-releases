/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file tar.c
    Minimal ustar archive tool (UEFI tar(1) equivalent), built on the
    AxlTar codec (`<axl/axl-tar.h>`).

    Usage:
      tar -c ARCHIVE FILE...      create ARCHIVE from FILE... (dirs recurse)
      tar -t ARCHIVE              list ARCHIVE contents
      tar -x ARCHIVE [-C DIR]     extract ARCHIVE (into DIR, default cwd)
      add -v for verbose (print each member).
      add -z to gzip on create; list/extract auto-detect gzip (1f 8b).

    Ctrl-C during -x stops it, between members or part-way through one:
    the member being written is discarded (its temporary removed, any
    file it would have replaced unchanged), "tar: interrupted" is printed,
    and the exit status is 1.

    The archives are standard POSIX ustar — interoperable with GNU/BSD
    tar for the common case (regular files + directories, names within
    the ustar name/prefix limit). List and extract also read the long
    names GNU tar (`L`) and pax (`x` path/size) write; links and other
    member types are reported and skipped, never written as files.
    Sparse files are not consumed.

    Member names are stored as GNU tar and bsdtar store them: relative,
    '/'-separated, the volume prefix dropped -- `tar -c a.tar FS1:\src`
    writes `src/`, `src/probe.txt`, ... A member ustar cannot hold whole
    (a component over 100 bytes, or a path no name/prefix split fits) is
    reported and skipped, never cut, and the exit status is non-zero.

    Build with axl-cc:
      axl-cc tar.c -o tar.efi
**/

#include <axl.h>
#include <axl/axl-fs.h>
#include <axl/axl-stream.h>
#include <axl/axl-tar.h>
#include <axl/axl-compress.h>

AXL_LOG_DOMAIN("tar");

/// Recursion cap for directory creation — matches AxlTar's practical
/// name-depth ceiling; deeper trees are rejected by the name/prefix split.
#define TAR_WALK_MAX_DEPTH  256

/// Attempts at a free temporary name before giving up on a member.
#define TAR_TEMP_TRIES  16

/// Longest AXL_TAR_TEST_TEMP the test seam accepts (a FAT leaf is 255
/// UTF-16 units; this is far below that and far above any test name).
#define TAR_TEST_TEMP_MAX  64

/// Extraction result of one member: written, failed, or stopped by Ctrl-C
/// (the whole extract stops).
typedef enum {
    EXTRACT_OK = 0,
    EXTRACT_FAILED,
    EXTRACT_INTERRUPTED,
} ExtractResult;

// ---------------------------------------------------------------------------
// Args
// ---------------------------------------------------------------------------

static const AxlArgDesc flags[] = {
    { .name = "create",  .short_name = 'c', .type = AXL_ARG_BOOL,
      .help = "Create an archive from the given files" },
    { .name = "list",    .short_name = 't', .type = AXL_ARG_BOOL,
      .help = "List the contents of an archive" },
    { .name = "extract", .short_name = 'x', .type = AXL_ARG_BOOL,
      .help = "Extract an archive" },
    { .name = "dir",     .short_name = 'C', .type = AXL_ARG_STRING,
      .help = "Extract into this directory (default: current directory)" },
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "List each member as it is processed" },
    { .name = "gzip",    .short_name = 'z', .type = AXL_ARG_BOOL,
      .help = "Compress (-c) / force-decompress (-t,-x) with gzip; "
              "list/extract auto-detect gzip regardless" },
    { .name = "file",    .short_name = 'f', .type = AXL_ARG_STRING,
      .help = "Archive file (GNU/BSD tar -f; alternative to the positional). "
              "With -f the leading positional is treated as a member, not the "
              "archive, so `tar -cf a.tar x y` works" },
    {0}
};

static const AxlArgDesc positional[] = {
    /* Not required: the archive may instead come from -f (GNU-tar style). */
    { .name = "archive", .type = AXL_ARG_STRING,
      .help = "Archive file path (or use -f)" },
    { .name = "files",   .type = AXL_ARG_MULTI,
      .help = "Files / directories to archive (with -c)" },
    {0}
};

// ---------------------------------------------------------------------------
// Member names
// ---------------------------------------------------------------------------

/* Skip a leading volume prefix ("FS1:", "C:") -- a ':' before any
   separator -- and the separators after it. */
static const char *
skip_volume(
    const char  *path
    )
{
    const char *p = path;
    for (const char *q = path; *q != '\0' && *q != '/' && *q != '\\'; q++) {
        if (*q == ':') { p = q + 1; }
    }
    while (*p == '/' || *p == '\\') { p++; }
    return p;
}

/* True if @p s has a ".." component; '/' and '\' both delimit one. */
static bool
has_dotdot(
    const char  *s
    )
{
    size_t n = axl_strlen(s);
    for (size_t i = 0; i + 1 < n; i++) {
        bool at_start = (i == 0) || s[i - 1] == '/' || s[i - 1] == '\\';
        bool at_end   = (i + 2 == n) || s[i + 2] == '/' || s[i + 2] == '\\';
        if (at_start && s[i] == '.' && s[i + 1] == '.' && at_end) {
            return true;
        }
    }
    return false;
}

/* The archive member name for UEFI path @p path, as GNU tar and bsdtar
   store one: the volume prefix and leading separators dropped and '\'
   mapped to '/', so host tar extracts it into a relative tree (and
   AxlTar's name/prefix split, which splits at '/', can place a long one).
   A volume root becomes ".". The inverse of sanitize_name. Returns 0, or
   -1 when the name does not fit @p cap whole or has a ".." component
   (no tar would extract it) -- never a cut name. */
static int
member_name(
    const char  *path,
    char        *out,
    size_t       cap
    )
{
    const char *p = skip_volume(path);
    if (*p == '\0') { p = "."; }
    size_t n = axl_strlen(p);
    if (n + 1 > cap || has_dotdot(p)) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (p[i] == '\\') ? '/' : p[i];
    }
    out[n] = '\0';
    return 0;
}

// ---------------------------------------------------------------------------
// Create
// ---------------------------------------------------------------------------

typedef struct {
    AxlTarWriter  *w;
    bool           verbose;
    int            err;
} CreateCtx;

/* Archive the file, or the directory header, for @p path. Messages name
   the UEFI path the user gave. Returns 0, or -1 with the member reported. */
static int
add_one(
    AxlTarWriter  *w,
    const char    *path,
    bool           is_dir,
    bool           verbose
    )
{
    char name[AXL_TAR_NAME_MAX];
    if (is_dir) {
        if (member_name(path, name, sizeof name) != 0
            || axl_tar_writer_add_dir(w, name, 0755) != AXL_OK) {
            axl_printerr("tar: cannot archive dir %s\n", path);
            return -1;
        }
        if (verbose) { axl_printf("%s/\n", path); }
        return 0;
    }
    void  *buf = NULL;
    size_t len = 0;
    if (axl_file_get_contents(path, &buf, &len) != AXL_OK) {
        axl_printerr("tar: cannot read %s\n", path);
        return -1;
    }
    int rc = AXL_ERR;
    if (member_name(path, name, sizeof name) == 0) {
        rc = axl_tar_writer_add(w, name, 0644, buf, len);
    }
    axl_free(buf);
    if (rc != AXL_OK) {
        axl_printerr("tar: cannot archive %s (name too long?)\n", path);
        return -1;
    }
    if (verbose) { axl_printf("%s\n", path); }
    return 0;
}

/* Per-entry callback for the recursive create walk. axl_dir_walk now
   composes each child path with the root's own separator, so the paths
   reopen on strict UEFI volumes.

   A member that cannot be archived is reported and the walk goes on, as
   GNU tar goes on: the error is remembered in c->err and becomes the exit
   status. Returning non-zero would stop the whole walk -- axl_dir_walk
   returns a callback's verdict from any depth. */
static int
create_cb(
    const char        *full_path,
    const AxlFsEntry  *entry,
    void              *user
    )
{
    CreateCtx *c = (CreateCtx *)user;
    if (add_one(c->w, full_path, axl_fs_entry_is_dir(entry), c->verbose) != 0) {
        c->err = 1;
    }
    return 0;
}

/* Archive one path (file or directory tree) into @p w. Returns 0 on success,
   1 on any error (with a message already printed). */
static int
tar_add_path(
    AxlTarWriter *w,
    const char   *path,
    bool          verbose
    )
{
    if (path == NULL) {
        return 0;
    }
    if (axl_file_is_dir(path)) {
        /* Recurse — a dir entry for the root, then each descendant.
           axl_dir_walk composes child paths with the root's own
           separator (UEFI-native), so they reopen on strict volumes. */
        CreateCtx ctx = { .w = w, .verbose = verbose, .err = 0 };
        if (add_one(w, path, true, verbose) != 0) {
            return 1;
        }
        axl_dir_walk(path, create_cb, &ctx, TAR_WALK_MAX_DEPTH);
        return ctx.err ? 1 : 0;
    }
    return (add_one(w, path, false, verbose) != 0) ? 1 : 0;
}

static int
do_create(
    AxlArgs     *a,
    const char  *archive,
    const char  *lead_file,   /* extra leading member (the "archive" positional
                                 when -f named the archive), or NULL */
    bool         verbose,
    bool         gzip
    )
{
    /* The named "archive" positional is fetched by name; get_pos indexes
       only the variadic "files" tail. With -f, lead_file carries the first
       positional (which is a member, not the archive). */
    int pos_count = axl_args_get_pos_count(a);
    if (pos_count < 1 && lead_file == NULL) {
        axl_printerr("tar: -c needs at least one file\n");
        return 1;
    }

    AxlStream *out = axl_fopen(archive, "w");
    if (out == NULL) {
        axl_printerr("tar: cannot create %s\n", archive);
        return 1;
    }
    /* With -z, the ustar bytes flow through a gzip writer into the file;
       it must be finalized after the tar writer to flush the trailer. */
    AxlStream *gz = NULL;
    if (gzip) {
        gz = axl_gzip_writer(out, AXL_COMPRESS_LEVEL_DEFAULT);
        if (gz == NULL) {
            axl_fclose(out);
            axl_printerr("tar: out of memory\n");
            return 1;
        }
    }
    AxlStream    *sink = gzip ? gz : out;
    AxlTarWriter *w    = axl_tar_writer_new(sink);
    if (w == NULL) {
        axl_fclose(gz);
        axl_fclose(out);
        axl_printerr("tar: out of memory\n");
        return 1;
    }

    int rc = 0;
    if (tar_add_path(w, lead_file, verbose) != 0) {
        rc = 1;
    }
    for (int i = 0; i < pos_count; i++) {
        if (tar_add_path(w, axl_args_get_pos(a, i), verbose) != 0) {
            rc = 1;
        }
    }

    if (axl_tar_writer_finish(w) != AXL_OK) {
        axl_printerr("tar: write %s failed\n", archive);
        rc = 1;
    }
    axl_tar_writer_free(w);
    /* Flush the gzip trailer to the file and surface any compress error. */
    if (gz != NULL) {
        if (axl_compress_writer_finish(gz) != AXL_OK) {
            axl_printerr("tar: gzip %s failed\n", archive);
            rc = 1;
        }
        axl_fclose(gz);
    }
    /* Flush the archive through to the volume BEFORE reporting. axl_fclose is
       not a durability point -- it drains the AXL-side buffer and never calls
       the stream's flush -- and the firmware close under it cannot report
       anything, since EFI_FILE_PROTOCOL.Close is specified to return only
       EFI_SUCCESS. Without this a full volume or write-protected media
       produced a TRUNCATED ARCHIVE announced as a successful create, which is
       the worst answer available: the operator moves on and discovers it when
       they try to extract. Runs after the gzip finish above, which pushes the
       trailer into this stream. The partial archive is left on the volume
       rather than deleted, matching every other failure path here -- a
       diagnosable artifact beats a vanished one. */
    if (axl_fflush(out) != AXL_OK) {
        axl_printerr("tar: flush %s failed (archive did not reach the "
                     "volume)\n", archive);
        rc = 1;
    }
    axl_fclose(out);
    return rc;
}

// ---------------------------------------------------------------------------
// Archive open for reading (with transparent gzip)
// ---------------------------------------------------------------------------

/* Open @p archive for reading and, if it is gzip (forced by @p gzip or
   auto-detected via the 1f 8b magic), wrap it in a decompressing reader
   so list/extract transparently handle .tar.gz.

   On success returns the stream to feed the tar reader and sets @p *raw
   to the underlying file stream. When the archive is gzipped these are
   two different streams (both must be closed); otherwise they are the
   same (close once — guard with `reader != raw`). On failure returns
   NULL; @p *raw is still set if the file opened, so the caller can
   close it. */
static AxlStream *
open_archive_read(
    const char  *archive,
    bool         gzip,
    AxlStream  **raw
    )
{
    *raw = NULL;
    AxlStream *in = axl_fopen(archive, "r");
    if (in == NULL) {
        axl_printerr("tar: cannot open %s\n", archive);
        return NULL;
    }
    *raw = in;

    if (!gzip) {
        /* Non-consuming peek (pread leaves the position at 0). */
        uint8_t magic[2];
        if (axl_pread(in, magic, sizeof magic, 0) == (axl_ssize_t)sizeof magic
            && magic[0] == 0x1f && magic[1] == 0x8b) {
            gzip = true;
        }
    }
    if (!gzip) {
        return in;
    }

    AxlStream *gz = axl_gzip_reader(in);
    if (gz == NULL) {
        axl_printerr("tar: %s is not a valid gzip archive\n", archive);
        return NULL;  /* caller closes *raw */
    }
    return gz;
}

// ---------------------------------------------------------------------------
// Member types
// ---------------------------------------------------------------------------

/* True (with the member reported) for an entry list and extract both pass
   over: a name the reader refused, a pax global header (noted once, not
   an error), a link, or a type tar.efi cannot write. A regular file
   ('0', '7') or directory is false. The reader steps over the skipped
   member's data on its next call. */
static bool
member_skipped(
    const AxlTarEntry  *e,
    bool               *noted_global
    )
{
    if (e->type == AXL_TAR_TYPE_GLOBAL) {
        if (!*noted_global) {
            axl_printerr("tar: ignoring pax global header\n");
            *noted_global = true;
        }
        return true;
    }
    if (e->name_refused) {
        axl_printerr("tar: skipping member with an over-long or malformed "
                     "name\n");
        return true;
    }
    if (e->type == '1' || e->type == '2') {
        axl_printerr("tar: %s: not extracted (link)\n", e->name);
        return true;
    }
    if (e->type != AXL_TAR_TYPE_FILE && e->type != '7'
        && e->type != AXL_TAR_TYPE_DIR) {
        axl_printerr("tar: %s: unsupported member type '%c', skipped\n",
                     e->name, e->type);
        return true;
    }
    return false;
}

/* Report how reading the archive stopped -- @p st, the last
   axl_tar_reader_next() status. Returns 0 for a real end (or a loop the
   caller left early, AXL_OK), 1 for a truncated, damaged or unreadable
   archive: an archive that stops early must never read as a whole one. */
static int
report_archive_end(
    int  st
    )
{
    switch (st) {
    case AXL_OK:
    case AXL_ERR:
        return 0;
    case AXL_INCOMPLETE:
        axl_printerr("tar: unexpected end of archive\n");
        return 1;
    case AXL_INVALID:
        axl_printerr("tar: damaged archive: a header failed its checksum\n");
        return 1;
    default:
        axl_printerr("tar: error reading the archive\n");
        return 1;
    }
}

// ---------------------------------------------------------------------------
// List
// ---------------------------------------------------------------------------

static int
do_list(
    const char  *archive,
    bool         verbose,
    bool         gzip
    )
{
    AxlStream *raw = NULL;
    AxlStream *in  = open_archive_read(archive, gzip, &raw);
    if (in == NULL) {
        axl_fclose(raw);
        return 1;
    }
    AxlTarReader *r = axl_tar_reader_new(in);
    if (r == NULL) {
        if (in != raw) { axl_fclose(in); }
        axl_fclose(raw);
        axl_printerr("tar: out of memory\n");
        return 1;
    }
    int         rc = 0;
    bool        noted_global = false;
    AxlTarEntry e;
    int         st;
    while ((st = axl_tar_reader_next(r, &e)) == AXL_OK) {
        if (member_skipped(&e, &noted_global)) {
            if (e.type != AXL_TAR_TYPE_GLOBAL) { rc = 1; }
            continue;
        }
        if (verbose) {
            axl_printf("%c %10llu  %s\n",
                       (e.type == AXL_TAR_TYPE_DIR) ? 'd' : '-',
                       (unsigned long long)e.size, e.name);
        } else {
            axl_printf("%s\n", e.name);
        }
    }
    if (report_archive_end(st) != 0) { rc = 1; }
    axl_tar_reader_free(r);
    if (in != raw) { axl_fclose(in); }
    axl_fclose(raw);
    return rc;
}

// ---------------------------------------------------------------------------
// Extract
// ---------------------------------------------------------------------------

/* Create every parent directory of @p path (best-effort; existing dirs
   are fine). @p path is a full filesystem path. */
static void
mkdir_parents(
    const char  *path
    )
{
    char *tmp = axl_strdup(path);
    if (tmp == NULL) { return; }
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p = '\0';
            axl_dir_mkdir(tmp);
            *p = sep;
        }
    }
    axl_free(tmp);
}

/* A temporary sibling of @p full -- "<its directory>~TARhhhhhhhh.TMP",
   or "<its directory><first>" on the first try when the test seam names
   one -- that names nothing yet, so it collides neither with a file
   already there nor with a member extracted earlier. A later member of
   the same name is no hazard: the temporary is renamed away before the
   next member is read. (A member that is the temporary's 8.3 ALIAS is,
   and open_temp() checks for it.) Returns NULL when no free name turns
   up or memory runs out. */
/* The final component of @p path: what follows its last '\', '/' or ':'. */
static const char *
path_leaf(
    const char  *path
    )
{
    const char *leaf = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/' || *p == ':') { leaf = p + 1; }
    }
    return leaf;
}

/* The words for why axl_file_replace() could not put a member in place. */
static const char *
replace_failure(
    int  st
    )
{
    switch (st) {
    case AXL_IS_DIR:
        return "a directory is in the way";
    case AXL_DENIED:
        return "the existing file or the volume is read-only";
    case AXL_NO_RESOURCES:
        return "the volume is too full to put it in place";
    case AXL_NOT_DIR:
        return "a file is in the way of its directory";
    case AXL_NOT_FOUND:
        return "its directory is gone";
    default:
        return "it could not be renamed into place";
    }
}

static char *
temp_sibling(
    const char  *full,
    const char  *first
    )
{
    size_t dir_len = (size_t)(path_leaf(full) - full);
    for (int i = 0; i < TAR_TEMP_TRIES; i++) {
        char *t = (i == 0 && first != NULL)
            ? axl_asprintf("%.*s%s", (int)dir_len, full, first)
            : axl_asprintf("%.*s~TAR%08X.TMP", (int)dir_len, full,
                           (unsigned)axl_random_uint32());
        if (t == NULL) { return NULL; }
        AxlFsEntry info;
        if (axl_file_info(t, &info) != AXL_OK) { return t; }
        axl_free(t);
    }
    return NULL;
}

/* Remove the temporary in @p *slot, if any, and clear the slot. Says so
   when it cannot be removed: a stray ~TAR*.TMP must not go unexplained. */
static void
discard_temp(
    char  **slot
    )
{
    if (*slot == NULL) { return; }
    if (axl_file_delete(*slot) != AXL_OK) {
        axl_printerr("tar: temporary file %s could not be removed\n", *slot);
    }
    axl_free(*slot);
    *slot = NULL;
}

/* Ctrl-C handler for -x: nothing to do at interrupt time -- the runtime
   has set the flag axl_interrupted() reads, and extraction polls it.
   Installing it stops the runtime's default exit-at-next-yield. */
static void
on_interrupt(void)
{
}

/* axl_atexit hook: a Ctrl-C (or any exit) while a member is being written
   removes its temporary. @p data is the extract's temp slot. */
static void
discard_temp_at_exit(
    void  *data
    )
{
    discard_temp((char **)data);
}

/* True if @p full now reaches the temporary @p temp: its stored name is
   the temporary's. On FAT a fresh temporary ~TAR1234ABCD.TMP also answers
   to its 8.3 alias (~TAR12~1.TMP, or a longer numeric tail), so a member
   whose own name is that alias resolves to the temporary -- and putting
   the temporary in place would rename it onto itself, leaving the data
   under the temporary's name. Asked of the volume, not of the spelling,
   so it holds for every alias shape and every driver, and refuses no
   name that is merely LIKE tar's. */
static bool
is_own_alias(
    const char  *full,
    const char  *temp
    )
{
    AxlStream *t = axl_fopen(full, "r");
    if (t == NULL) {
        return false;   /* nothing there yet: no alias of the temporary */
    }
    AxlFsEntry e;
    bool same = axl_fstat(t, &e) == AXL_OK
                && axl_strcasecmp(e.name, path_leaf(temp)) == 0;
    axl_fclose(t);
    return same;
}

/* The test seam's value, if it is a plain leaf name -- non-empty, at
   most TAR_TEST_TEMP_MAX bytes, no '\\', '/' or ':', and not "." or
   ".." -- else NULL, with one warning: a value with a separator, a
   volume or a dot-dot would put the temporary outside the member's
   directory. Takes ownership of @p v. */
static char *
test_temp_leaf(
    char  *v
    )
{
    if (v == NULL) { return NULL; }
    size_t n  = axl_strlen(v);
    bool   ok = n > 0 && n <= TAR_TEST_TEMP_MAX
                && axl_strcmp(v, ".") != 0 && axl_strcmp(v, "..") != 0;
    for (size_t i = 0; ok && i < n; i++) {
        ok = v[i] != '\\' && v[i] != '/' && v[i] != ':';
    }
    if (!ok) {
        axl_warning("AXL_TAR_TEST_TEMP '%s' is not a plain file name; ignored",
                    v);
        axl_free(v);
        return NULL;
    }
    return v;
}

/* Create and open, for writing, the temporary of member @p full, held in
   @p *slot. One whose creation makes @p full resolve to it (is_own_alias)
   is removed and another tried, up to TAR_TEMP_TRIES. NULL, with the
   member reported and @p *slot clear, when none can be had. */
static AxlStream *
open_temp(
    const char   *full,
    char        **slot,
    const char   *first_temp
    )
{
    for (int i = 0; i < TAR_TEMP_TRIES; i++) {
        *slot = temp_sibling(full, (i == 0) ? first_temp : NULL);
        AxlStream *of = (*slot != NULL) ? axl_fopen(*slot, "w") : NULL;
        if (of == NULL) {
            axl_printerr("tar: %s: not extracted (cannot create a temporary "
                         "file beside it)\n", full);
            discard_temp(slot);
            return NULL;
        }
        if (!is_own_alias(full, *slot)) {
            return of;
        }
        axl_debug("temporary %s is the member's own name; retrying",
                  *slot);
        axl_fclose(of);
        discard_temp(slot);
    }
    axl_printerr("tar: %s: not extracted (every temporary tried was another "
                 "name of it)\n", full);
    return NULL;
}

/* Extract one regular-file member to @p full with SAFE WRITES, after
   libarchive's ARCHIVE_EXTRACT_SAFE_WRITES: the data goes to a temporary
   file in the same directory, which is flushed and then put in place by
   axl_file_replace() -- one rename on one volume. A member whose data is
   cut short or unreadable, whose write or flush fails, or that cannot be
   put in place therefore never touches a file already at @p full: the
   temporary is removed, the member is reported, and the result is
   EXTRACT_FAILED. @p slot holds the temporary while it exists, for the
   atexit hook. */
static ExtractResult
extract_file(
    AxlTarReader  *r,
    const char    *full,
    char         **slot,
    const char    *first_temp,
    bool           verbose
    )
{
    mkdir_parents(full);
    AxlStream *of = open_temp(full, slot, first_temp);
    if (of == NULL) {
        return EXTRACT_FAILED;
    }
    char        buf[4096];
    axl_ssize_t n;
    bool        ok = true;
    bool        interrupted = false;
    while ((n = axl_tar_reader_read(r, buf, sizeof buf)) > 0) {
        /* A long member stops at the next chunk, not at its end. */
        if (axl_interrupted()) {
            interrupted = true;
            ok = false;
            break;
        }
        if (axl_write(of, buf, (size_t)n) != n) {
            axl_printerr("tar: write %s failed; not extracted\n", full);
            ok = false;
            break;
        }
    }
    if (ok && n < 0) {
        /* The archive ended inside the member (the reader returns -1 when
           the stream ends with bytes still owed), or could not be read. */
        axl_printerr("tar: %s: not extracted (member data incomplete or "
                     "unreadable); any existing file is unchanged\n", full);
        ok = false;
    }
    /* Flush through to the volume and CHECK it: axl_fclose never calls the
       stream's flush, and EFI_FILE_PROTOCOL.Close reports nothing, so an
       unchecked close let a full or write-protected volume take a short
       file as a whole one. */
    if (ok && axl_fflush(of) != AXL_OK) {
        axl_printerr("tar: flush %s failed (extracted file did not reach the "
                     "volume); not extracted\n", full);
        ok = false;
    }
    axl_fclose(of);
    if (!ok) {
        discard_temp(slot);
        if (interrupted) {
            axl_printerr("tar: %s: not extracted (interrupted); any existing "
                         "file is unchanged\n", full);
            return EXTRACT_INTERRUPTED;
        }
        return EXTRACT_FAILED;
    }

    int st = axl_file_replace(*slot, full, 0, NULL);
    if (st == AXL_OK) {
        axl_free(*slot);
        *slot = NULL;
        if (verbose) { axl_printf("%s\n", full); }
        return EXTRACT_OK;
    }
    discard_temp(slot);
    if (st == AXL_CANCELLED) {
        axl_printerr("tar: %s: not extracted (interrupted); any existing "
                     "file is unchanged\n", full);
        return EXTRACT_INTERRUPTED;
    }
    axl_printerr("tar: %s: not extracted (%s); any existing file is "
                 "unchanged\n", full, replace_failure(st));
    return EXTRACT_FAILED;
}

/* Turn a tar entry name into a safe UEFI-relative path: convert '/' to
   '\' (UEFI-native), strip any leading "VOL:" prefix and leading
   separators (so extraction is relative, never absolute; archives from
   older tar.efi builds carry "FS1:\..." names), and reject ".." path
   components so a hostile archive can't escape the destination. A ':'
   left after the volume is stripped is refused too: no FAT name holds
   one, and a path parser could read it as a volume. Returns 0 with
   @p out filled, or -1 if the name is empty after stripping, does not
   fit @p outcap whole, or contains a ".." component or a ':'. */
static int
sanitize_name(
    const char  *name,
    char        *out,
    size_t       outcap
    )
{
    const char *p = skip_volume(name);
    size_t      n = axl_strlen(p);
    if (n == 0 || n + 1 > outcap || has_dotdot(p)
        || axl_strchr(p, ':') != NULL) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (p[i] == '/') ? '\\' : p[i];
    }
    out[n] = '\0';
    return 0;
}

static int
do_extract(
    const char  *archive,
    const char  *destdir,
    bool         verbose,
    bool         gzip
    )
{
    AxlStream *raw = NULL;
    AxlStream *in  = open_archive_read(archive, gzip, &raw);
    if (in == NULL) {
        axl_fclose(raw);
        return 1;
    }
    AxlTarReader *r = axl_tar_reader_new(in);
    if (r == NULL) {
        if (in != raw) { axl_fclose(in); }
        axl_fclose(raw);
        axl_printerr("tar: out of memory\n");
        return 1;
    }

    /* The temporary of the member being written, removed on any exit --
       a Ctrl-C included -- by the atexit hook, until extraction ends. */
    char    *temp      = NULL;
    uint32_t temp_hook = axl_atexit(discard_temp_at_exit, &temp);
    /* TEST SEAM: the Shell variable AXL_TAR_TEST_TEMP names the FIRST
       temporary leaf tried for each member, instead of a random one, so a
       test can make a temporary collide with a member deterministically
       (test-tar-fat-qemu.sh). It is still checked free like any other. */
    char    *first_temp = test_temp_leaf(axl_getenv("AXL_TAR_TEST_TEMP"));
    /* Own the Ctrl-C: the runtime's default would end tar at the next
       yield from wherever it is, with the runtime's exit status and no
       word from tar. tar polls axl_interrupted() instead -- per chunk and
       between members -- and stops with "tar: interrupted" and 1. */
    axl_signal_install(on_interrupt);

    int         rc = 0;
    bool        noted_global = false;
    AxlTarEntry e;
    int         st;
    bool        interrupted = false;
    while ((st = axl_tar_reader_next(r, &e)) == AXL_OK) {
        if (axl_interrupted()) {
            interrupted = true;
            break;
        }
        if (member_skipped(&e, &noted_global)) {
            if (e.type != AXL_TAR_TYPE_GLOBAL) { rc = 1; }
            continue;
        }
        char rel[AXL_TAR_NAME_MAX];
        if (sanitize_name(e.name, rel, sizeof rel) != 0) {
            axl_printerr("tar: skipping unsafe member %s\n", e.name);
            rc = 1;
            continue;
        }
        /* Join under destdir with an explicit backslash (UEFI-native) so
           the result never mixes separators on a strict volume. */
        char *full;
        if (destdir != NULL && destdir[0] != '\0') {
            size_t dl = axl_strlen(destdir);
            bool   ds = (destdir[dl - 1] == '/' || destdir[dl - 1] == '\\');
            full = axl_asprintf("%s%s%s", destdir, ds ? "" : "\\", rel);
        } else {
            full = axl_strdup(rel);
        }
        if (full == NULL) { rc = 1; break; }

        ExtractResult xr = EXTRACT_OK;
        if (e.type == AXL_TAR_TYPE_DIR) {
            mkdir_parents(full);
            axl_dir_mkdir(full);
            if (verbose) { axl_printf("%s\n", full); }
        } else {
            xr = extract_file(r, full, &temp, first_temp, verbose);
        }
        axl_free(full);
        if (xr != EXTRACT_OK) {
            rc = 1;
            if (xr == EXTRACT_INTERRUPTED) {
                interrupted = true;
                break;
            }
        }
    }
    if (interrupted) {
        axl_printerr("tar: interrupted\n");
        rc = 1;
    }
    if (report_archive_end(st) != 0) { rc = 1; }
    /* extract_file removes every temporary it creates, on every path; this
       is the backstop, and it should never find one. Say so when it does
       -- a silent sweep would hide a missing discard on a path that goes
       on to the next member, where the next temporary overwrites the slot
       and the old one is left on the volume. */
    if (temp != NULL) {
        axl_printerr("tar: internal: a temporary outlived its member: %s\n",
                     temp);
        discard_temp(&temp);
    }
    axl_atexit_remove(temp_hook);
    axl_free(first_temp);
    /* The handler stays installed: -x is the last thing tar does, and with
       it in place the runtime does not add its own "Interrupted (Ctrl-C)"
       notice at exit -- tar has already said "tar: interrupted". */
    axl_tar_reader_free(r);
    if (in != raw) { axl_fclose(in); }
    axl_fclose(raw);
    return rc;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static int
run_tar(
    AxlArgs  *a
    )
{
    bool create  = axl_args_get_bool(a, "create");
    bool list    = axl_args_get_bool(a, "list");
    bool extract = axl_args_get_bool(a, "extract");
    bool verbose = axl_args_get_bool(a, "verbose");
    bool gzip    = axl_args_get_bool(a, "gzip");

    int modes = (create ? 1 : 0) + (list ? 1 : 0) + (extract ? 1 : 0);
    if (modes != 1) {
        axl_printerr("tar: specify exactly one of -c (create), -t (list), "
                     "-x (extract)\n");
        return 1;
    }

    /* Archive source: -f <file> (GNU/BSD style) wins; else the positional.
       When -f names the archive, the leading positional is a MEMBER, not the
       archive (so `tar -cf a.tar x y` archives x and y). */
    const char *fflag       = axl_args_get_string(a, "file");
    const char *pos_archive = axl_args_get_string(a, "archive");
    const char *archive;
    const char *lead_file = NULL;
    if (fflag != NULL && fflag[0] != '\0') {
        archive   = fflag;
        lead_file = pos_archive;
    } else {
        archive = pos_archive;
    }
    if (archive == NULL || archive[0] == '\0') {
        axl_printerr("tar: no archive specified (use -f <file> or a "
                     "positional archive path)\n");
        return 1;
    }

    if (create)  { return do_create(a, archive, lead_file, verbose, gzip); }
    if (list)    { return do_list(archive, verbose, gzip); }
    return do_extract(archive, axl_args_get_string(a, "dir"), verbose, gzip);
}

AXL_TOOL_MAIN(tar)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = "tar",
        .help        = "Create, list, or extract ustar archives",
        .flags       = flags,
        .positionals = positional,
        .handler     = run_tar,
    });
}
