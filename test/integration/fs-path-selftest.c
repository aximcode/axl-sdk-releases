/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* fs-path-selftest.c — pins the file/directory layer's path resolution
 * (`fsN:`-qualified, root-relative, and cwd-relative) against BOTH shells.
 *
 * On the modern EDK2 shell every check passes through EFI_SHELL_PROTOCOL.
 * On the old EFI 1.x shell there is no such protocol, so the backend must
 * resolve paths itself (SHELL_ENVIRONMENT.GetMap / .CurDir + a direct
 * EFI_FILE_PROTOCOL open). The SAME battery runs on both — parity is the
 * contract.
 *
 * A plain shell app (int main), public headers only.
 *
 * Staged fixture layout (the runner provides it):
 *     fs0:\dof_in.txt      == "HELLO\n"   (6 bytes)
 *     fs0:\sub\nested.txt  == "NESTED\n"  (7 bytes)
 *
 * argv[1] selects the scenario:
 *   (none)  — full battery; assumes the cwd is the volume root.
 *   sub     — cwd-relative battery; assumes the cwd is fs0:\sub. Proves
 *             relative resolution follows the shell's *current directory*,
 *             not just the volume root.
 *
 * Every check prints an exact, greppable line: `FSSELF:<name>=PASS|FAIL`.
 * The footer `FSSELF:results pass=<n> fail=<n>` closes the run so the
 * host can distinguish "all passed" from "died halfway".
 */
#include <axl.h>

static unsigned mPass;
static unsigned mFail;

static void
check(const char *name, bool ok)
{
    if (ok) {
        mPass++;
    } else {
        mFail++;
    }
    axl_printf("FSSELF:%s=%s\n", name, ok ? "PASS" : "FAIL");
}

/* True when @p path holds exactly @p want (NUL-terminated compare on a
 * NUL-padded copy — the file's bytes are not NUL-terminated on disk). */
static bool
contents_are(const char *path, const char *want)
{
    void   *buf = NULL;
    size_t  n   = 0;
    if (axl_file_get_contents(path, &buf, &n) != AXL_OK || buf == NULL) {
        return false;
    }
    size_t wlen = axl_strlen(want);
    bool   ok   = (n == wlen) && (axl_memcmp(buf, want, wlen) == 0);
    axl_free(buf);
    return ok;
}

/* True when opening @p path for read yields exactly @p want. */
static bool
stream_reads(const char *path, const char *want)
{
    AxlStream *s = axl_fopen(path, "r");
    if (s == NULL) {
        return false;
    }
    char   buf[32] = {0};
    size_t n       = axl_fread(buf, 1, sizeof(buf) - 1, s);
    axl_fclose(s);
    return (n == axl_strlen(want)) && (axl_strcmp(buf, want) == 0);
}

static bool
exists(const char *path)
{
    AxlFsEntry e;
    return axl_file_info(path, &e) == AXL_OK;
}

static bool
sized(const char *path, uint64_t want)
{
    AxlFsEntry e;
    return axl_file_info(path, &e) == AXL_OK && e.size == want;
}

// ===================================================================
// Scenarios
// ===================================================================

static void
scenario_sub(void)
{
    /* cwd is fs0:\sub — relative names must resolve there, not at the
       volume root, and an absolute path must still reach the root. */
    check("sub-cwdrel-info", sized("nested.txt", 7));
    check("sub-cwdrel-get", contents_are("nested.txt", "NESTED\n"));
    check("sub-cwdrel-open", stream_reads("nested.txt", "NESTED\n"));
    /* A leading '\' is root-relative even from a subdirectory cwd. */
    check("sub-rootrel-get", contents_are("\\dof_in.txt", "HELLO\n"));
    check("sub-abs-get", contents_are("fs0:\\dof_in.txt", "HELLO\n"));
    /* The sibling name must NOT resolve from the parent's directory. */
    check("sub-cwdrel-miss", !exists("dof_in.txt"));
}

static void
scenario_root(void)
{
    // --- read paths, three spellings of the same file --------------
    check("info-abs", sized("fs0:\\dof_in.txt", 6));
    check("info-rel", sized("dof_in.txt", 6));
    check("info-rootrel", sized("\\dof_in.txt", 6));
    check("info-abs-upper", sized("FS0:\\dof_in.txt", 6));

    check("open-abs", stream_reads("fs0:\\dof_in.txt", "HELLO\n"));
    check("open-rel", stream_reads("dof_in.txt", "HELLO\n"));

    check("get-abs", contents_are("fs0:\\dof_in.txt", "HELLO\n"));
    check("get-rel", contents_are("dof_in.txt", "HELLO\n"));

    // --- subdirectory paths ----------------------------------------
    check("nested-abs", contents_are("fs0:\\sub\\nested.txt", "NESTED\n"));
    check("nested-rel", contents_are("sub\\nested.txt", "NESTED\n"));

    check("isdir-yes", axl_file_is_dir("fs0:\\sub"));
    check("isdir-no", !axl_file_is_dir("fs0:\\dof_in.txt"));
    /* A trailing separator on a directory path must still resolve (the shell's
       OpenFileByName tolerates it; the old-shell resolver strips it). */
    check("isdir-trailing", axl_file_is_dir("fs0:\\sub\\"));

    // --- seek/tell over a resolved handle --------------------------
    AxlStream *s  = axl_fopen("fs0:\\dof_in.txt", "r");
    bool       st = false;
    if (s != NULL) {
        st = (axl_fseek(s, 0, AXL_SEEK_END) == AXL_OK) && (axl_ftell(s) == 6);
        axl_fclose(s);
    }
    check("seek-tell", st);

    // --- directory iteration ---------------------------------------
    AxlDir *d     = axl_dir_open("fs0:\\");
    bool    found = false;
    if (d != NULL) {
        AxlFsEntry e;
        while (axl_dir_read(d, &e)) {
            if (axl_strcmp(e.name, "dof_in.txt") == 0) {
                found = true;
            }
        }
        axl_dir_close(d);
    }
    check("dir-list", found);

    // --- write paths (this is the `RW: ro` misreport in the field) --
    check("set-contents", axl_file_set_contents("fs0:\\axlprb.txt", "xyz", 3)
                          == AXL_OK);
    check("set-get-roundtrip", contents_are("fs0:\\axlprb.txt", "xyz"));
    /* Rewriting shorter must truncate the whole file, not leave a stale tail. */
    check("set-truncate", axl_file_set_contents("fs0:\\axlprb.txt", "z", 1)
                          == AXL_OK
                          && contents_are("fs0:\\axlprb.txt", "z"));
    check("set-contents-rel", axl_file_set_contents("axlprb2.txt", "r", 1)
                              == AXL_OK
                              && contents_are("fs0:\\axlprb2.txt", "r"));

    // --- rename / delete -------------------------------------------
    check("rename", axl_file_rename("fs0:\\axlprb.txt", "axlprb3.txt") == AXL_OK
                    && exists("fs0:\\axlprb3.txt")
                    && !exists("fs0:\\axlprb.txt"));
    check("delete", axl_file_delete("fs0:\\axlprb3.txt") == AXL_OK
                    && !exists("fs0:\\axlprb3.txt"));
    check("delete-rel", axl_file_delete("axlprb2.txt") == AXL_OK
                        && !exists("fs0:\\axlprb2.txt"));

    // --- mkdir / rmdir ---------------------------------------------
    check("mkdir", axl_dir_mkdir("fs0:\\axltd") == AXL_OK
                   && axl_file_is_dir("fs0:\\axltd"));
    check("rmdir", axl_dir_rmdir("fs0:\\axltd") == AXL_OK
                   && !exists("fs0:\\axltd"));

    // --- negatives (must fail cleanly, not resolve to the wrong file)
    check("neg-missing-file", !exists("fs0:\\nosuch.txt")
                              && axl_fopen("fs0:\\nosuch.txt", "r") == NULL);
    check("neg-missing-vol", !exists("fs9:\\nosuch.txt"));
    check("neg-missing-dir", !exists("fs0:\\nodir\\nosuch.txt"));

    // --- shell environment -----------------------------------------
    char *path_var = axl_getenv("path");
    check("getenv-path", path_var != NULL);
    axl_free(path_var);
}

int
main(int argc, char **argv)
{
    /* The negative-path checks deliberately probe missing files, which log a
       WARN apiece. On the interactive serial console stdout and that WARN
       stream share one line-buffered device, so a warning can interleave into
       the middle of an FSSELF: line and corrupt exact-line matching. Silence
       logs — this is a self-checking binary, its FSSELF: lines are the output
       that matters. Keep ERROR so a real failure still surfaces. */
    axl_log_set_level(AXL_LOG_ERROR);

    char *cwd = axl_get_current_dir();
    axl_printf("FSSELF:cwd=[%s]\n", (cwd != NULL) ? cwd : "(null)");
    check("getcwd", cwd != NULL);
    axl_free(cwd);

    if (argc > 1 && axl_strcmp(argv[1], "sub") == 0) {
        scenario_sub();
    } else {
        scenario_root();
    }

    axl_printf("FSSELF:results pass=%u fail=%u\n", mPass, mFail);
    return (mFail == 0) ? 0 : 1;
}
