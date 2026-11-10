/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* shell-growth-test.c -- fixture for test-shell-buffer-growth-qemu.sh.
 *
 * THE DEFECT. The modern UEFI Shell parks the buffer several
 * EFI_SHELL_PROTOCOL calls return (GetCurDir, GetEnv, GetMapFromDevicePath,
 * and through them GetDevicePathFromFilePath on a relative path and
 * OpenFileByName on EVERY open) on a free list it empties only when the
 * current shell COMMAND ends. An image that does N path operations inside
 * one command -- an app looping for hours, a resident driver whose calls all
 * park onto whatever command is current -- grows that list by O(N).
 *
 * Modes (argv[1]):
 *   growth <N>   Inside this ONE command, samples the firmware's free pages
 *                (EfiConventionalMemory in GetMemoryMap) around N operations
 *                of each class, relative and absolute, and prints
 *                `SHGROW <class> <rel|abs|-> n=<N> pages=<delta>`. The
 *                `control` class calls the raw Shell OpenFileByName: it must
 *                show growth, or every small figure is the sampler's
 *                blindness rather than the fix.
 *   func         Prints the OUTCOME of the tricky path cases -- return codes,
 *                contents, names -- as `SHFUNC <case> <outcome>` lines the
 *                test pins exactly, so resolution done AXL-side answers what
 *                the Shell answered.
 *   cd <dir>     Child mode: axl_chdir(<dir>) and exit. Started by `func`
 *                through axl_image_start (not the Shell), so the parent's
 *                cached current directory has to notice a change made by
 *                another image.
 *   exec cdroot|cdsub
 *                Child mode: axl_shell_execute a nested shell's `cd fs0:\`
 *                or `cd fs0:\sub` -- a nested shell started by another
 *                image, not by the parent.
 *
 * Staged layout (the test provides it), cwd fs0:\ at start:
 *     fs0:\dof_in.txt        "HELLO\n"
 *     fs0:\a                 "A\n"       (a one-character name)
 *     fs0:\LongFileName.txt  "LONG\n"    (an 8.3 alias LONGFI~1.TXT)
 *     fs0:\sub\nested.txt    "NESTED\n"
 *     fs0:\work\keep.txt     ""          (the classes that create and
 *                                         remove entries churn in work, so
 *                                         sub -- listed and stat'ed -- keeps
 *                                         its size, and the FAT driver's
 *                                         cache of it does too)
 */

#define AXL_ALLOW_UEFI 1
#include <axl.h>
#include <uefi/axl-uefi.h>
#include "../../src/fs/axl-fs-internal.h"   /* axl_fs_path_canonical */

#define VOL_MAX  16

// ---------------------------------------------------------------------------
// The sampler (heap-release-test.c's free_pages)
// ---------------------------------------------------------------------------

static int
free_pages(
    uint64_t *out
    )
{
    UINTN       size = 0, key = 0, dsize = 0;
    UINT32      dver = 0;
    EFI_STATUS  s;
    uint8_t    *buf;
    uint64_t    conv = 0;

    s = gBS->GetMemoryMap(&size, NULL, &key, &dsize, &dver);
    if (s != EFI_BUFFER_TOO_SMALL || dsize == 0) {
        return AXL_ERR;
    }
    /* Room for the descriptors the allocation below may itself add. */
    size += 8 * dsize;
    buf = axl_malloc(size);
    if (buf == NULL) {
        return AXL_ERR;
    }
    s = gBS->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)buf, &key, &dsize,
                          &dver);
    if (EFI_ERROR(s)) {
        axl_free(buf);
        return AXL_ERR;
    }
    for (UINTN off = 0; off < size; off += dsize) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(buf + off);

        if (d->Type == EfiConventionalMemory) {
            conv += d->NumberOfPages;
        }
    }
    axl_free(buf);
    *out = conv;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// One operation of each class
// ---------------------------------------------------------------------------

typedef enum {
    OP_READ,       /* open + read + close */
    OP_WRITE,      /* open(create) + write + close */
    OP_STAT,       /* stat */
    OP_DELETE,     /* create, then delete */
    OP_MKDIR,      /* mkdir, then rmdir */
    OP_LIST,       /* dir open + read all + close */
    OP_CANON,      /* axl_fs_path_canonical */
    OP_ENUM,       /* axl_volume_enumerate (no path) */
    OP_GETCWD,     /* axl_get_current_dir (no path) */
    OP_CONTROL,    /* the raw Shell OpenFileByName + CloseFile */
} OpClass;

typedef struct {
    const char *name;
    OpClass     op;
    const char *rel;   /* NULL: the class takes no path */
    const char *abs;
} OpSpec;

static const OpSpec mOps[] = {
    { "read",    OP_READ,    "sub\\nested.txt", "fs0:\\sub\\nested.txt" },
    { "write",   OP_WRITE,   "work\\w.txt",     "fs0:\\work\\w.txt" },
    { "stat",    OP_STAT,    "sub\\nested.txt", "fs0:\\sub\\nested.txt" },
    { "delete",  OP_DELETE,  "work\\d.txt",     "fs0:\\work\\d.txt" },
    { "mkdir",   OP_MKDIR,   "work\\md",        "fs0:\\work\\md" },
    { "list",    OP_LIST,    "sub",             "fs0:\\sub" },
    { "canon",   OP_CANON,   "sub\\nested.txt", "fs0:\\sub\\nested.txt" },
    { "enum",    OP_ENUM,    NULL,              NULL },
    { "getcwd",  OP_GETCWD,  NULL,              NULL },
    { "control", OP_CONTROL, "sub\\nested.txt", "fs0:\\sub\\nested.txt" },
};

static EFI_SHELL_PROTOCOL *
shell_protocol(void)
{
    EFI_GUID            guid  = gEfiShellProtocolGuid;
    EFI_SHELL_PROTOCOL *shell = NULL;
    if (EFI_ERROR(gBS->LocateProtocol(&guid, NULL, (VOID **)&shell))) {
        return NULL;
    }
    return shell;
}

/* One operation. @return true when it did what it should. */
static bool
op_once(
    OpClass     op,
    const char *path
    )
{
    switch (op) {
    case OP_READ: {
        void  *buf = NULL;
        size_t n   = 0;
        bool   ok  = axl_file_get_contents(path, &buf, &n) == AXL_OK && n == 7;
        axl_free(buf);
        return ok;
    }
    case OP_WRITE:
        return axl_file_set_contents(path, "w", 1) == AXL_OK;
    case OP_STAT: {
        AxlFsEntry e;
        return axl_file_info(path, &e) == AXL_OK && e.size == 7;
    }
    case OP_DELETE:
        return axl_file_set_contents(path, "d", 1) == AXL_OK
               && axl_file_delete(path) == AXL_OK;
    case OP_MKDIR:
        return axl_dir_mkdir(path) == AXL_OK && axl_dir_rmdir(path) == AXL_OK;
    case OP_LIST: {
        AxlDir *d = axl_dir_open(path);
        if (d == NULL) {
            return false;
        }
        AxlFsEntry  e;
        size_t      n = 0;
        while (axl_dir_read(d, &e)) {
            n++;
        }
        axl_dir_close(d);
        return n > 0;
    }
    case OP_CANON: {
        void *vol = NULL;
        char  out[AXL_FS_REL_MAX];
        return axl_fs_path_canonical(path, &vol, out, sizeof(out)) == AXL_OK
               && axl_strcmp(out, "sub\\nested.txt") == 0;
    }
    case OP_ENUM: {
        AxlVolume v[VOL_MAX];
        size_t    n = 0;
        return axl_volume_enumerate(v, VOL_MAX, &n) == AXL_OK && n > 0;
    }
    case OP_GETCWD: {
        char *cwd = axl_get_current_dir();
        bool  ok  = cwd != NULL;
        axl_free(cwd);
        return ok;
    }
    case OP_CONTROL: {
        EFI_SHELL_PROTOCOL *shell = shell_protocol();
        unsigned short      w[64];
        SHELL_FILE_HANDLE   fh = NULL;
        if (shell == NULL) {
            return false;
        }
        axl_utf8_to_ucs2_buf(path, w, 64);
        if (EFI_ERROR(shell->OpenFileByName((CHAR16 *)w, &fh,
                                            EFI_FILE_MODE_READ))) {
            return false;
        }
        shell->CloseFile(fh);
        return true;
    }
    }
    return false;
}

/* @p n operations; @return how many did what they should. */
static size_t
op_loop(
    OpClass     op,
    const char *path,
    size_t      n
    )
{
    size_t good = 0;
    for (size_t i = 0; i < n; i++) {
        if (op_once(op, path)) {
            good++;
        }
    }
    return good;
}

static int
growth(
    size_t n
    )
{
    for (size_t i = 0; i < sizeof(mOps) / sizeof(mOps[0]); i++) {
        const OpSpec *s = &mOps[i];
        for (int pass = 0; pass < 2; pass++) {
            const char *path = (pass == 0) ? s->rel : s->abs;
            const char *kind = (s->rel == NULL) ? "-" : (pass == 0) ? "rel" : "abs";
            if (pass == 1 && s->rel == NULL) {
                break;
            }
            /* Warm-up: the same operation an eighth as many times first, so
               one-off growth lands outside the sample -- the first lookups a
               cache keeps, and AxlMem's DEBUG free quarantine (up to 64 KiB
               of freed blocks, held) taking on THIS class's block sizes. That
               reshaping measured 5-7 pages on the first class to list a
               directory, the same at N=400 and at N=1600: a step, not a
               slope. */
            op_loop(s->op, path, n / 8);
            uint64_t before = 0, after = 0;
            if (free_pages(&before) != AXL_OK) {
                axl_printf("SHGROW %s %s FAILED sampler\n", s->name, kind);
                return 1;
            }
            size_t good = op_loop(s->op, path, n);
            if (free_pages(&after) != AXL_OK) {
                axl_printf("SHGROW %s %s FAILED sampler\n", s->name, kind);
                return 1;
            }
            axl_printf("SHGROW %s %s n=%zu ok=%zu pages=%ld\n", s->name, kind,
                       n, good, (long)((int64_t)before - (int64_t)after));
        }
    }
    axl_printf("SHGROW done\n");
    return 0;
}

// ---------------------------------------------------------------------------
// func: the outcomes, pinned exactly by the test
// ---------------------------------------------------------------------------

/* `SHFUNC <case> rc=<rc> [<contents with \n shown as |>]` */
static void
show_read(
    const char *tag,
    const char *path
    )
{
    void  *buf = NULL;
    size_t n   = 0;
    int    rc  = axl_file_get_contents(path, &buf, &n);
    char   text[32];
    size_t k = 0;
    for (size_t i = 0; rc == AXL_OK && i < n && k + 1 < sizeof(text); i++) {
        char c = ((const char *)buf)[i];
        text[k++] = (c == '\n') ? '|' : c;
    }
    text[k] = '\0';
    axl_free(buf);
    axl_printf("SHFUNC %s rc=%d %s\n", tag, rc, text);
}

static void
show_info(
    const char *tag,
    const char *path
    )
{
    AxlFsEntry e;
    axl_memset(&e, 0, sizeof(e));
    int rc = axl_file_info(path, &e);
    axl_printf("SHFUNC %s rc=%d dir=%d size=%llu\n", tag, rc,
               rc == AXL_OK ? (int)axl_fs_entry_is_dir(&e) : -1,
               rc == AXL_OK ? (unsigned long long)e.size : 0ull);
}

static void
show_canon(
    const char *tag,
    const char *path
    )
{
    void *vol = NULL;
    char  out[AXL_FS_REL_MAX];
    out[0] = '\0';
    int rc = axl_fs_path_canonical(path, &vol, out, sizeof(out));
    axl_printf("SHFUNC %s rc=%d [%s]\n", tag, rc, rc == AXL_OK ? out : "");
}

static void
show_cwd(
    const char *tag
    )
{
    char *cwd = axl_get_current_dir();
    axl_printf("SHFUNC %s cwd=[%s]\n", tag, cwd != NULL ? cwd : "(null)");
    axl_free(cwd);

    /* Diagnostic, not pinned: the Shell's `cwd` variable mirror beside it. */
    /* EDK2's gShellVariableGuid, spelled here so the fixture builds against
       the library before the change too (the RED run). */
    EFI_GUID       guid = { 0x158def5a, 0xf656, 0x419c,
                            {0xb0, 0x27, 0x7a, 0x31, 0x92, 0xc0, 0x79, 0xd2} };
    unsigned short var[128];
    UINTN          sz = sizeof(var) - sizeof(var[0]);
    char           text[128] = "(none)";
    if (!EFI_ERROR(gRT->GetVariable((CHAR16 *)L"cwd", &guid, NULL, &sz, var))) {
        var[sz / sizeof(var[0])] = 0;
        axl_ucs2_to_utf8_buf(var, text, sizeof(text));
    }
    axl_printf("SHDIAG %s var=[%s]\n", tag, text);
}

/* The device path of the volume the shell calls fs0. */
static const void *
fs0_device_path(void)
{
    AxlVolume v[VOL_MAX];
    size_t    n = 0;
    if (axl_volume_enumerate(v, VOL_MAX, &n) != AXL_OK) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (axl_strcmp(v[i].name, "fs0") == 0) {
            return v[i].device_path;
        }
    }
    return NULL;
}

static void
show_names(
    const char *tag,
    const void *dp
    )
{
    char name[32]  = "";
    char alias[32] = "";
    int  rn = axl_volume_map_name(dp, name, sizeof(name));
    int  ra = axl_volume_map_alias(dp, alias, sizeof(alias));
    axl_printf("SHFUNC %s name rc=%d [%s] alias rc=%d [%s]\n", tag, rn,
               rn == AXL_OK ? name : "", ra, ra == AXL_OK ? alias : "");
}

/* Every volume, in enumeration order: its name and its map name. */
static void
show_volumes(
    const char *tag
    )
{
    AxlVolume v[VOL_MAX];
    size_t    n = 0;
    int       rc = axl_volume_enumerate(v, VOL_MAX, &n);
    axl_printf("SHFUNC %s rc=%d count=%zu", tag, rc, n);
    for (size_t i = 0; rc == AXL_OK && i < n; i++) {
        char name[32] = "";
        int  rn = axl_volume_map_name(v[i].device_path, name, sizeof(name));
        axl_printf(" [%s %d %s]", v[i].name, rn, rn == AXL_OK ? name : "");
    }
    axl_printf("\n");
}

/* Start this image again as `<mode> <arg>` through axl_image_start, so
   ANOTHER image acts -- not this one, and not through this one's calls. */
static int
child_run(
    const char *mode,
    const char *arg
    )
{
    AxlImage *img = NULL;
    if (axl_image_load("fs0:\\shell-growth-test.efi", &img) != AXL_OK) {
        return AXL_ERR;
    }
    char opts[96];
    int  n = axl_snprintf(opts, sizeof(opts), "shell-growth-test.efi %s %s", mode, arg);
    unsigned short w[96];
    axl_utf8_to_ucs2_buf(opts, w, 96);
    int rc = axl_image_set_load_options(img, w, ((size_t)n + 1) * sizeof(w[0]));
    int code = -1;
    if (rc == AXL_OK) {
        rc = axl_image_start(img, &code);
    }
    axl_image_unload(img);
    return (rc == AXL_OK && code == 0) ? AXL_OK : AXL_ERR;
}

static int
func(void)
{
    /* Volumes: any case, with or without the separator after the ':'. */
    show_read("abs", "fs0:\\dof_in.txt");
    show_read("abs-upper", "FS0:\\dof_in.txt");
    show_read("abs-mixed-nosep", "Fs0:dof_in.txt");
    show_read("abs-slash", "fs0:/sub/nested.txt");
    show_read("abs-dotdot", "fs0:\\sub\\..\\dof_in.txt");
    show_read("abs-dot", "fs0:\\.\\dof_in.txt");
    show_read("abs-trailing-sep", "fs0:\\dof_in.txt\\");
    show_read("abs-onechar", "fs0:\\a");
    show_read("abs-onechar-nosep", "fs0:a");
    show_read("abs-8.3", "fs0:\\LONGFI~1.TXT");
    show_read("abs-long-case", "fs0:\\longfilename.TXT");
    /* Relative to the current directory (fs0:\). */
    show_read("rel", "dof_in.txt");
    show_read("rel-sub", "sub\\nested.txt");
    show_read("rel-dot", ".\\dof_in.txt");
    show_read("rel-rootsep", "\\sub\\nested.txt");
    show_read("rel-onechar", "a");
    /* Missing things: a file, a directory on the way, a volume, a map. */
    show_read("miss-file-abs", "fs0:\\nope.txt");
    show_read("miss-file-rel", "nope.txt");
    show_read("miss-dir-abs", "fs0:\\nodir\\x.txt");
    show_read("miss-dir-rel", "nodir\\x.txt");
    show_read("miss-vol", "fs9:\\x.txt");
    show_read("miss-map", "zz:\\x.txt");
    show_read("miss-map-nosep", "zz:x.txt");
    axl_printf("SHFUNC miss-delete-abs rc=%d\n", axl_file_delete("fs0:\\nope.txt"));
    axl_printf("SHFUNC miss-delete-rel rc=%d\n", axl_file_delete("nope.txt"));
    axl_printf("SHFUNC miss-delete-vol rc=%d\n", axl_file_delete("fs9:\\nope.txt"));
    axl_printf("SHFUNC miss-delete-map rc=%d\n", axl_file_delete("zz:\\nope.txt"));
    /* Stat: the root in every spelling, a directory, with a trailing '\'. */
    show_info("info-root-bare", "fs0:");
    show_info("info-root", "fs0:\\");
    show_info("info-dir", "fs0:\\sub");
    show_info("info-dir-sep", "fs0:\\sub\\");
    show_info("info-dir-rel", "sub");
    show_info("info-file-rel", "sub\\nested.txt");
    show_info("info-miss-vol", "fs9:\\");
    axl_printf("SHFUNC isdir-rel %d\n", (int)axl_file_is_dir("sub"));
    axl_printf("SHFUNC isdir-abs %d\n", (int)axl_file_is_dir("FS0:\\SUB"));
    /* Canonical: the stored names, through an 8.3 alias and a case change. */
    show_canon("canon-8.3", "fs0:\\LONGFI~1.TXT");
    show_canon("canon-case", "FS0:\\SUB\\NESTED.TXT");
    show_canon("canon-rel-dotdot", "sub\\..\\dof_in.txt");
    show_canon("canon-rel-missing", "sub\\missing.txt");
    show_canon("canon-missing-dir", "fs0:\\nodir\\x");
    show_canon("canon-miss-vol", "fs9:\\x");
    show_canon("canon-root", "fs0:\\");
    /* Create and remove, relative and absolute. */
    axl_printf("SHFUNC mkdir-rel rc=%d\n", axl_dir_mkdir("newd"));
    show_info("mkdir-rel-info", "fs0:\\newd");
    axl_printf("SHFUNC rmdir-abs rc=%d\n", axl_dir_rmdir("FS0:\\NEWD"));
    axl_printf("SHFUNC write-rel rc=%d\n", axl_file_set_contents("sub\\made.txt", "M\n", 2));
    show_read("write-rel-read", "fs0:\\sub\\made.txt");
    axl_printf("SHFUNC delete-rel rc=%d\n", axl_file_delete("sub\\made.txt"));
    show_read("delete-rel-gone", "fs0:\\sub\\made.txt");

    /* The current directory, and every way it changes under this image. A
       relative operation BEFORE each change has the current directory in
       hand, so a stale copy of it would show AFTER the change. */
    show_cwd("cwd-start");
    show_read("cwd-start-rel", "dof_in.txt");
    axl_printf("SHFUNC chdir-sub rc=%d\n", axl_chdir("sub"));
    show_cwd("cwd-after-chdir");
    show_read("chdir-rel", "nested.txt");
    show_read("chdir-rel-dotdot", "..\\dof_in.txt");
    show_read("chdir-rel-rootsep", "\\dof_in.txt");
    show_canon("chdir-canon", "nested.txt");
    axl_printf("SHFUNC chdir-up rc=%d\n", axl_chdir(".."));
    show_cwd("cwd-after-up");
    show_read("up-rel", "dof_in.txt");
    axl_printf("SHFUNC chdir-root rc=%d\n", axl_chdir("\\"));
    show_cwd("cwd-after-root");
    show_read("root-rel", "dof_in.txt");
    /* The Shell's own Execute runs the command in a NESTED shell: its `cd`
       leaves this shell's current directory -- and, measured, the `cwd`
       variable -- alone. */
    axl_printf("SHFUNC exec-cd rc=%d\n", axl_shell_execute("cd fs0:\\sub"));
    show_cwd("cwd-after-exec");
    show_read("exec-rel", "dof_in.txt");
    show_read("exec-rel-sub", "nested.txt");
    /* A chdir here to exactly where the nested `cd` went. */
    show_cwd("pre-converge-cwd");
    axl_printf("SHFUNC converge rc=%d\n", axl_chdir("fs0:\\sub"));
    show_cwd("converged-cwd");
    show_read("converged-rel", "nested.txt");
    axl_printf("SHFUNC converge-back rc=%d\n", axl_chdir("\\"));
    show_cwd("converged-back-cwd");
    /* Another image started with StartImage changes it. */
    axl_printf("SHFUNC child-cd rc=%d\n", child_run("cd", "sub"));
    show_cwd("cwd-after-child");
    show_read("child-rel", "nested.txt");
    axl_printf("SHFUNC child-cd-back rc=%d\n", child_run("cd", "\\"));
    show_cwd("cwd-after-child-back");
    show_read("child-back-rel", "dof_in.txt");

    /* The directory moved back to one this image had in hand, by others,
       with this image asking nothing in between: were the `cwd` variable to
       lag the shell (a nested shell writing it alone), these are the
       sequences that would show it. On the EDK2 Shell it has not lagged. */
    /* (1) Nested shells and another image move it while this image waits. */
    axl_printf("SHFUNC aba1-chdir rc=%d\n", axl_chdir("fs0:\\sub"));
    axl_printf("SHFUNC aba1-child-exec rc=%d\n", child_run("exec", "cdroot"));
    show_cwd("aba1-mid-cwd");
    axl_printf("SHFUNC aba1-child-cd rc=%d\n", child_run("cd", "\\"));
    axl_printf("SHFUNC aba1-child-exec2 rc=%d\n", child_run("exec", "cdsub"));
    show_cwd("aba1-cwd");
    show_read("aba1-rel", "dof_in.txt");
    axl_printf("SHFUNC aba1-back rc=%d\n", axl_chdir("\\"));
    /* (2) Another image moves it away; this image's Execute moves it back. */
    axl_printf("SHFUNC aba2-chdir rc=%d\n", axl_chdir("fs0:\\sub"));
    show_cwd("aba2-start-cwd");
    axl_printf("SHFUNC aba2-child-cd rc=%d\n", child_run("cd", "\\"));
    axl_printf("SHFUNC aba2-exec rc=%d\n", axl_shell_execute("cd fs0:\\sub"));
    show_cwd("aba2-cwd");
    show_read("aba2-rel", "dof_in.txt");
    axl_printf("SHFUNC aba2-back rc=%d\n", axl_chdir("\\"));
    /* (3) This image moves it away; another image's Execute moves it back. */
    axl_printf("SHFUNC aba3-chdir rc=%d\n", axl_chdir("fs0:\\sub"));
    show_cwd("aba3-start-cwd");
    axl_printf("SHFUNC aba3-chdir-root rc=%d\n", axl_chdir("\\"));
    axl_printf("SHFUNC aba3-child-exec rc=%d\n", child_run("exec", "cdsub"));
    show_cwd("aba3-cwd");
    show_read("aba3-rel", "dof_in.txt");
    axl_printf("SHFUNC aba3-back rc=%d\n", axl_chdir("\\"));
    show_cwd("aba-end-cwd");

    /* Names for a volume, and every way they change under this image. A
       lookup before each change has the names in hand. */
    const void *dp = fs0_device_path();
    axl_printf("SHFUNC fs0-dp %s\n", dp != NULL ? "found" : "missing");
    if (dp != NULL) {
        show_volumes("vols-start");
        show_names("names-start", dp);
        axl_printf("SHFUNC setmap-qq rc=%d\n", axl_volume_set_map(dp, "QQ:"));
        show_names("names-after-setmap", dp);
        /* A second fs<n> name, then the first set AGAIN: the Shell deletes
           and re-appends it, so FS7 is now the first fs<n> and something
           else the first name -- with every name the cache held still
           mapped to this volume. */
        axl_printf("SHFUNC setmap-fs7 rc=%d\n", axl_volume_set_map(dp, "FS7:"));
        axl_printf("SHFUNC reset-fs0 rc=%d\n", axl_volume_set_map(dp, "FS0:"));
        show_names("names-after-reorder", dp);
        axl_printf("SHFUNC unmap-fs7 rc=%d\n", axl_volume_unmap("FS7:"));
        show_names("names-after-unmap-fs7", dp);
        /* The same moves made by ANOTHER image, which this image hears
           nothing about: FS7 added, the names read (two fs<n> now), FS0 set
           again. */
        axl_printf("SHFUNC child-setmap-fs7 rc=%d\n", child_run("setmap", "FS7:"));
        show_names("names-after-child-fs7", dp);
        axl_printf("SHFUNC child-reset-fs0 rc=%d\n", child_run("setmap", "FS0:"));
        show_names("names-after-child-reorder", dp);
        axl_printf("SHFUNC child-unmap-fs7 rc=%d\n", child_run("unmap", "FS7:"));
        show_names("names-after-child-unmap-fs7", dp);
        show_volumes("vols-after-child-unmap-fs7");
        show_read("alias-read", "qq:\\dof_in.txt");
        show_read("alias-read-upper-nosep", "QQ:dof_in.txt");
        /* The volume's fs<n> name, and the one the current directory is on,
           goes away -- removed by another image. */
        axl_printf("SHFUNC unmap-fs0 rc=%d\n", child_run("unmap", "FS0:"));
        show_names("names-after-unmap-fs0", dp);
        show_read("unmapped-read", "fs0:\\dof_in.txt");
        show_read("unmapped-alias-read", "qq:\\dof_in.txt");
        show_cwd("unmapped-cwd");
        show_read("unmapped-rel", "dof_in.txt");
        axl_printf("SHFUNC unmapped-delete-rel rc=%d\n", axl_file_delete("nope.txt"));
        axl_printf("SHFUNC unmap-qq rc=%d\n", axl_volume_unmap("qq"));
        show_names("names-after-unmap-qq", dp);
        /* Back, now LAST on the list (a refresh would run in a nested shell
           and leave this one's map alone). */
        axl_printf("SHFUNC remap-fs0 rc=%d\n", axl_volume_set_map(dp, "FS0:"));
        show_names("names-after-remap", dp);
        show_read("remap-read", "fs0:\\dof_in.txt");
        show_cwd("remap-cwd");
        show_read("remap-rel", "dof_in.txt");

        /* EVERY name goes. With the Shell's map list empty, GetCurDir
           answers NULL although `cwd` itself (and its variable mirror)
           survives -- the one change the mirror cannot see. */
        AxlVolume v[VOL_MAX];
        size_t    nv      = 0;
        size_t    removed = 0;
        int       bad     = 0;
        if (axl_volume_enumerate(v, VOL_MAX, &nv) == AXL_OK) {
            for (size_t i = 0; i < nv; i++) {
                if (axl_volume_unmap_device(v[i].device_path, &removed) != AXL_OK) {
                    bad++;
                }
            }
        }
        for (AxlHandle h = axl_block_next(NULL); h != NULL; h = axl_block_next(h)) {
            void *bdp = NULL;
            if (axl_handle_get_protocol(h, "device-path", &bdp) == AXL_OK
                && axl_volume_unmap_device(bdp, &removed) != AXL_OK) {
                bad++;
            }
        }
        axl_printf("SHFUNC unmap-all failures=%d\n", bad);
        show_names("names-all-unmapped", dp);
        show_cwd("all-unmapped-cwd");
        show_read("all-unmapped-rel", "dof_in.txt");
        axl_printf("SHFUNC all-unmapped-delete-rel rc=%d\n", axl_file_delete("nope.txt"));
        axl_printf("SHFUNC restore-fs0 rc=%d\n", axl_volume_set_map(dp, "FS0:"));
        show_cwd("restored-cwd");
        show_read("restored-rel", "dof_in.txt");
    }
    axl_printf("SHFUNC done\n");
    return 0;
}

int
main(
    int    argc,
    char **argv
    )
{
    uint64_t n = 0;

    if (argc == 3 && axl_strcmp(argv[1], "growth") == 0
        && axl_str_to_u64(argv[2], 10, &n, NULL) == AXL_OK && n > 0) {
        return growth((size_t)n);
    }
    if (argc == 2 && axl_strcmp(argv[1], "func") == 0) {
        return func();
    }
    if (argc == 3 && axl_strcmp(argv[1], "exec") == 0) {
        /* `exec cdroot` / `exec cdsub` run a nested shell's `cd \` /
           `cd \sub` (one word each: the load options split a command with
           a space into two arguments). */
        const char *cmd = (axl_strcmp(argv[2], "cdroot") == 0) ? "cd fs0:\\"
                        : (axl_strcmp(argv[2], "cdsub") == 0)  ? "cd fs0:\\sub"
                        : NULL;
        return (cmd != NULL && axl_shell_execute(cmd) == AXL_OK) ? 0 : 1;
    }
    if (argc == 3 && axl_strcmp(argv[1], "setmap") == 0) {
        const void *dp = fs0_device_path();
        return (dp != NULL && axl_volume_set_map(dp, argv[2]) == AXL_OK) ? 0 : 1;
    }
    if (argc == 3 && axl_strcmp(argv[1], "unmap") == 0) {
        return axl_volume_unmap(argv[2]) == AXL_OK ? 0 : 1;
    }
    if (argc == 3 && axl_strcmp(argv[1], "cd") == 0) {
        return axl_chdir(argv[2]) == AXL_OK ? 0 : 1;
    }
    axl_printf("usage: shell-growth-test growth <N> | func | cd <dir> | exec cdroot|cdsub | setmap|unmap <name>\n");
    return 1;
}
