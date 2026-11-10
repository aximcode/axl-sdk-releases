/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* stderr-row.c -- writes standard output and standard error the way a tool
 * does, interleaved, for test-stderr-row-qemu.sh to read off the screen a
 * person sees: a partial stdout line finished by stdout, a stderr line after
 * a whole stdout line, a partial stdout line finished by stderr, a log line,
 * and -- after main returns -- the teardown's leak verdict.
 *
 * `stderr-row.efi size` prints the console's text size instead, as
 * `SR-SIZE <columns>x<rows>`, so the test draws its terminal at that size.
 * `stderr-row.efi many` writes 70 stderr lines with the Shell's page break
 * on. `stderr-row.efi wrap` writes lines measured against the console's
 * width (see wrap_lines). `stderr-row.efi devices` prints each ConOut
 * device's geometry and cursor (see devices); `stderr-row.efi fakes`
 * drives the line writer into fake standard-error devices (see fakes);
 * `stderr-row.efi rows` checks every ConOut device's cursor after a line
 * exactly as wide as the narrowest one (see rows_check); `stderr-row.efi
 * history` writes lines for the Shell's scroll-back (see history_lines);
 * `stderr-row.efi xstream` ends a full stdout line on stderr (see
 * cross_stream). */
#include <axl.h>
#include <uefi/axl-uefi.h>   /* devices: the console's own devices */

AXL_LOG_DOMAIN("stderr-row");

/* What a log line puts before its message: `HH:MM:SS.uuuuuu [ERROR] `
   and the domain's `stderr-row: `. */
#define SR_LOG_PREFIX_LEN  36

/* @p tag, then dots up to @p len characters, in @p buf (room for 255). */
static const char *
wrap_text(
    char        *buf,
    const char  *tag,
    size_t       len
    )
{
    size_t n = axl_strlen(tag);
    if (len > 255) {
        len = 255;
    }
    axl_memcpy(buf, tag, n < len ? n : len);
    for (size_t i = n; i < len; i++) {
        buf[i] = '.';
    }
    buf[len] = '\0';
    return buf;
}

/* Lines measured against the console's width W: exactly W, W - 1, W + 1,
   two of exactly W in a row, exactly W on standard error, a log line
   exactly W wide, exactly W with its newline in a write of its own, and
   exactly W followed by an empty line. A UEFI console wraps the moment a
   character lands in its last column, so an exactly-W line must not leave
   an empty row after it -- and the empty line after the last one must
   still be one, on the serial terminal too, whose own wrap is deferred.
   Each other line is one write, its newline included, as a tool writes
   it. */
static int
wrap_lines(
    void
    )
{
    uint32_t           index = 0;
    AxlConsoleTextMode mode;
    char               buf[256];
    if (axl_console_text_current_mode(&index) != AXL_OK
        || axl_console_text_query_mode(index, &mode) != AXL_OK
        || mode.columns <= SR_LOG_PREFIX_LEN + 8 || mode.columns > 250) {
        axl_print("SR-WRAP no console size\n");
        return 1;
    }
    size_t w = mode.columns;
    axl_printf("%s\n", wrap_text(buf, "SR-W1", w));
    axl_printf("%s\n", wrap_text(buf, "SR-W2", w - 1));
    axl_printf("%s\n", wrap_text(buf, "SR-W3", w + 1));
    axl_printf("%s\n", wrap_text(buf, "SR-W4", w));
    axl_printf("%s\n", wrap_text(buf, "SR-W5", w));
    axl_printerr("%s\n", wrap_text(buf, "SR-W6", w));
    axl_error("%s", wrap_text(buf, "SR-W7", w - SR_LOG_PREFIX_LEN));
    axl_print("%s", wrap_text(buf, "SR-W8", w));
    axl_print("\n");
    axl_printf("%s\n\n", wrap_text(buf, "SR-W9", w));
    axl_print("SR-W-END\n");
    return 0;
}

/* One line per ConOut device: `SR-DEV <i> <cols>x<rows> gop=<0|1>
   col=<cursor column>`, so a test can see which devices run which geometry. */
static int
devices(
    void
    )
{
    EFI_HANDLE *handles = NULL;
    UINTN       count   = 0;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &gEfiConsoleOutDeviceGuid, NULL,
                                          &count, &handles))) {
        axl_print("SR-DEV none\n");
        return 1;
    }
    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out  = NULL;
        void                            *gop  = NULL;
        UINTN                            cols = 0;
        UINTN                            rows = 0;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &gEfiSimpleTextOutputProtocolGuid,
                                          (void **)&out))
            || out == NULL || out->Mode == NULL) {
            continue;
        }
        out->QueryMode(out, (UINTN)out->Mode->Mode, &cols, &rows);
        bool screen = !EFI_ERROR(gBS->HandleProtocol(handles[i],
                                                     &gEfiGraphicsOutputProtocolGuid, &gop));
        axl_printf("SR-DEV %u %ux%u gop=%d col=%d\n", (unsigned)i, (unsigned)cols,
                   (unsigned)rows, screen ? 1 : 0, (int)out->Mode->CursorColumn);
    }
    gBS->FreePool(handles);
    return 0;
}

// ---------------------------------------------------------------------------
// Fake standard-error devices: a SimpleTextOut tagged as a StdErr device and
// not a ConOut one, so AXL writes each standard-error line to it directly,
// with its handle -- whose device path names the terminal type. It keeps a
// UEFI console's cursor (wrap the moment a character lands in the last
// column) unless frozen, and logs what it is sent: characters as they are,
// a cursor move as `{col,row}`.
// ---------------------------------------------------------------------------

#define FAKE_COLS  30
#define FAKE_ROWS  10

typedef struct {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL proto;   /* first: the protocol is the fake */
    SIMPLE_TEXT_OUTPUT_MODE         mode;
    bool                            frozen;
    char                            log[256];
    size_t                          len;
    EFI_HANDLE                      handle;
    uint8_t                         path[24];   /* one vendor node + the end */
} FakeOut;

static void
fake_log(
    FakeOut     *f,
    const char  *s
    )
{
    for (; *s != '\0' && f->len + 1 < sizeof(f->log); s++) {
        f->log[f->len++] = *s;
    }
    f->log[f->len] = '\0';
}

static EFI_STATUS EFIAPI
fake_output(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    CHAR16                          *String
    )
{
    FakeOut *f = (FakeOut *)This;
    for (; *String != 0; String++) {
        char c[2] = { (char)*String, '\0' };
        fake_log(f, c);
        if (f->frozen) {
            continue;
        }
        if (*String == L'\r') {
            f->mode.CursorColumn = 0;
        } else if (*String == L'\n') {
            if (f->mode.CursorRow < FAKE_ROWS - 1) {
                f->mode.CursorRow++;
            }
        } else if (*String == L'\b') {
            if (f->mode.CursorColumn > 0) {
                f->mode.CursorColumn--;
            }
        } else if (++f->mode.CursorColumn >= FAKE_COLS) {
            f->mode.CursorColumn = 0;
            if (f->mode.CursorRow < FAKE_ROWS - 1) {
                f->mode.CursorRow++;
            }
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
fake_cursor(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINTN                            Column,
    UINTN                            Row
    )
{
    FakeOut *f = (FakeOut *)This;
    char     b[24];
    axl_snprintf(b, sizeof(b), "{%u,%u}", (unsigned)Column, (unsigned)Row);
    fake_log(f, b);
    if (!f->frozen) {
        f->mode.CursorColumn = (INT32)Column;
        f->mode.CursorRow    = (INT32)Row;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
fake_query(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINTN                            ModeNumber,
    UINTN                           *Columns,
    UINTN                           *Rows
    )
{
    (void)This;
    (void)ModeNumber;
    *Columns = FAKE_COLS;
    *Rows    = FAKE_ROWS;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
fake_attr(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    UINTN                            Attribute
    )
{
    ((FakeOut *)This)->mode.Attribute = (INT32)Attribute;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
fake_clear(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This
    )
{
    (void)This;
    return EFI_SUCCESS;
}

/* Install @p f as a standard-error device whose path ends in a vendor node
   naming @p type (NULL: no vendor node), its cursor at (0, 2). */
static bool
fake_install(
    FakeOut         *f,
    const uint8_t   *type,
    bool             frozen
    )
{
    axl_memset(f, 0, sizeof(*f));
    f->proto.OutputString      = fake_output;
    f->proto.SetCursorPosition = fake_cursor;
    f->proto.QueryMode         = fake_query;
    f->proto.SetAttribute      = fake_attr;
    f->proto.ClearScreen       = fake_clear;
    f->proto.Mode              = &f->mode;
    f->mode.MaxMode            = 1;
    f->mode.Attribute          = 0x07;
    f->mode.CursorRow          = 2;
    f->frozen                  = frozen;
    uint8_t *p = f->path;
    if (type != NULL) {
        p[0] = MESSAGING_DEVICE_PATH;
        p[1] = MSG_VENDOR_DP;
        p[2] = 20;
        axl_memcpy(p + 4, type, 16);
        p += 20;
    }
    p[0] = END_DEVICE_PATH_TYPE;
    p[1] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    p[2] = 4;
    return !EFI_ERROR(gBS->InstallMultipleProtocolInterfaces(
        &f->handle,
        &gEfiSimpleTextOutputProtocolGuid, &f->proto,
        &gEfiStandardErrorDeviceGuid,      NULL,
        &gEfiDevicePathProtocolGuid,       f->path,
        NULL));
}

static void
fake_remove(
    FakeOut  *f
    )
{
    gBS->UninstallMultipleProtocolInterfaces(
        f->handle,
        &gEfiSimpleTextOutputProtocolGuid, &f->proto,
        &gEfiStandardErrorDeviceGuid,      NULL,
        &gEfiDevicePathProtocolGuid,       f->path,
        NULL);
}

/* `SR-FAKE <name> ok`, or `bad` and what the fake was sent. */
static void
fake_verdict(
    const char  *name,
    FakeOut     *f,
    const char  *want
    )
{
    char   shown[512];
    size_t k = 0;
    for (size_t i = 0; i < f->len && k + 5 < sizeof(shown); i++) {
        unsigned char c = (unsigned char)f->log[i];
        if (c < 0x20) {
            k += (size_t)axl_snprintf(shown + k, sizeof(shown) - k, "<%02x>", c);
        } else {
            shown[k++] = (char)c;
        }
    }
    shown[k] = '\0';
    if (axl_strcmp(f->log, want) == 0) {
        axl_printf("SR-FAKE %s ok\n", name);
    } else {
        axl_printf("SR-FAKE %s bad %s\n", name, shown);
    }
}

/* The terminal types: EDK2 TerminalDxe's eight that never send their own
   wrap down the wire (the four UEFI ones, then Linux, Xterm-R6, VT400 and
   SCO from MdeModulePkg), then TtyTerm, which does. */
static const struct {
    const char *name;
    uint8_t     guid[16];
    bool        deferred;
} kTypes[] = {
    { "pcansi",  { 0x53, 0x47, 0xc1, 0xe0, 0xbe, 0xf9, 0xd2, 0x11,
                   0x9a, 0x0c, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d }, true },
    { "vt100",   { 0x65, 0x60, 0xa6, 0xdf, 0x19, 0xb4, 0xd3, 0x11,
                   0x9a, 0x2d, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d }, true },
    { "vt100+",  { 0x0b, 0xc7, 0xae, 0x7b, 0xe0, 0x57, 0x76, 0x4c,
                   0x8e, 0x87, 0x2f, 0x9e, 0x28, 0x08, 0x83, 0x43 }, true },
    { "vtutf8",  { 0xd6, 0xa0, 0x15, 0xad, 0xec, 0x8b, 0xcf, 0x4a,
                   0xa0, 0x73, 0xd0, 0x1d, 0xe7, 0x7e, 0x2d, 0x88 }, true },
    { "linux",   { 0x7f, 0x4a, 0x36, 0xe4, 0x25, 0xf8, 0x0e, 0x43,
                   0x9d, 0x3a, 0x9c, 0x9b, 0xe6, 0x81, 0x7c, 0xa5 }, true },
    { "xtermr6", { 0x6b, 0xa5, 0xfc, 0xfb, 0x36, 0xbb, 0x78, 0x4b,
                   0xaa, 0xab, 0xbe, 0x1b, 0x97, 0xec, 0x7c, 0xcb }, true },
    { "vt400",   { 0xdd, 0xdd, 0x46, 0x8e, 0x49, 0x3d, 0x9d, 0x4a,
                   0xb8, 0x75, 0x3c, 0x08, 0x6f, 0x6a, 0xa2, 0xbd }, true },
    { "sco",     { 0xe0, 0xd6, 0x7d, 0xfc, 0x3c, 0x81, 0x4d, 0x43,
                   0xb4, 0xda, 0x3b, 0xd6, 0x49, 0xe9, 0xe1, 0x5a }, true },
    { "ttyterm", { 0x80, 0x6d, 0x91, 0x7d, 0xb1, 0x5b, 0x8c, 0x45,
                   0xa4, 0x8f, 0xe2, 0x5f, 0xdd, 0x51, 0xef, 0x94 }, false },
};

/* Each case writes one standard-error line exactly the fake's width (or
   its pieces) and checks what the fake was sent. */
static int
fakes(
    void
    )
{
    static FakeOut f;
    static FakeOut g;
    char           line[FAKE_COLS + 1];
    char           want[96];
    for (int i = 0; i < FAKE_COLS; i++) {
        line[i] = 'F';
    }
    line[FAKE_COLS] = '\0';

    /* The break after a wrap, per terminal type: CR LF and the cursor back
       on the row the wrap left (2 + 1) where the terminal still has to be
       told; a space and a backspace where nothing has to move. */
    for (size_t t = 0; t < sizeof(kTypes) / sizeof(kTypes[0]); t++) {
        if (!fake_install(&f, kTypes[t].guid, false)) {
            axl_printf("SR-FAKE %s install-failed\n", kTypes[t].name);
            continue;
        }
        axl_printerr("%s\n", line);
        fake_remove(&f);
        axl_snprintf(want, sizeof(want), kTypes[t].deferred ? "%s\r\n{0,3}" : "%s \b", line);
        fake_verdict(kTypes[t].name, &f, want);
    }
    if (fake_install(&f, NULL, false)) {
        axl_printerr("%s\n", line);
        fake_remove(&f);
        axl_snprintf(want, sizeof(want), "%s \b", line);
        fake_verdict("no-type", &f, want);
    }

    /* A CR after the wrap, its LF in the next write: one break, and the
       CR has already ended the wrap, so nothing more on a device that is
       not a TerminalDxe terminal. */
    if (fake_install(&f, NULL, false)) {
        axl_printerr("%s\r", line);
        axl_printerr("\n");
        fake_remove(&f);
        axl_snprintf(want, sizeof(want), "%s\r", line);
        fake_verdict("cr-then-lf", &f, want);
    }

    /* A device that ended on a wrap is gone, and a new one -- another
       protocol, with a frozen cursor -- has the same Mode memory and stands
       where the wrap left the old one: its newline is its own, CR LF. */
    if (fake_install(&f, NULL, false)) {
        axl_printerr("%s", line);
        fake_remove(&f);
        if (fake_install(&g, NULL, true)) {
            g.proto.Mode = &f.mode;   /* the old device's Mode, as reused memory */
            axl_printerr("\n");
            fake_remove(&g);
            fake_verdict("reused-mode", &g, "\r\n");
        }
    }
    return 0;
}

/* The ConOut devices (up to 8) in @p dev, their widths in @p cols, their
   count in @p n. Returns the narrowest width, 0 when there is none. */
static UINTN
conout_devices(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL **dev,
    UINTN                            *cols,
    UINTN                            *n
    )
{
    EFI_HANDLE *handles = NULL;
    UINTN       count   = 0;
    UINTN       narrow  = 0;
    *n = 0;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &gEfiConsoleOutDeviceGuid, NULL,
                                          &count, &handles))) {
        return 0;
    }
    for (UINTN i = 0; i < count && *n < 8; i++) {
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = NULL;
        UINTN                            r   = 0;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &gEfiSimpleTextOutputProtocolGuid,
                                          (void **)&out))
            || out == NULL || out->Mode == NULL) {
            continue;
        }
        out->QueryMode(out, (UINTN)out->Mode->Mode, &cols[*n], &r);
        dev[*n] = out;
        if (narrow == 0 || cols[*n] < narrow) {
            narrow = cols[*n];
        }
        (*n)++;
    }
    gBS->FreePool(handles);
    return narrow;
}

/* A line exactly as wide as the NARROWEST ConOut device, from column 0:
   on every device the cursor must end one row down at column 0 -- the
   device it wrapped as well as the wider ones it did not. Prints one
   `SR-ROWS <i> <cols> drow=<d> col=<c>` per device, then `SR-ROWS ok` or
   `SR-ROWS bad`. Then the console's own cursor record (gST->ConOut's Mode,
   the splitter's, which copies its first device's cursor) must stand at
   column 0 on the row every device is on: `SR-ROWS-SPLITTER ok`, or `bad`
   and where it is. Run from a cleared screen, so no row is clamped at the bottom. */
static int
rows_check(
    void
    )
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *dev[8];
    UINTN                            cols[8];
    INT32                            row0[8];
    UINTN                            n      = 0;
    UINTN                            narrow = conout_devices(dev, cols, &n);
    char                             line[256];
    if (n == 0 || narrow == 0 || narrow >= sizeof(line)) {
        axl_print("SR-ROWS bad no devices\n");
        return 1;
    }
    for (UINTN i = 0; i < n; i++) {
        row0[i] = dev[i]->Mode->CursorRow;
    }
    axl_printf("%s\n", wrap_text(line, "SR-ROWS-LINE", narrow));
    bool ok = true;
    INT32 drow[8];
    INT32 col[8];
    for (UINTN i = 0; i < n; i++) {
        drow[i] = dev[i]->Mode->CursorRow - row0[i];
        col[i]  = dev[i]->Mode->CursorColumn;
        ok      = ok && drow[i] == 1 && col[i] == 0;
    }
    INT32 scol  = gST->ConOut->Mode->CursorColumn;
    INT32 srow  = gST->ConOut->Mode->CursorRow;
    /* Every device at column 0 of the SAME row (from a cleared screen each
       is one row down), and the record there too -- so matching "a device"
       is matching the splitter's first one, whichever that is. */
    bool  split = scol == 0;
    for (UINTN i = 0; i < n; i++) {
        split = split && srow == dev[i]->Mode->CursorRow && dev[i]->Mode->CursorColumn == 0;
    }
    for (UINTN i = 0; i < n; i++) {
        axl_printf("SR-ROWS %u %u drow=%d col=%d\n", (unsigned)i, (unsigned)cols[i],
                   (int)drow[i], (int)col[i]);
    }
    axl_printf("SR-ROWS %s\n", ok ? "ok" : "bad");
    if (split) {
        axl_print("SR-ROWS-SPLITTER ok\n");
    } else {
        axl_printf("SR-ROWS-SPLITTER bad col=%d row=%d\n", (int)scol, (int)srow);
    }
    return ok && split ? 0 : 1;
}

/* Lines for the Shell's scroll-back, which keeps its own copy of the
   screen: 10 short ones, one exactly as wide as the narrowest ConOut device
   (which wraps it where a wider first device does not), `SR-H-NEXT`, 20
   more, `SR-H-END`. The test pages back and reads the copy off the
   display. In all, a few rows more than a 31-row screen, so Page Up has
   history to page to (it pages half a screen, or to the top) and the wide
   line lands near the top of the redrawn page, nowhere near its edges. */
static int
history_lines(
    void
    )
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *dev[8];
    UINTN                            cols[8];
    UINTN                            n      = 0;
    UINTN                            narrow = conout_devices(dev, cols, &n);
    char                             line[256];
    if (narrow == 0 || narrow >= sizeof(line)) {
        axl_print("SR-H no devices\n");
        return 1;
    }
    for (unsigned i = 1; i <= 10; i++) {
        axl_printf("SR-H %02u\n", i);
    }
    axl_printf("%s\n", wrap_text(line, "SR-H-WIDE", narrow));
    axl_print("SR-H-NEXT\n");
    for (unsigned i = 11; i <= 30; i++) {
        axl_printf("SR-H %02u\n", i);
    }
    axl_print("SR-H-END\n");
    return 0;
}

/* A line exactly the console's width on standard output with no newline,
   its newline on standard error, then `SR-X-NEXT`: on a terminal the two
   streams share one cursor, so the newline ends the line the output
   wrapped -- no empty row. They are two outputs over one console (the
   Shell's logger and the splitter beneath it, which share a Mode). */
static int
cross_stream(
    void
    )
{
    uint32_t           index = 0;
    AxlConsoleTextMode mode;
    char               buf[256];
    if (axl_console_text_current_mode(&index) != AXL_OK
        || axl_console_text_query_mode(index, &mode) != AXL_OK
        || mode.columns < 16 || mode.columns > 250) {
        axl_print("SR-X no console size\n");
        return 1;
    }
    axl_print("%s", wrap_text(buf, "SR-X", mode.columns));
    axl_printerr("\n");
    axl_print("SR-X-NEXT\n");
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc > 1 && axl_strcmp(argv[1], "size") == 0) {
        uint32_t           index = 0;
        AxlConsoleTextMode mode;
        if (axl_console_text_current_mode(&index) != AXL_OK
            || axl_console_text_query_mode(index, &mode) != AXL_OK) {
            axl_print("SR-SIZE unknown\n");
            return 1;
        }
        axl_printf("SR-SIZE %ux%u\n", (unsigned)mode.columns, (unsigned)mode.rows);
        return 0;
    }
    if (argc > 1 && axl_strcmp(argv[1], "xstream") == 0) {
        return cross_stream();
    }
    if (argc > 1 && axl_strcmp(argv[1], "history") == 0) {
        return history_lines();
    }
    if (argc > 1 && axl_strcmp(argv[1], "rows") == 0) {
        return rows_check();
    }
    if (argc > 1 && axl_strcmp(argv[1], "fakes") == 0) {
        return fakes();
    }
    if (argc > 1 && axl_strcmp(argv[1], "devices") == 0) {
        return devices();
    }
    if (argc > 1 && axl_strcmp(argv[1], "wrap") == 0) {
        return wrap_lines();
    }
    if (argc > 1 && axl_strcmp(argv[1], "many") == 0) {
        /* More stderr lines than a screen holds, with the Shell's page
           break on: nothing may stop them to wait for a key. */
        axl_console_set_page_break(true);
        for (unsigned i = 1; i <= 70; i++) {
            axl_printerr("SR-MANY %u\n", i);
        }
        axl_console_set_page_break(false);
        return 0;
    }
    axl_print("SR-partial ");
    axl_print("SR-line\n");
    axl_printerr("SR-err-1\n");
    axl_print("SR-out-2\n");
    axl_print("SR-mix ");
    axl_printerr("SR-err-2\n");
    axl_error("SR-log-1");
    axl_print("SR-out-3\n");
    return 0;
}
