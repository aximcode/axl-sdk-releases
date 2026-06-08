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

    The archives are standard POSIX ustar — interoperable with GNU/BSD
    tar for the common case (regular files + directories, names within
    the ustar name/prefix limit). GNU long-name / sparse extensions are
    not produced or consumed.

    Build with axl-cc:
      axl-cc tar.c -o tar.efi
**/

#include <axl.h>
#include <axl/axl-fs.h>
#include <axl/axl-stream.h>
#include <axl/axl-tar.h>
#include <axl/axl-compress.h>

/// Recursion cap for directory creation — matches AxlTar's practical
/// name-depth ceiling; deeper trees are rejected by the name/prefix split.
#define TAR_WALK_MAX_DEPTH  256

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
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "archive", .type = AXL_ARG_STRING, .required = true,
      .help = "Archive file path" },
    { .name = "files",   .type = AXL_ARG_MULTI,
      .help = "Files / directories to archive (with -c)" },
    {0}
};

// ---------------------------------------------------------------------------
// Create
// ---------------------------------------------------------------------------

typedef struct {
    AxlTarWriter  *w;
    bool           verbose;
    int            err;
} CreateCtx;

static int
add_one_file(
    AxlTarWriter  *w,
    const char    *path,
    bool           verbose
    )
{
    void  *buf = NULL;
    size_t len = 0;
    if (axl_file_get_contents(path, &buf, &len) != AXL_OK) {
        axl_printerr("tar: cannot read %s\n", path);
        return -1;
    }
    int rc = axl_tar_writer_add(w, path, 0644, buf, len);
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
   reopen on strict UEFI volumes. */
static int
create_cb(
    const char        *full_path,
    const AxlFsEntry  *entry,
    void              *user
    )
{
    CreateCtx *c = (CreateCtx *)user;
    if (axl_fs_entry_is_dir(entry)) {
        if (axl_tar_writer_add_dir(c->w, full_path, 0755) != AXL_OK) {
            axl_printerr("tar: cannot archive dir %s\n", full_path);
            c->err = 1;
            return -1;
        }
        if (c->verbose) { axl_printf("%s/\n", full_path); }
        return 0;
    }
    if (add_one_file(c->w, full_path, c->verbose) != 0) {
        c->err = 1;
        return -1;
    }
    return 0;
}

static int
do_create(
    AxlArgs     *a,
    const char  *archive,
    bool         verbose,
    bool         gzip
    )
{
    /* The named "archive" positional is fetched by name; get_pos indexes
       only the variadic "files" tail. */
    int pos_count = axl_args_get_pos_count(a);
    if (pos_count < 1) {
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
    for (int i = 0; i < pos_count; i++) {
        const char *path = axl_args_get_pos(a, i);
        if (path == NULL) { continue; }
        if (axl_file_is_dir(path)) {
            /* Recurse — a dir entry for the root, then each descendant.
               axl_dir_walk composes child paths with the root's own
               separator (UEFI-native), so they reopen on strict volumes. */
            CreateCtx ctx = { .w = w, .verbose = verbose, .err = 0 };
            if (axl_tar_writer_add_dir(w, path, 0755) != AXL_OK) {
                axl_printerr("tar: cannot archive dir %s\n", path);
                rc = 1;
            } else {
                if (verbose) { axl_printf("%s/\n", path); }
                (void)axl_dir_walk(path, create_cb, &ctx, TAR_WALK_MAX_DEPTH);
                if (ctx.err) { rc = 1; }
            }
        } else if (add_one_file(w, path, verbose) != 0) {
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
    AxlTarEntry e;
    while (axl_tar_reader_next(r, &e) == AXL_OK) {
        if (verbose) {
            axl_printf("%c %10llu  %s\n",
                       (e.type == AXL_TAR_TYPE_DIR) ? 'd' : '-',
                       (unsigned long long)e.size, e.name);
        } else {
            axl_printf("%s\n", e.name);
        }
    }
    axl_tar_reader_free(r);
    if (in != raw) { axl_fclose(in); }
    axl_fclose(raw);
    return 0;
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
            (void)axl_dir_mkdir(tmp);
            *p = sep;
        }
    }
    axl_free(tmp);
}

static int
extract_file(
    AxlTarReader  *r,
    const char    *full,
    bool           verbose
    )
{
    mkdir_parents(full);
    AxlStream *of = axl_fopen(full, "w");
    if (of == NULL) {
        axl_printerr("tar: cannot create %s\n", full);
        return 1;
    }
    char        buf[4096];
    axl_ssize_t n;
    int         rc = 0;
    while ((n = axl_tar_reader_read(r, buf, sizeof buf)) > 0) {
        if (axl_write(of, buf, (size_t)n) != n) {
            axl_printerr("tar: write %s failed\n", full);
            rc = 1;
            break;
        }
    }
    if (n < 0) {
        axl_printerr("tar: read error extracting %s\n", full);
        rc = 1;
    }
    axl_fclose(of);
    if (verbose && rc == 0) { axl_printf("%s\n", full); }
    return rc;
}

/* Turn a tar entry name into a safe UEFI-relative path: convert '/' to
   '\' (UEFI-native), strip any leading "VOL:" prefix and leading
   separators (so extraction is relative, never absolute), and reject
   ".." path components so a hostile archive can't escape the
   destination. Returns 0 with @p out filled, or -1 if the name is empty
   after stripping or contains a ".." component. */
static int
sanitize_name(
    const char  *name,
    char        *out,
    size_t       outcap
    )
{
    /* Drop a leading volume prefix ("FS1:", "C:") up to the first ':'
       that precedes any separator. */
    const char *p = name;
    for (const char *q = name; *q != '\0' && *q != '/' && *q != '\\'; q++) {
        if (*q == ':') { p = q + 1; }
    }
    while (*p == '/' || *p == '\\') { p++; }   /* leading separators */

    size_t j = 0;
    while (*p != '\0' && j + 1 < outcap) {
        out[j++] = (*p == '/') ? '\\' : *p;
        p++;
    }
    out[j] = '\0';
    if (j == 0) { return -1; }

    /* Reject any ".." path component (directory-traversal guard). */
    for (size_t i = 0; i + 1 < j; i++) {
        bool at_start = (i == 0) || out[i - 1] == '\\';
        bool dotdot_end = (i + 2 == j) || out[i + 2] == '\\';
        if (at_start && out[i] == '.' && out[i + 1] == '.' && dotdot_end) {
            return -1;
        }
    }
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

    int         rc = 0;
    AxlTarEntry e;
    while (axl_tar_reader_next(r, &e) == AXL_OK) {
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

        if (e.type == AXL_TAR_TYPE_DIR) {
            mkdir_parents(full);
            (void)axl_dir_mkdir(full);
            if (verbose) { axl_printf("%s\n", full); }
        } else if (extract_file(r, full, verbose) != 0) {
            rc = 1;
        }
        axl_free(full);
    }
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

    const char *archive = axl_args_get_string(a, "archive");
    if (archive == NULL || archive[0] == '\0') {
        axl_printerr("tar: archive path required\n");
        return 1;
    }

    if (create)  { return do_create(a, archive, verbose, gzip); }
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
