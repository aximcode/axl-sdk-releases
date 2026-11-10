/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-shell.c
    Launch a real UEFI Shell as a foreground child image.

    Shell *policy* over the generic foreground launcher: locate a
    `Shell.efi` (the EDK2 ShellLauncher search) and run it via
    @ref axl_image_run with the `-nostartup` convention — so a child Shell
    started *from* `startup.nsh` does not re-run `startup.nsh` and recurse
    back into the launcher. The generic "run a blocking UEFI app and get
    its exit code" mechanism is `axl_image_run` (`<axl/axl-image.h>`); this
    is just the Shell-specific wrapper around it.
**/

#include <axl/axl-shell.h>
#include <axl/axl-image.h>
#include <axl/axl-driver.h>
#include <axl/axl-sys.h>     /* AxlGuid, AXL_GUID */
#include <axl/axl-log.h>
#include <axl/axl-str.h>     /* axl_utf8_to_ucs2 */
#include <axl/axl-mem.h>     /* axl_free */
#include "../backend/axl-backend.h"   /* axl_bs() (gBS) + UEFI types/GUIDs; axl_backend_shell_execute */
#include "../fv/axl-fv-internal.h"

AXL_LOG_DOMAIN("shell");

extern EFI_HANDLE gImageHandle;   /* set by the runtime at startup */

/* Well-known UEFI Shell FFS file name GUIDs to look for in firmware
   volumes, in search-preference order:
   1. gUefiShellFileGuid — the GUID EDK2 platform FDFs (OVMF, AAVMF, most
      vendor firmware) name the embedded Shell FV file with. Empirically the
      one OVMF/AAVMF carry.
   2. ShellPkg Shell.inf FILE_GUID — what a firmware that embeds the Shell
      *module* directly (not via gUefiShellFileGuid) names it; also the
      FILE_GUID a standalone Shell.efi binary carries. */
static const AxlGuid SHELL_FV_GUIDS[] = {
    AXL_GUID(0x7C04A583, 0x9E3E, 0x4F1C,
             0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1),
    AXL_GUID(0xEA4BB293, 0x2D7F, 0x4456,
             0xA6, 0x81, 0x1F, 0x22, 0xF4, 0x2C, 0xD0, 0xBC),
};
#define SHELL_FV_GUID_COUNT (sizeof(SHELL_FV_GUIDS) / sizeof(SHELL_FV_GUIDS[0]))

int
axl_shell_launch(
    int *out_exit_code
    )
{
    char path[256];
    if (axl_driver_locate("Shell.efi", path, sizeof(path)) != AXL_OK) {
        axl_debug("shell launch: Shell.efi not found");
        if (out_exit_code != NULL) {
            *out_exit_code = 0;
        }
        return AXL_ERR;
    }
    /* -nostartup: the launcher is typically itself started from startup.nsh;
       without this the child Shell would re-run startup.nsh and recurse. */
    return axl_image_run(path, "-nostartup", out_exit_code);
}

int
axl_shell_launch_fv(
    const char *load_options,
    int        *out_exit_code
    )
{
    if (out_exit_code != NULL) {
        *out_exit_code = 0;
    }
    /* Try each known Shell file GUID in turn. axl_image_run_fv_file returns
       AXL_ERR when the GUID isn't carried by any readable FV (move on to the
       next); on the first one that loads it StartImages the Shell, blocks
       until it exits, and returns AXL_OK. */
    for (size_t i = 0; i < SHELL_FV_GUID_COUNT; i++) {
        if (axl_image_run_fv_file(&SHELL_FV_GUIDS[i], load_options,
                                  out_exit_code) == AXL_OK) {
            return AXL_OK;
        }
    }
    axl_debug("shell launch_fv: no firmware-embedded Shell in a readable FV");
    return AXL_ERR;
}

int
axl_shell_execute(
    const char *cmd
    )
{
    if (cmd == NULL) {
        return AXL_ERR;
    }
    unsigned short *ucs2 = axl_utf8_to_ucs2(cmd);
    if (ucs2 == NULL) {
        return AXL_ERR;
    }
    int rc = axl_backend_shell_execute(ucs2);
    axl_free(ucs2);
    return rc;
}

/* The Shell's "not found" status (SHELL_NOT_FOUND), as %lasterror% holds it. */
#define SHELL_LASTERROR_NOT_FOUND 0xEu
#define UEFI_ERROR_BIT            0x8000000000000000ULL

static const char *
skip_blanks(
    const char *s
    )
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/* Copy @p line's first word into @p out as the Shell parses it
   (GetNextParameter): it ends at a blank outside quotes, every unescaped `"`
   is dropped wherever it stands, and `^` takes the next character literally --
   so `"exit"`, `ex"it"` and `e^xit` are all exit, and `"exit"5` is exit5.
   Empty when there is none. No `@` handling: that prefix is a script's, and
   Execute runs neither `@x` nor `@ x` (measured: both not found). */
static void
first_word(
    const char *line,
    char       *out,
    size_t      out_size
    )
{
    line = skip_blanks(line);
    bool   quoted = false;
    size_t n      = 0;
    for (; *line != '\0'; line++) {
        if (*line == '^' && line[1] != '\0') {
            line++;                       /* the escaped character, as itself */
        } else if (*line == '"') {
            quoted = !quoted;
            continue;
        } else if (!quoted && (*line == ' ' || *line == '\t')) {
            break;
        }
        if (n + 1 < out_size) {
            out[n++] = *line;
        }
    }
    out[n] = '\0';
}

/* The UTF-8 value of a Shell alias, or NULL; caller frees. */
static char *
alias_value(
    const char *name
    )
{
    unsigned short *w = axl_utf8_to_ucs2(name);
    if (w == NULL) {
        return NULL;
    }
    const unsigned short *v = axl_backend_shell_get_alias(w);
    axl_free(w);
    return v != NULL ? axl_ucs2_to_utf8(v) : NULL;
}

/* `exit`, typed or behind an alias: in a -nonest Shell it ends the Shell
   hosting the caller while reporting 0. */
static bool
is_exit(
    const char *line
    )
{
    char word[64];
    first_word(line, word, sizeof(word));
    if (axl_strcasecmp(word, "exit") == 0) {
        return true;
    }
    if (word[0] == '\0') {
        return false;
    }
    char *alias = alias_value(word);
    if (alias == NULL) {
        return false;
    }
    char target[64];
    first_word(alias, target, sizeof(target));
    axl_free(alias);
    return axl_strcasecmp(target, "exit") == 0;
}

/* A Shell variable read as a hex number ("0xE"); false when unset or not a
   number. */
static bool
shell_env_hex(
    const unsigned short *name,
    uint64_t             *out
    )
{
    const unsigned short *v = axl_backend_shell_getenv(name);
    if (v == NULL) {
        return false;
    }
    char *u = axl_ucs2_to_utf8(v);
    if (u == NULL) {
        return false;
    }
    const char *digits = u;
    if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        digits += 2;
    }
    size_t len = axl_strlen(digits);
    bool ok = len > 0 && axl_hex_parse_u64(digits, len, out) == (int)len;
    axl_free(u);
    return ok;
}

static bool
shell_env_true(
    const unsigned short *name
    )
{
    const unsigned short *v = axl_backend_shell_getenv(name);
    if (v == NULL) {
        return false;
    }
    char *u = axl_ucs2_to_utf8(v);
    bool t = u != NULL && axl_strcasecmp(u, "True") == 0;
    axl_free(u);
    return t;
}

/* Set %lasterror% to 0 with a quiet `cd .` -- the variable cannot be written,
   and `cd` is the one silent command that resets it. Not when there is no
   current directory (cd would fail, and print) or `cd` is an alias (it would
   run something else). True only if it then reads 0. */
static bool
reset_lasterror(void)
{
    if (axl_backend_shell_getcwd() == NULL) {
        return false;
    }
    char *cd_alias = alias_value("cd");
    if (cd_alias != NULL) {
        axl_free(cd_alias);
        return false;
    }
    if (axl_backend_shell_execute_quiet(u"cd .") != AXL_OK) {
        return false;
    }
    uint64_t le = 1;
    return shell_env_hex(u"lasterror", &le) && le == 0;
}

/* The outcome of a line a -nonest Shell ran, from what it reported -- the
   header's table, row by row. */
static AxlShellOutcome
nonest_outcome(
    const AxlShellResult *r,
    bool                  have_lasterror,
    uint64_t              lasterror
    )
{
    bool exec_failed = (r->execute_status & UEFI_ERROR_BIT) != 0;
    if (r->status_written) {
        /* A written status without the error bit beside a failed Execute is
           a load error: the image never ran. */
        bool load_error = exec_failed && (r->status & UEFI_ERROR_BIT) == 0;
        return load_error ? AXL_SHELL_OUTCOME_NOT_RUN : AXL_SHELL_OUTCOME_RAN;
    }
    if (exec_failed) {
        return AXL_SHELL_OUTCOME_NOT_RUN;   /* refused before running */
    }
    if (!r->reset_ok || !have_lasterror) {
        return AXL_SHELL_OUTCOME_UNKNOWN;
    }
    return lasterror == SHELL_LASTERROR_NOT_FOUND ? AXL_SHELL_OUTCOME_NOT_RUN
                                                  : AXL_SHELL_OUTCOME_RAN;
}

int
axl_shell_execute_status(
    const char     *cmd,
    AxlShellResult *out
    )
{
    if (cmd == NULL || out == NULL) {
        return AXL_ERR;
    }
    if (*skip_blanks(cmd) == '\0') {
        return AXL_ERR;   /* a nested Shell handed this starts a whole session */
    }
    if (is_exit(cmd)) {
        return AXL_DENIED;   /* the line itself: a caller can tell it from a failure */
    }
    AxlShellKind kind = axl_shell_kind();
    if (kind == AXL_SHELL_KIND_NONE) {
        return AXL_ERR;
    }
    unsigned short *ucs2 = axl_utf8_to_ucs2(cmd);
    if (ucs2 == NULL) {
        return AXL_ERR;
    }

    AxlShellResult r;
    axl_memset(&r, 0, sizeof(r));   /* outcome UNKNOWN */

    if (kind == AXL_SHELL_KIND_EFI_1X) {
        /* SHELL_ENVIRONMENT.Execute returns the command's status and a
           dispatch failure through the same value, so even its error says
           nothing about whether the line ran: UNKNOWN either way. */
        axl_backend_shell_execute(ucs2);
        axl_free(ucs2);
        *out = r;
        return AXL_OK;
    }

    r.nonest = shell_env_true(u"nonesting");
    if (r.nonest) {
        r.reset_ok = reset_lasterror();
    }
    uint64_t status = 0;
    int rc = axl_backend_shell_execute_status(ucs2, &status, &r.status_written,
                                              &r.execute_status);
    axl_free(ucs2);
    if (rc != AXL_OK) {
        return AXL_ERR;
    }
    uint64_t lasterror = 0;
    bool have_le = r.nonest && shell_env_hex(u"lasterror", &lasterror);
    r.status  = r.status_written ? status : lasterror;
    r.outcome = r.nonest ? nonest_outcome(&r, have_le, lasterror)
                         : AXL_SHELL_OUTCOME_UNKNOWN;
    *out = r;
    return AXL_OK;
}

int
axl_shell_sources(
    AxlShellSources *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    axl_memset(out, 0, sizeof(*out));

    /* File source: a locatable `Shell.efi` (the axl_shell_launch search path).
       axl_driver_locate fills out->file_path on success; force it empty on
       failure so a partial write can't masquerade as a path. */
    out->file = (axl_driver_locate("Shell.efi", out->file_path,
                                   sizeof(out->file_path)) == AXL_OK);
    if (!out->file) {
        out->file_path[0] = '\0';
    }

    /* FV source: count how many of the known Shell file GUIDs a readable FV
       carries. Reported INDEPENDENTLY of the file above — an FV-first consumer
       can prefer the firmware Shell even when a foreign Shell.efi also exists
       (the case the file-first axl_shell_locate masks). */
    for (size_t i = 0; i < SHELL_FV_GUID_COUNT; i++) {
        AxlHandle fv = NULL;
        if (_axl_fv_find_app_file(&SHELL_FV_GUIDS[i], &fv) == AXL_OK) {
            out->fv_count++;
        }
    }
    out->fv = (out->fv_count > 0);

    return AXL_OK;
}

AxlShellSource
axl_shell_locate(void)
{
    /* The file-first single verdict is exactly the file-first projection of
       the independent sources — one code path keeps the two APIs consistent.
       (The extra FV scan when a file exists is cheap: it walks firmware-volume
       headers in memory, while the file search that both share walks mounted
       filesystems and dominates the cost.) */
    AxlShellSources s;
    if (axl_shell_sources(&s) != AXL_OK) {
        return AXL_SHELL_NONE;
    }
    if (s.file) {
        return AXL_SHELL_FILE;
    }
    return s.fv ? AXL_SHELL_FIRMWARE : AXL_SHELL_NONE;
}

AxlShellKind
axl_shell_kind(void)
{
    void *iface = NULL;

    /* Modern EDK2 UEFI Shell first: it never publishes the legacy EFI 1.x
       GUIDs, so a hit here is unambiguous and needs no further probing. */
    EFI_GUID uefi_guid = gEfiShellProtocolGuid;
    if (!EFI_ERROR(axl_bs()->LocateProtocol(&uefi_guid, NULL, &iface))
        && iface != NULL) {
        return AXL_SHELL_KIND_UEFI;
    }

    /* Old EFI 1.x shell: SHELL_ENVIRONMENT is installed globally, so a plain
       LocateProtocol finds it whenever that shell is present. */
    EFI_GUID env_guid = gEfiShellEnvironmentGuid;
    iface = NULL;
    if (!EFI_ERROR(axl_bs()->LocateProtocol(&env_guid, NULL, &iface))
        && iface != NULL) {
        return AXL_SHELL_KIND_EFI_1X;
    }

    /* Fallback: the old shell also installs SHELL_INTERFACE on each
       shell-launched image handle — so probe our own handle in case the
       global environment protocol isn't locatable on a given build. */
    if (gImageHandle != NULL) {
        EFI_GUID if_guid = gEfiShellInterfaceGuid;
        iface = NULL;
        if (!EFI_ERROR(axl_bs()->HandleProtocol(gImageHandle, &if_guid, &iface))
            && iface != NULL) {
            return AXL_SHELL_KIND_EFI_1X;
        }
    }

    return AXL_SHELL_KIND_NONE;
}
