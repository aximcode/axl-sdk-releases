/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-hii.c
    Unit tests for the AxlHii setup-form reader.

    Runs against the unit OVMF in QEMU, which publishes its own HII form
    sets (the platform / Secure Boot config forms). The enumeration tests
    assert that at least one form set with questions is discovered and
    that a known question projects sanely; the negative-path tests pin the
    argument-validation contract regardless of what firmware publishes.
**/

#include <axl.h>
#include "axl-test.h"
#include "axl-hii-internal.h"   /* HiiFormSet + _axl_hii_parse_form_package seam */

// ---------------------------------------------------------------------------
// Argument-validation contract (holds independent of published firmware).
// ---------------------------------------------------------------------------

static void
test_arg_validation(void)
{
    /* Out-of-range form-set index is rejected. SIZE_MAX is always past
       the end. */
    test_check(axl_hii_formset_get(SIZE_MAX, NULL, 0, NULL, 0, NULL, NULL) == AXL_ERR,
               "hii: formset_get out-of-range index -> AXL_ERR");

    AxlHiiQuestion q;
    test_check(axl_hii_question_get(SIZE_MAX, 0, &q) == AXL_ERR,
               "hii: question_get out-of-range formset -> AXL_ERR");
    test_check(axl_hii_question_get(0, SIZE_MAX, &q) == AXL_ERR,
               "hii: question_get out-of-range question -> AXL_ERR");
    test_check(axl_hii_question_get(0, 0, NULL) == AXL_ERR,
               "hii: question_get NULL out -> AXL_ERR");

    uint64_t v;
    test_check(axl_hii_question_read(SIZE_MAX, 0, &v) == AXL_ERR,
               "hii: question_read out-of-range -> AXL_ERR");
    test_check(axl_hii_question_read(0, 0, NULL) == AXL_ERR,
               "hii: question_read NULL value -> AXL_ERR");
    test_check(axl_hii_question_write(SIZE_MAX, 0, 0) == AXL_ERR,
               "hii: question_write out-of-range -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// Enumeration against live OVMF HII (the unit firmware publishes form sets).
// ---------------------------------------------------------------------------

static void
test_enumeration(void)
{
    test_check(axl_hii_available(),
               "hii: OVMF publishes at least one form set");

    size_t fs_count = axl_hii_formset_count();
    test_check(fs_count > 0, "hii: formset_count > 0");

    /* Walk every form set + question, tallying what we find. The unit
       firmware is OVMF, whose platform/Secure-Boot config forms always
       carry a titled form set with questions, including at least one
       ONE_OF that offers options. */
    size_t titled_formsets = 0;
    size_t total_questions = 0;
    size_t one_of_with_options = 0;
    bool   all_types_valid = true;
    bool   all_widths_valid = true;

    for (size_t f = 0; f < fs_count; f++) {
        char title[128];
        char help[128];
        size_t qcount = 0;
        if (axl_hii_formset_get(f, title, sizeof title,
                                help, sizeof help, NULL, &qcount) != AXL_OK) {
            continue;
        }
        if (title[0] != '\0') {
            titled_formsets++;
        }
        for (size_t q = 0; q < qcount; q++) {
            AxlHiiQuestion question;
            if (axl_hii_question_get(f, q, &question) != AXL_OK) {
                continue;
            }
            total_questions++;
            switch (question.type) {
            case AXL_HII_ONE_OF:
                if (question.u.one_of.option_count > 1) {
                    one_of_with_options++;
                }
                if (question.width != 1 && question.width != 2 &&
                    question.width != 4 && question.width != 8) {
                    all_widths_valid = false;
                }
                break;
            case AXL_HII_CHECKBOX:
                if (question.width != 1) {
                    all_widths_valid = false;
                }
                break;
            case AXL_HII_NUMERIC:
                if (question.width != 1 && question.width != 2 &&
                    question.width != 4 && question.width != 8) {
                    all_widths_valid = false;
                }
                break;
            case AXL_HII_STRING:
                if (question.width != 0) {
                    all_widths_valid = false;
                }
                break;
            default:
                all_types_valid = false;
                break;
            }
        }
    }

    test_check(titled_formsets > 0, "hii: at least one titled form set");
    test_check(total_questions > 0, "hii: at least one question discovered");
    test_check(one_of_with_options > 0,
               "hii: at least one ONE_OF with multiple options");
    test_check(all_types_valid, "hii: every question has a valid type");
    test_check(all_widths_valid, "hii: every question width matches its type");
}

// ---------------------------------------------------------------------------
// Known form set + questions (pins the projection against real IFR).
//
// EDK2's RamDiskDxe publishes the "RAM Disk Configuration" form set on both
// the x64 (OVMF) and aa64 (AAVMF) unit firmware, byte-identically: a ONE_OF
// "Disk Memory Type:" (width 1, 2 options) and a NUMERIC "Size (Hex):"
// (width 8). Looking everything up by exact title/prompt (not index) keeps
// the test robust to form-set ordering and gives cross-arch-balanced
// coverage of both the ONE_OF and NUMERIC projections.
// ---------------------------------------------------------------------------

/* Find a question by exact prompt within a form set; returns true and
   fills @p out on a match. */
static bool
find_question(size_t formset, size_t qcount, const char *prompt,
              AxlHiiQuestion *out)
{
    for (size_t q = 0; q < qcount; q++) {
        if (axl_hii_question_get(formset, q, out) != AXL_OK) {
            continue;
        }
        if (axl_strcmp(out->prompt, prompt) == 0) {
            return true;
        }
    }
    return false;
}

static void
test_known_formset(void)
{
    size_t fs_count = axl_hii_formset_count();
    size_t match = (size_t)-1;
    size_t qcount = 0;
    AxlGuid guid;
    axl_memset(&guid, 0, sizeof guid);

    for (size_t f = 0; f < fs_count; f++) {
        char title[128];
        size_t qc = 0;
        if (axl_hii_formset_get(f, title, sizeof title, NULL, 0, &guid, &qc) != AXL_OK) {
            continue;
        }
        if (axl_strcmp(title, "RAM Disk Configuration") == 0) {
            match = f;
            qcount = qc;
            break;
        }
    }

    test_check(match != (size_t)-1,
               "hii: found 'RAM Disk Configuration' form set");
    if (match == (size_t)-1) {
        return;
    }

    /* RamDiskDxe's stable form-set GUID (gRamDiskConfigFormSetGuid),
       identical on x64 OVMF and aa64 AAVMF. */
    AxlGuid expected = AXL_GUID(0x2a46715f, 0x3581, 0x4a55,
                                0x8e, 0x73, 0x2b, 0x76,
                                0x9a, 0xaa, 0x30, 0xc5);
    test_check(axl_guid_cmp(&guid, &expected),
               "hii: 'RAM Disk Configuration' form-set GUID matches");

    AxlHiiQuestion one_of;
    bool got_one_of = find_question(match, qcount, "Disk Memory Type:", &one_of);
    test_check(got_one_of, "hii: found 'Disk Memory Type:' ONE_OF question");
    test_check(got_one_of && one_of.type == AXL_HII_ONE_OF,
               "hii: 'Disk Memory Type:' is ONE_OF");
    test_check(got_one_of && one_of.width == 1,
               "hii: 'Disk Memory Type:' width is 1");
    test_check(got_one_of && one_of.u.one_of.option_count == 2,
               "hii: 'Disk Memory Type:' has 2 options");

    AxlHiiQuestion numeric;
    bool got_numeric = find_question(match, qcount, "Size (Hex):", &numeric);
    test_check(got_numeric, "hii: found 'Size (Hex):' NUMERIC question");
    test_check(got_numeric && numeric.type == AXL_HII_NUMERIC,
               "hii: 'Size (Hex):' is NUMERIC");
    test_check(got_numeric && numeric.width == 8,
               "hii: 'Size (Hex):' width is 8");
}

// ---------------------------------------------------------------------------
// Value I/O.
//
// Negatives are deterministic on both arches: a STRING question is never
// readable through the u64 API, and a read-only question is never writable.
// (OVMF/AAVMF both publish the STRING "File Name" and the read-only RAM-disk
// "Disk Memory Type:".)
//
// The positive read + write round-trip is pinned to one specific stable
// writable question by exact prompt: OVMF PlatformDxe's "Change Preferred
// Resolution for Next Boot" ONE_OF (EFI-variable-backed, not read-only). It
// exists on x64 OVMF and not on aa64 AAVMF, so the round-trip is firmware-
// gated and SKIP-balanced: the found and not-found branches run the same
// number of assertions to keep the cross-arch ratchet count equal. Pinning by
// prompt (rather than "first writable ONE_OF") keeps it deterministic — a
// transient variable-store state can't make some other question stand in.
// ---------------------------------------------------------------------------

#define HII_WRITABLE_PROMPT "Change Preferred Resolution for Next Boot"

/* Find a question matching @p prompt anywhere in the model. */
static bool
find_question_anywhere(const char *prompt, size_t *out_fs, size_t *out_q,
                       AxlHiiQuestion *out)
{
    size_t fs_count = axl_hii_formset_count();
    for (size_t f = 0; f < fs_count; f++) {
        size_t qcount = 0;
        if (axl_hii_formset_get(f, NULL, 0, NULL, 0, NULL, &qcount) != AXL_OK) {
            continue;
        }
        for (size_t q = 0; q < qcount; q++) {
            if (axl_hii_question_get(f, q, out) != AXL_OK) {
                continue;
            }
            if (axl_strcmp(out->prompt, prompt) == 0) {
                *out_fs = f;
                *out_q = q;
                return true;
            }
        }
    }
    return false;
}

static void
test_value_negatives(void)
{
    size_t f, q;
    AxlHiiQuestion x;

    bool got_string = find_question_anywhere("File Name", &f, &q, &x);
    test_check(got_string && x.type == AXL_HII_STRING,
               "hii value: 'File Name' is a STRING question");
    uint64_t v = 123;
    test_check(got_string && axl_hii_question_read(f, q, &v) == AXL_ERR,
               "hii value: STRING question read -> AXL_ERR");

    bool got_ro = find_question_anywhere("Disk Memory Type:", &f, &q, &x);
    test_check(got_ro && x.read_only,
               "hii value: 'Disk Memory Type:' is read-only");
    test_check(got_ro && axl_hii_question_write(f, q, 0) == AXL_ERR,
               "hii value: read-only question write -> AXL_ERR");
}

static void
test_value_roundtrip(void)
{
    size_t f, q;
    AxlHiiQuestion x;

    /* The round-trip targets one specific writable question. Its absence
       (aa64) is a legitimate SKIP; its presence (x64) means the read MUST
       succeed — so a regressed read path fails here rather than silently
       skipping. */
    if (!find_question_anywhere(HII_WRITABLE_PROMPT, &f, &q, &x) ||
        x.type != AXL_HII_ONE_OF || x.read_only) {
        axl_printf("SKIP: writable question '%s' not present\n",
                   HII_WRITABLE_PROMPT);
        test_check(true, "hii value: round-trip SKIP balance");
        test_check(true, "hii value: round-trip SKIP balance");
        test_check(true, "hii value: round-trip SKIP balance");
        test_check(true, "hii value: round-trip SKIP balance");
        test_check(true, "hii value: round-trip SKIP balance");
        return;
    }

    uint64_t orig = 0;
    test_check(axl_hii_question_read(f, q, &orig) == AXL_OK,
               "hii value: read original value -> AXL_OK");

    /* Pick a benign distinct target: another of the question's options. */
    uint64_t target = orig;
    for (size_t i = 0; i < x.u.one_of.option_count; i++) {
        if (x.u.one_of.options[i].value != orig) {
            target = x.u.one_of.options[i].value;
            break;
        }
    }

    test_check(axl_hii_question_write(f, q, target) == AXL_OK,
               "hii value: write target -> AXL_OK");

    uint64_t readback = orig;
    test_check(axl_hii_question_read(f, q, &readback) == AXL_OK
                   && readback == target,
               "hii value: read back equals written target");

    /* Restore the original so the test leaves no trace. */
    test_check(axl_hii_question_write(f, q, orig) == AXL_OK,
               "hii value: restore original -> AXL_OK");

    uint64_t restored = target;
    test_check(axl_hii_question_read(f, q, &restored) == AXL_OK
                   && restored == orig,
               "hii value: read back equals restored original");
}

// ---------------------------------------------------------------------------
// Malformed-IFR bounds (synthetic byte streams via the internal parse seam).
//
// Live OVMF/AAVMF emit only well-formed IFR, so the parser's bounds guards
// against a hostile/corrupt firmware image can't be exercised against real HII.
// These build raw IFR opcode streams by hand and drive the real parser
// (_axl_hii_parse_form_package) directly. The discriminating case is a VARSTORE
// whose opcode Length is shorter than the fixed struct: without the guard, the
// trailing-name length underflows to a huge value and the parser both reads out
// of bounds and adds a garbage varstore. With the guard it is skipped, so
// varstore_count distinguishes fixed from broken. The parse return value is
// ignored — with no HII String protocol the form-set title resolves empty (so
// the "titled form set" gate returns false), but the varstore/question model is
// still populated during the walk.
// ---------------------------------------------------------------------------

/* Emit a `length`-byte IFR opcode (2-byte header + zero payload) at @p pos. */
static size_t
ifr_emit(uint8_t *buf, size_t pos, uint8_t opcode, uint8_t length, bool scope)
{
    buf[pos++] = opcode;
    buf[pos++] = (uint8_t)(length | (scope ? 0x80 : 0x00));
    for (uint8_t i = 2; i < length; i++) {
        buf[pos++] = 0;
    }
    return pos;
}

static void
test_malformed_ifr_bounds(void)
{
    uint8_t ifr[256];
    HiiFormSet fs;
    size_t pos;

    /* FORM_SET | valid VARSTORE | malformed-short VARSTORE | CHECKBOX | END.
       sizeof(EFI_IFR_VARSTORE) == 22; the malformed one declares Length 10. */
    pos = 0;
    pos = ifr_emit(ifr, pos, EFI_IFR_FORM_SET_OP,  23, true);
    pos = ifr_emit(ifr, pos, EFI_IFR_VARSTORE_OP,  22, false);  /* valid (no name) */
    pos = ifr_emit(ifr, pos, EFI_IFR_VARSTORE_OP,  10, false);  /* malformed: 10 < 22 */
    pos = ifr_emit(ifr, pos, EFI_IFR_CHECKBOX_OP,  14, false);  /* valid question */
    pos = ifr_emit(ifr, pos, EFI_IFR_END_OP,        2, false);
    _axl_hii_parse_form_package(NULL, ifr, pos, &fs);
    test_check(fs.varstore_count == 1,
               "hii ifr: malformed short VARSTORE rejected (count 1, not 2)");
    test_check(fs.question_count == 1,
               "hii ifr: opcode walk resumes past a malformed VARSTORE");
    axl_free(fs.questions);

    /* VARSTORE_EFI variant: sizeof(EFI_IFR_VARSTORE_EFI) == 26. */
    pos = 0;
    pos = ifr_emit(ifr, pos, EFI_IFR_FORM_SET_OP,     23, true);
    pos = ifr_emit(ifr, pos, EFI_IFR_VARSTORE_EFI_OP, 12, false);  /* malformed: 12 < 26 */
    pos = ifr_emit(ifr, pos, EFI_IFR_VARSTORE_EFI_OP, 26, false);  /* valid (no name) */
    _axl_hii_parse_form_package(NULL, ifr, pos, &fs);
    test_check(fs.varstore_count == 1,
               "hii ifr: malformed short VARSTORE_EFI rejected");
    axl_free(fs.questions);

    /* A top-level opcode with Length < the 2-byte header halts the walk
       cleanly (no advance-by-zero infinite loop, no OOB): the VARSTORE before
       it survives, the one after is never reached. */
    pos = 0;
    pos = ifr_emit(ifr, pos, EFI_IFR_FORM_SET_OP, 23, true);
    pos = ifr_emit(ifr, pos, EFI_IFR_VARSTORE_OP, 22, false);  /* valid */
    ifr[pos++] = EFI_IFR_VARSTORE_OP;   /* bogus: declares Length 1 (< 2) */
    ifr[pos++] = 1;
    pos = ifr_emit(ifr, pos, EFI_IFR_VARSTORE_OP, 22, false);  /* unreachable */
    _axl_hii_parse_form_package(NULL, ifr, pos, &fs);
    test_check(fs.varstore_count == 1,
               "hii ifr: opcode with Length < header size halts the walk");
    axl_free(fs.questions);
}

// ---------------------------------------------------------------------------
// STRING value I/O.
//
// read_string IS exercisable in QEMU: OVMF/AAVMF both back "Preferred
// Resolution at Next Boot" (non-empty text) and "Cert GUID" (empty but
// variable-backed) with readable varstores. write_string's positive path is
// not — every STRING question both firmwares publish is read-only — so it is
// real-hardware territory; here we pin the read-only rejection. All targets
// exist on both arches, so the assertion count is naturally balanced.
// ---------------------------------------------------------------------------

static void
test_string_io(void)
{
    size_t f, q;
    AxlHiiQuestion x;
    char buf[64];

    /* Positive read: a backed STRING with real text. The exact value is
       arch/firmware state ("1280x800" vs "Unset"), so assert it reads and
       is a non-empty UTF-8 string within max_size — proving the CHAR16 ->
       UTF-8 extraction actually ran. */
    bool got_res = find_question_anywhere("Preferred Resolution at Next Boot",
                                          &f, &q, &x);
    test_check(got_res && x.type == AXL_HII_STRING,
               "hii str: 'Preferred Resolution' is a STRING question");
    buf[0] = 'Z';
    int rc_res = got_res
        ? axl_hii_question_read_string(f, q, buf, sizeof buf) : AXL_ERR;
    test_check(rc_res == AXL_OK, "hii str: read_string of a backed STRING -> AXL_OK");
    size_t len = axl_strlen(buf);
    test_check(rc_res == AXL_OK && len > 0 && len <= x.u.string.max_size,
               "hii str: read_string returns non-empty text within max_size");

    /* A variable-backed but empty STRING still reads OK (empty result). */
    bool got_cert = find_question_anywhere("Cert GUID", &f, &q, &x);
    test_check(got_cert
                   && axl_hii_question_read_string(f, q, buf, sizeof buf) == AXL_OK,
               "hii str: read_string of an empty backed STRING -> AXL_OK");

    /* read_string rejects a non-STRING question (cross-arch RAM-disk ONE_OF). */
    bool got_oneof = find_question_anywhere("Disk Memory Type:", &f, &q, &x);
    test_check(got_oneof
                   && axl_hii_question_read_string(f, q, buf, sizeof buf) == AXL_ERR,
               "hii str: read_string of a non-STRING question -> AXL_ERR");

    /* Argument validation. */
    test_check(axl_hii_question_read_string(SIZE_MAX, 0, buf, sizeof buf) == AXL_ERR,
               "hii str: read_string out-of-range -> AXL_ERR");
    test_check(got_cert && axl_hii_question_read_string(f, q, NULL, 0) == AXL_ERR,
               "hii str: read_string NULL buf -> AXL_ERR");

    /* write_string rejects a read-only STRING (every OVMF/AAVMF STRING is
       read-only; the writable path is real-hardware-only). */
    test_check(got_cert && find_question_anywhere("Cert GUID", &f, &q, &x)
                   && x.read_only
                   && axl_hii_question_write_string(f, q, "x") == AXL_ERR,
               "hii str: write_string of a read-only STRING -> AXL_ERR");

    /* write_string rejects a non-STRING question and bad arguments. */
    test_check(got_oneof && find_question_anywhere("Disk Memory Type:", &f, &q, &x)
                   && axl_hii_question_write_string(f, q, "x") == AXL_ERR,
               "hii str: write_string of a non-STRING question -> AXL_ERR");
    test_check(axl_hii_question_write_string(SIZE_MAX, 0, "x") == AXL_ERR,
               "hii str: write_string out-of-range -> AXL_ERR");
    test_check(got_cert && axl_hii_question_write_string(f, q, NULL) == AXL_ERR,
               "hii str: write_string NULL value -> AXL_ERR");
}

static int
test_hii_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlHii");

    test_arg_validation();
    test_enumeration();
    test_known_formset();
    test_value_negatives();
    test_value_roundtrip();
    test_malformed_ifr_bounds();
    test_string_io();

    return test_print_results();
}

AXL_APP(test_hii_main)
