/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-json-print.c
    Colorized JSON pretty-printer for UEFI console.
    Migrated from SoftBmc's JsonPrint.c.
**/

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "../backend/axl-backend.h"
#include <axl/axl-json.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("json");

// ---------------------------------------------------------------------------
// Color Constants (UEFI SimpleTextOutput attribute values)
// ---------------------------------------------------------------------------

#define JP_COLOR_KEY      0x03  /* EFI_CYAN */
#define JP_COLOR_STRING   0x02  /* EFI_GREEN */
#define JP_COLOR_NUMBER   0x0E  /* EFI_YELLOW */
#define JP_COLOR_BOOL     0x05  /* EFI_MAGENTA */
#define JP_COLOR_PUNCT    0x07  /* EFI_LIGHTGRAY */
#define JP_COLOR_DEFAULT  0x07  /* EFI_LIGHTGRAY */
#define JP_BG             0x00  /* EFI_BACKGROUND_BLACK */

// ---------------------------------------------------------------------------
// Internal Helpers
// ---------------------------------------------------------------------------

static uint32_t  mOrigAttr;

static void
set_color(uint32_t fg)
{
    axl_backend_console_set_attr(fg | JP_BG);
}

static void
restore_color(void)
{
    axl_backend_console_set_attr(mOrigAttr);
}

static void
jprint_char(char ch)
{
    unsigned short buf[2];

    buf[0] = (unsigned short)(unsigned char)ch;
    buf[1] = 0;
    axl_backend_console_write(buf);
}

static void
jprint_indent(int depth)
{
    int i;

    for (i = 0; i < depth; i++) {
        axl_backend_console_write((const unsigned short *)L"  ");
    }
}

static void
jprint_newline(void)
{
    axl_backend_console_write((const unsigned short *)L"\r\n");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void
axl_json_console_print(const char *json, size_t len)
{
    int      depth;
    bool     in_string;
    bool     escaped;
    bool     expect_key;
    bool     obj_stack[64];
    int      stack_top;
    size_t   i;
    char     ch;

    if (json == NULL || len == 0) {
        return;
    }

    mOrigAttr = axl_backend_console_get_attr();

    depth = 0;
    in_string = false;
    escaped = false;
    expect_key = false;
    stack_top = -1;

    for (i = 0; i < len; i++) {
        ch = json[i];

        if (in_string) {
            if (escaped) {
                jprint_char(ch);
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                jprint_char(ch);
                escaped = true;
                continue;
            }
            if (ch == '"') {
                jprint_char('"');
                set_color(JP_COLOR_DEFAULT);
                in_string = false;
                continue;
            }
            jprint_char(ch);
            continue;
        }

        switch (ch) {
        case '{':
            set_color(JP_COLOR_PUNCT);
            jprint_char('{');
            depth++;
            jprint_newline();
            jprint_indent(depth);
            if (stack_top < 63) {
                obj_stack[++stack_top] = true;
            }
            expect_key = true;
            break;

        case '}':
            depth--;
            jprint_newline();
            jprint_indent(depth);
            set_color(JP_COLOR_PUNCT);
            jprint_char('}');
            if (stack_top >= 0) stack_top--;
            expect_key = false;
            break;

        case '[':
            set_color(JP_COLOR_PUNCT);
            jprint_char('[');
            depth++;
            jprint_newline();
            jprint_indent(depth);
            if (stack_top < 63) {
                obj_stack[++stack_top] = false;
            }
            expect_key = false;
            break;

        case ']':
            depth--;
            jprint_newline();
            jprint_indent(depth);
            set_color(JP_COLOR_PUNCT);
            jprint_char(']');
            if (stack_top >= 0) stack_top--;
            expect_key = false;
            break;

        case ',':
            set_color(JP_COLOR_PUNCT);
            jprint_char(',');
            jprint_newline();
            jprint_indent(depth);
            expect_key = (stack_top >= 0 && obj_stack[stack_top]);
            break;

        case ':':
            set_color(JP_COLOR_PUNCT);
            jprint_char(':');
            jprint_char(' ');
            expect_key = false;
            break;

        case '"':
            if (expect_key) {
                set_color(JP_COLOR_KEY);
                expect_key = false;
            } else {
                set_color(JP_COLOR_STRING);
            }
            jprint_char('"');
            in_string = true;
            escaped = false;
            break;

        case ' ':
        case '\t':
        case '\n':
        case '\r':
            break;

        default:
            if (axl_isdigit((unsigned char)ch) || ch == '-' || ch == '.') {
                set_color(JP_COLOR_NUMBER);
            } else {
                set_color(JP_COLOR_BOOL);
            }
            jprint_char(ch);
            break;
        }
    }

    jprint_newline();
    restore_color();
}
