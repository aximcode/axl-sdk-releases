/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file fwtool.c
    UEFI firmware image inspector — list/extract/find over raw .fd images.

    Built on the AxlFw library (axl/axl-fw.h): parses firmware volumes,
    FFS files, and sections from raw .fd / SPI-dump images without
    requiring a live EFI_FIRMWARE_VOLUME2_PROTOCOL.

    Build with axl-cc:
      axl-cc fwtool.c -o fwtool.efi

    Usage:
      fwtool list <image>
      fwtool extract <image> <guid> [-o outpath]
      fwtool find <image> <guid>
**/

#ifdef AXL_HOSTED

/* Host build (HOSTCC): the firmware parser + LZMA codec are backend-free,
   but fwtool's own I/O (file read/write, stdout, formatted print, GUID hex
   parse) normally goes through the UEFI/native AXL backend. Under AXL_HOSTED
   we include only the backend-free parser header and implement those few I/O
   primitives over libc below, so the command logic stays byte-for-byte shared
   with the UEFI build. See tools/fwtool-host-shim.h (force-included) for the
   allocation/memory leaf mappings. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <axl/axl-fw.h>
#include <axl/axl-sys.h>   /* AxlGuid, axl_guid_equal */

typedef long axl_ssize_t;

#ifndef AXL_OK
#define AXL_OK 0
#endif

/* Formatted output: AXL's print API maps directly onto libc stdio. The
   format strings used below (%s %zx %zu %02x %04x %08x) are all standard. */
#define axl_printf(...)   printf(__VA_ARGS__)
#define axl_printerr(...) fprintf(stderr, __VA_ARGS__)
#define axl_print(s)      fputs((s), stdout)
#define axl_snprintf      snprintf
#define axl_strcmp        strcmp

/* Raw binary stdout sink. The UEFI build guards on axl_stdout_raw being
   non-NULL (only set under the Shell); on the host it is always available. */
static void *const axl_stdout_raw = (void *)1;

/* On the host there is no busybox dispatch; the tool body is a plain main. */
#define AXL_TOOL_MAIN(name) int main(int argc, char **argv)

/** @brief Host: parse up to @p max_chars hex digits from @p s into @p out.
    Mirrors axl_hex_parse_u64's contract: returns the digit count consumed
    (>= 1) on success, or -1 if no hex digit leads or the value overflows. */
static int
axl_hex_parse_u64(
    const char *s,
    size_t      max_chars,
    uint64_t   *out
    )
{
    uint64_t v = 0;
    size_t   i = 0;
    for (; i < max_chars; i++) {
        char     c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9')      { d = (unsigned)(c - '0'); }
        else if (c >= 'a' && c <= 'f') { d = (unsigned)(c - 'a' + 10); }
        else if (c >= 'A' && c <= 'F') { d = (unsigned)(c - 'A' + 10); }
        else                           { break; }
        if (v > (UINT64_MAX >> 4)) { return -1; }   /* would overflow */
        v = (v << 4) | d;
    }
    if (i == 0) { return -1; }
    *out = v;
    return (int)i;
}

/** @brief Host: read an entire file into a freshly malloc'd buffer.
    @return AXL_OK on success (caller frees @p *buf with free/axl_free). */
static int
axl_file_get_contents(
    const char *path,
    void      **buf,
    size_t     *len
    )
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) { return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    void *p = malloc((size_t)sz ? (size_t)sz : 1);
    if (p == NULL) { fclose(f); return -1; }
    if (sz > 0 && fread(p, 1, (size_t)sz, f) != (size_t)sz) {
        free(p);
        fclose(f);
        return -1;
    }
    fclose(f);
    *buf = p;
    *len = (size_t)sz;
    return AXL_OK;
}

/** @brief Host: write a whole buffer to a file (create/overwrite). */
static int
axl_file_set_contents(
    const char *path,
    const void *buf,
    size_t      len
    )
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) { return -1; }
    if (len > 0 && fwrite(buf, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) { return -1; }
    return AXL_OK;
}

/** @brief Host: write raw bytes to binary stdout (the @p sink arg is the
    sentinel axl_stdout_raw and is ignored). */
static axl_ssize_t
axl_write(
    void       *sink,
    const void *buf,
    size_t      len
    )
{
    (void)sink;
    return (axl_ssize_t)fwrite(buf, 1, len, stdout);
}

#else  /* !AXL_HOSTED — UEFI/native build */

#include <axl.h>
#include <axl/axl-fw.h>
#include <axl/axl-fs.h>
#include <axl/axl-stream.h>
#include <axl/axl-str.h>

#endif /* AXL_HOSTED */

/* ---------------------------------------------------------------------------
 * GUID helpers
 * ---------------------------------------------------------------------------
 * There is no axl_guid_to_string / axl_guid_from_string in the public API,
 * so we provide local helpers here.  The canonical form is:
 *   XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
 * AxlGuid stores data1/data2/data3 as little-endian host integers and
 * data4[8] as verbatim bytes.
 */

/** @brief Format an AxlGuid into the 36-char canonical UUID string.
    @p buf must be at least 37 bytes. */
static void
guid_to_str(
    const AxlGuid *g,
    char           buf[37]
    )
{
    axl_snprintf(buf, 37,
        "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        (unsigned)g->data1,
        (unsigned)g->data2,
        (unsigned)g->data3,
        (unsigned)g->data4[0], (unsigned)g->data4[1],
        (unsigned)g->data4[2], (unsigned)g->data4[3],
        (unsigned)g->data4[4], (unsigned)g->data4[5],
        (unsigned)g->data4[6], (unsigned)g->data4[7]);
}

/** @brief Parse the canonical GUID string "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"
    into an AxlGuid.  @return 0 on success, -1 on parse error. */
static int
guid_from_str(
    const char *s,
    AxlGuid    *out
    )
{
    /* Canonical form is exactly 36 chars: 8-4-4-4-12 hex + 4 dashes */
    uint64_t v = 0;
    int      n;

    n = axl_hex_parse_u64(s, 8, &v);
    if (n != 8 || s[8] != '-') { return -1; }
    out->data1 = (uint32_t)v;
    s += 9;

    n = axl_hex_parse_u64(s, 4, &v);
    if (n != 4 || s[4] != '-') { return -1; }
    out->data2 = (uint16_t)v;
    s += 5;

    n = axl_hex_parse_u64(s, 4, &v);
    if (n != 4 || s[4] != '-') { return -1; }
    out->data3 = (uint16_t)v;
    s += 5;

    /* First 2 bytes of data4 (the "variant" field) */
    n = axl_hex_parse_u64(s, 4, &v);
    if (n != 4 || s[4] != '-') { return -1; }
    out->data4[0] = (uint8_t)(v >> 8);
    out->data4[1] = (uint8_t)(v & 0xffu);
    s += 5;

    /* Remaining 6 bytes of data4 (12 hex chars, no separator) */
    for (int i = 2; i < 8; i++) {
        n = axl_hex_parse_u64(s, 2, &v);
        if (n != 2) { return -1; }
        out->data4[i] = (uint8_t)v;
        s += 2;
    }

    return (*s == '\0') ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * Kind and type name helpers
 * ---------------------------------------------------------------------------
 */

static const char *
kind_name(
    AxlFwNodeKind kind
    )
{
    switch (kind) {
    case AXL_FW_NODE_IMAGE:   return "IMAGE";
    case AXL_FW_NODE_REGION:  return "REGION";
    case AXL_FW_NODE_VOLUME:  return "VOLUME";
    case AXL_FW_NODE_FILE:    return "FILE";
    case AXL_FW_NODE_SECTION: return "SECTION";
    case AXL_FW_NODE_NVRAM:   return "NVRAM";
    default:                  return "UNKNOWN";
    }
}

/* FFS file types (UEFI PI 1.8 §3.2.2) */
static const char *
file_type_name(
    int type
    )
{
    switch ((unsigned)type) {
    case 0x00: return "ALL";
    case 0x01: return "RAW";
    case 0x02: return "FREEFORM";
    case 0x03: return "SECURITY_CORE";
    case 0x04: return "PEI_CORE";
    case 0x05: return "DXE_CORE";
    case 0x06: return "PEIM";
    case 0x07: return "DRIVER";
    case 0x08: return "COMBINED_PEIM_DRIVER";
    case 0x09: return "APPLICATION";
    case 0x0A: return "SMM";
    case 0x0B: return "FIRMWARE_VOLUME_IMAGE";
    case 0x0C: return "COMBINED_SMM_DXE";
    case 0x0D: return "SMM_CORE";
    case 0x0E: return "MM_STANDALONE";
    case 0x0F: return "MM_CORE_STANDALONE";
    case 0xF0: return "PAD";
    default:   return NULL;
    }
}

/* Section types (UEFI PI 1.8 §3.3.1) */
static const char *
section_type_name(
    int type
    )
{
    switch ((unsigned)type) {
    case 0x01: return "COMPRESSION";
    case 0x02: return "GUID_DEFINED";
    case 0x03: return "DISPOSABLE";
    case 0x10: return "PE32";
    case 0x11: return "PIC";
    case 0x12: return "TE";
    case 0x13: return "DXE_DEPEX";
    case 0x14: return "VERSION";
    case 0x15: return "USER_INTERFACE";
    case 0x16: return "COMPATIBILITY16";
    case 0x17: return "FIRMWARE_VOLUME_IMAGE";
    case 0x18: return "FREEFORM_SUBTYPE_GUID";
    case 0x19: return "RAW";
    case 0x1B: return "PEI_DEPEX";
    case 0x1C: return "MM_DEPEX";
    case 0x1D: return "SMM_DEPEX";
    default:   return NULL;
    }
}

/* ---------------------------------------------------------------------------
 * list subcommand — DFS indented tree
 * ---------------------------------------------------------------------------
 */

/* Path buffer for ancestry in find/list context — a simple stack of short
   strings.  Not used for list (it just tracks depth), but we keep a single
   recursive helper used by both list and find. */
#define PATH_DEPTH_MAX  64

typedef struct {
    char segs[PATH_DEPTH_MAX][64];   /* one label per depth level */
    int  depth;
} PathStack;

static void
path_push(
    PathStack  *ps,
    const char *label
    )
{
    if (ps->depth < PATH_DEPTH_MAX) {
        axl_snprintf(ps->segs[ps->depth], 64, "%s", label);
        ps->depth++;
    }
}

static void
path_pop(
    PathStack *ps
    )
{
    if (ps->depth > 0) {
        ps->depth--;
    }
}

/* Print one tree line. */
static void
print_node_line(
    AxlFwNode *node,
    int        depth
    )
{
    AxlFwNodeKind kind  = axl_fw_node_kind(node);
    int           type  = axl_fw_node_type(node);

    /* Indent: 2 spaces per depth, up to reasonable terminal width */
    for (int i = 0; i < depth * 2; i++) {
        axl_print(" ");
    }

    /* Kind name */
    axl_printf("%s", kind_name(kind));

    /* Type annotation for FILE and SECTION */
    if (kind == AXL_FW_NODE_FILE) {
        const char *tname = file_type_name(type);
        if (tname != NULL) {
            axl_printf("(%s)", tname);
        } else {
            axl_printf("(0x%02x)", (unsigned)type);
        }
    } else if (kind == AXL_FW_NODE_SECTION) {
        const char *tname = section_type_name(type);
        if (tname != NULL) {
            axl_printf("(%s)", tname);
        } else {
            axl_printf("(0x%02x)", (unsigned)type);
        }
    }

    /* GUID when present */
    AxlGuid  g;
    if (axl_fw_node_guid(node, &g)) {
        char gbuf[37];
        guid_to_str(&g, gbuf);
        axl_printf(" %s", gbuf);
    }

    /* Offset */
    size_t off = 0;
    if (axl_fw_node_offset(node, &off)) {
        axl_printf(" off=0x%zx", off);
    }

    /* Body size */
    const void *body_ptr;
    size_t      body_len;
    if (axl_fw_node_data(node, &body_ptr, &body_len)) {
        axl_printf(" size=%zu", body_len);
    }

    axl_print("\n");
}

static void
list_node(
    AxlFwNode *node,
    int        depth
    )
{
    print_node_line(node, depth);

    AxlFwNode *child = axl_fw_node_first_child(node);
    while (child != NULL) {
        list_node(child, depth + 1);
        child = axl_fw_node_next_sibling(child);
    }
}

static int
cmd_list(
    const char *image_path
    )
{
    void  *buf = NULL;
    size_t len = 0;
    if (axl_file_get_contents(image_path, &buf, &len) != AXL_OK) {
        axl_printerr("fwtool: cannot read '%s'\n", image_path);
        return 1;
    }

    AxlFwImage *img = axl_fw_open(buf, len);
    if (img == NULL) {
        axl_printerr("fwtool: '%s': no firmware volumes found (not a .fd?)\n",
                     image_path);
        axl_free(buf);
        return 1;
    }

    AxlFwNode *root = axl_fw_root(img);
    /* Print the IMAGE root's children (volumes) without indenting the root
       itself — the file path is context enough). */
    AxlFwNode *child = axl_fw_node_first_child(root);
    while (child != NULL) {
        list_node(child, 0);
        child = axl_fw_node_next_sibling(child);
    }

    axl_fw_close(img);
    axl_free(buf);
    return 0;
}

/* ---------------------------------------------------------------------------
 * extract subcommand — write body bytes of a matched node
 * ---------------------------------------------------------------------------
 */

/* FFS section types we treat as the extractable "image" of a file. */
#define FW_SECTION_PE32  0x10
#define FW_SECTION_TE    0x12

/* Depth-first search for the first PE32/TE section under @p node (the
   matched FILE's executable image). Mirrors extract-fv-shell.py, which
   returns the PE32/TE section *body* of the Shell FFS file rather than the
   raw FFS body (a section container). Returns NULL if none is found. */
static AxlFwNode *
find_image_section(
    AxlFwNode *node
    )
{
    AxlFwNodeKind kind = axl_fw_node_kind(node);
    int           type = axl_fw_node_type(node);
    if (kind == AXL_FW_NODE_SECTION
        && (type == FW_SECTION_PE32 || type == FW_SECTION_TE)) {
        return node;
    }
    for (AxlFwNode *c = axl_fw_node_first_child(node);
         c != NULL;
         c = axl_fw_node_next_sibling(c)) {
        AxlFwNode *found = find_image_section(c);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static int
cmd_extract(
    const char *image_path,
    const char *guid_str,
    const char *outpath      /* NULL = stdout */
    )
{
    AxlGuid target;
    if (guid_from_str(guid_str, &target) != 0) {
        axl_printerr("fwtool: invalid GUID '%s' "
                     "(expected XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX)\n",
                     guid_str);
        return 1;
    }

    void  *buf = NULL;
    size_t len = 0;
    if (axl_file_get_contents(image_path, &buf, &len) != AXL_OK) {
        axl_printerr("fwtool: cannot read '%s'\n", image_path);
        return 1;
    }

    AxlFwImage *img = axl_fw_open(buf, len);
    if (img == NULL) {
        axl_printerr("fwtool: '%s': no firmware volumes found\n", image_path);
        axl_free(buf);
        return 1;
    }

    /* AXL_FW_NODE_IMAGE as kind = match any kind */
    AxlFwNode *node = axl_fw_find(img, &target, AXL_FW_NODE_IMAGE);
    if (node == NULL) {
        char gbuf[37];
        guid_to_str(&target, gbuf);
        axl_printerr("fwtool: GUID %s not found in '%s'\n", gbuf, image_path);
        axl_fw_close(img);
        axl_free(buf);
        return 1;
    }

    /* A FILE's body is a section container, not its runnable image. To
       extract the actual payload (and to byte-match extract-fv-shell.py),
       descend a matched FILE to its first PE32/TE section. A directly
       matched SECTION (or a FILE with no such section) is used as-is. */
    if (axl_fw_node_kind(node) == AXL_FW_NODE_FILE) {
        AxlFwNode *img_sec = find_image_section(node);
        if (img_sec != NULL) {
            node = img_sec;
        }
    }

    const void *body;
    size_t      body_len;
    if (!axl_fw_node_data(node, &body, &body_len)) {
        char gbuf[37];
        guid_to_str(&target, gbuf);
        axl_printerr("fwtool: GUID %s found but has no body\n", gbuf);
        axl_fw_close(img);
        axl_free(buf);
        return 1;
    }

    int rc = 0;
    if (outpath != NULL) {
        if (axl_file_set_contents(outpath, body, body_len) != AXL_OK) {
            axl_printerr("fwtool: cannot write '%s'\n", outpath);
            rc = 1;
        }
    } else {
        /* Write raw bytes to stdout_raw (bypasses UTF-8→UCS-2 conversion). */
        if (axl_stdout_raw == NULL) {
            axl_printerr("fwtool: binary stdout not available (not running under UEFI Shell); use -o <file>\n");
            axl_fw_close(img);
            axl_free(buf);
            return 1;
        }
        if (axl_write(axl_stdout_raw, body, body_len) != (axl_ssize_t)body_len) {
            axl_printerr("fwtool: stdout write failed\n");
            rc = 1;
        }
    }

    axl_fw_close(img);
    axl_free(buf);
    return rc;
}

/* ---------------------------------------------------------------------------
 * find subcommand — print ancestry path + offset of every matching node
 * ---------------------------------------------------------------------------
 */

typedef struct {
    const AxlGuid *target;
    int            matches;
} FindCtx;

/* Build a short label for a node (kind + optional guid) */
static void
node_label(
    AxlFwNode *node,
    char      *out,
    size_t     cap
    )
{
    AxlFwNodeKind kind = axl_fw_node_kind(node);
    AxlGuid       g;
    if (axl_fw_node_guid(node, &g)) {
        char gbuf[37];
        guid_to_str(&g, gbuf);
        axl_snprintf(out, cap, "%s[%s]", kind_name(kind), gbuf);
    } else {
        size_t off = 0;
        axl_fw_node_offset(node, &off);
        axl_snprintf(out, cap, "%s[0x%zx]", kind_name(kind), off);
    }
}

static void
find_node(
    AxlFwNode  *node,
    PathStack  *ps,
    FindCtx    *ctx
    )
{
    /* Build this node's label and push onto path. Sized to match the
       PathStack segment (64) so the push never truncates — the longest
       label, "KIND[<36-char guid>]", is ~45 chars. */
    char label[64];
    node_label(node, label, sizeof(label));
    path_push(ps, label);

    /* Check for a GUID match */
    AxlGuid g;
    if (axl_fw_node_guid(node, &g) && axl_guid_equal(&g, ctx->target)) {
        /* Print ancestry path */
        for (int i = 0; i < ps->depth; i++) {
            if (i > 0) { axl_print("/"); }
            axl_printf("%s", ps->segs[i]);
        }
        /* Also print offset and body size */
        size_t off = 0;
        axl_fw_node_offset(node, &off);
        axl_printf(" off=0x%zx", off);
        const void *body;
        size_t      body_len;
        if (axl_fw_node_data(node, &body, &body_len)) {
            axl_printf(" size=%zu", body_len);
        }
        axl_print("\n");
        ctx->matches++;
    }

    /* Recurse into children */
    AxlFwNode *child = axl_fw_node_first_child(node);
    while (child != NULL) {
        find_node(child, ps, ctx);
        child = axl_fw_node_next_sibling(child);
    }

    path_pop(ps);
}

static int
cmd_find(
    const char *image_path,
    const char *guid_str
    )
{
    AxlGuid target;
    if (guid_from_str(guid_str, &target) != 0) {
        axl_printerr("fwtool: invalid GUID '%s' "
                     "(expected XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX)\n",
                     guid_str);
        return 1;
    }

    void  *buf = NULL;
    size_t len = 0;
    if (axl_file_get_contents(image_path, &buf, &len) != AXL_OK) {
        axl_printerr("fwtool: cannot read '%s'\n", image_path);
        return 1;
    }

    AxlFwImage *img = axl_fw_open(buf, len);
    if (img == NULL) {
        axl_printerr("fwtool: '%s': no firmware volumes found\n", image_path);
        axl_free(buf);
        return 1;
    }

    PathStack ps;
    ps.depth = 0;

    FindCtx ctx;
    ctx.target  = &target;
    ctx.matches = 0;

    AxlFwNode *root = axl_fw_root(img);
    find_node(root, &ps, &ctx);

    if (ctx.matches == 0) {
        char gbuf[37];
        guid_to_str(&target, gbuf);
        axl_printerr("fwtool: GUID %s not found in '%s'\n", gbuf, image_path);
        axl_fw_close(img);
        axl_free(buf);
        return 1;
    }

    axl_fw_close(img);
    axl_free(buf);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------------
 */

static void
usage(
    void
    )
{
    axl_printerr(
        "Usage:\n"
        "  fwtool list <image>\n"
        "  fwtool extract <image> <guid> [-o outpath]\n"
        "  fwtool find <image> <guid>\n"
        "\n"
        "  list     Print an indented tree of all firmware nodes.\n"
        "  extract  Write the body of the node matching <guid> to a file or stdout.\n"
        "  find     Print the ancestry path of every node matching <guid>.\n"
        "\n"
        "  <guid>   Canonical UUID: XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX\n");
}

AXL_TOOL_MAIN(fwtool)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *subcmd = argv[1];

    if (axl_strcmp(subcmd, "list") == 0) {
        if (argc < 3) {
            axl_printerr("fwtool list: missing <image> argument\n");
            return 1;
        }
        return cmd_list(argv[2]);
    }

    if (axl_strcmp(subcmd, "extract") == 0) {
        if (argc < 4) {
            axl_printerr("fwtool extract: usage: fwtool extract <image> <guid> [-o outpath]\n");
            return 1;
        }
        const char *outpath = NULL;
        /* Scan remaining args for -o */
        for (int i = 4; i < argc; i++) {
            if (axl_strcmp(argv[i], "-o") == 0) {
                if (i + 1 >= argc) {
                    axl_printerr("fwtool extract: -o requires an argument\n");
                    return 1;
                }
                outpath = argv[i + 1];
                i++;
            }
        }
        return cmd_extract(argv[2], argv[3], outpath);
    }

    if (axl_strcmp(subcmd, "find") == 0) {
        if (argc < 4) {
            axl_printerr("fwtool find: usage: fwtool find <image> <guid>\n");
            return 1;
        }
        return cmd_find(argv[2], argv[3]);
    }

    axl_printerr("fwtool: unknown subcommand '%s'\n\n", subcmd);
    usage();
    return 1;
}
