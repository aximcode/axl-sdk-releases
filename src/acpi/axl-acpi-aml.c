/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-acpi-aml.c
    Non-evaluating AML namespace walker.

    Finds `Device` declarations in a DSDT or SSDT and reports the
    integer objects each carries. **It never evaluates.** A `Method`
    body is skipped by its length and never entered; anything whose
    value is not a literal in the byte stream is reported as
    present-but-unreadable. There is no interpreter loop here, and a
    reviewer can confirm that by its absence.

    The parser's real job is SKIPPING. It recognises a small set of
    opcodes and steps over everything else by trusting PkgLength --
    `OperationRegion`, `Field`, `Buffer`, `Package`, method bodies and
    vendor terms all get walked past rather than failed on. Firmware is
    untrusted input, so every length is validated against what remains
    before it is used.
**/

#include <axl/axl-acpi.h>
#include <axl/axl-macros.h>
#include <axl/axl-str.h>

/* Opcodes we recognise. Everything else is skipped. */
#define OP_ZERO          0x00
#define OP_ONE           0x01
#define OP_ALIAS         0x06
#define OP_NAME          0x08
#define OP_BYTE_PREFIX   0x0A
#define OP_WORD_PREFIX   0x0B
#define OP_DWORD_PREFIX  0x0C
#define OP_STRING_PREFIX 0x0D
#define OP_QWORD_PREFIX  0x0E
#define OP_SCOPE         0x10
#define OP_BUFFER        0x11
#define OP_PACKAGE       0x12
#define OP_VAR_PACKAGE   0x13
#define OP_METHOD        0x14
#define OP_EXT_PREFIX    0x5B
#define OP_EXT_DEVICE    0x82
#define OP_EXT_REGION    0x80
#define OP_EXT_FIELD     0x81
#define OP_EXT_PROCESSOR 0x83
#define OP_EXT_POWER_RES 0x84
#define OP_EXT_THERMAL   0x85
#define OP_EXT_INDEXFLD  0x86
#define OP_EXT_BANKFLD   0x87
#define OP_EXT_MUTEX     0x01
#define OP_EXT_EVENT     0x02
#define OP_EXT_DATA_RGN  0x88
#define OP_EXT_CREATEFLD 0x13
#define OP_EXTERNAL      0x15
#define OP_STORE         0x70
#define OP_IF            0xA0
#define OP_ELSE          0xA1
#define OP_ONES          0xFF

/* NameString structural bytes. */
#define NAME_ROOT        0x5C
#define NAME_PARENT      0x5E
#define NAME_DUAL        0x2E
#define NAME_MULTI       0x2F
#define NAME_NULL        0x00

#define ACPI_HEADER_LEN  36

/**
 * @brief Decode a PkgLength at @p pos, validating it against @p limit.
 *
 * The encoding: the first byte's bits 7:6 give how many extra bytes
 * follow (0..3). With no extra bytes, bits 5:0 are the length; with
 * extra bytes, bits 3:0 are the low nibble and each extra byte
 * contributes 8 higher bits.
 *
 * The length INCLUDES the PkgLength bytes themselves.
 *
 * @return true when the package fits inside @p limit, with @p out_end
 *     set to the offset one past the package and @p out_hdr to the
 *     number of PkgLength bytes consumed.
 */
static bool
pkg_length(
    const uint8_t  *aml,
    size_t          pos,
    size_t          limit,
    size_t         *out_end,
    size_t         *out_hdr
    )
{
    if (pos >= limit) {
        return false;
    }

    uint8_t  lead  = aml[pos];
    unsigned extra = (unsigned)(lead >> 6);
    size_t   len;

    if (extra == 0) {
        len = (size_t)(lead & 0x3F);
    } else {
        if (pos + extra >= limit) {
            return false;
        }
        len = (size_t)(lead & 0x0F);
        for (unsigned i = 0; i < extra; i++) {
            len |= (size_t)aml[pos + 1 + i] << (4 + 8 * i);
        }
    }

    size_t hdr = 1 + extra;
    /* A package must at least contain its own length bytes, and must
       not claim more than the table has left. Both checks matter:
       firmware in the wild gets this wrong, and a runaway here is a
       read past the end of the table. */
    if (len < hdr || len > limit - pos) {
        return false;
    }

    *out_end = pos + len;
    *out_hdr = hdr;
    return true;
}

/**
 * @brief Step over a NameString, capturing its final NameSeg.
 *
 * @p out_seg receives the LAST segment (5 bytes incl. NUL), which is
 * what names the object being declared. Leading root/parent prefixes
 * and earlier segments only affect where it lives, and the walker
 * tracks that through its scope stack instead.
 *
 * @return true on a well-formed NameString, with @p out_pos advanced.
 */
static bool
name_string(
    const uint8_t  *aml,
    size_t          pos,
    size_t          limit,
    char            out_seg[5],
    size_t         *out_pos
    )
{
    unsigned count = 1;

    while (pos < limit && (aml[pos] == NAME_ROOT || aml[pos] == NAME_PARENT)) {
        pos++;
    }
    if (pos >= limit) {
        return false;
    }

    if (aml[pos] == NAME_DUAL) {
        count = 2;
        pos++;
    } else if (aml[pos] == NAME_MULTI) {
        pos++;
        if (pos >= limit) {
            return false;
        }
        count = aml[pos];
        pos++;
        if (count == 0) {
            return false;
        }
    } else if (aml[pos] == NAME_NULL) {
        /* NullName. Zero the WHOLE segment, not just the first byte:
           callers compare all four characters (method_argc does), and
           leaving three bytes uninitialised means comparing garbage.
           clang-analyzer caught this. */
        for (unsigned i = 0; i < 5; i++) {
            out_seg[i] = '\0';
        }
        *out_pos = pos + 1;
        return true;
    }

    if (count > (limit - pos) / 4) {
        return false;
    }

    size_t last = pos + (size_t)(count - 1) * 4;
    for (unsigned i = 0; i < 4; i++) {
        out_seg[i] = (char)aml[last + i];
    }
    out_seg[4] = '\0';

    *out_pos = pos + (size_t)count * 4;
    return true;
}

/**
 * @brief Read a `Name`'s integer value, if it is a literal.
 *
 * Only the integer data opcodes produce a value. A Buffer, Package or
 * String is a legitimate `Name` payload but not an integer, so it is
 * reported as present-with-no-integer via @p out_is_int false.
 *
 * @return true when the payload was recognised and stepped over.
 */
static bool
name_value(
    const uint8_t  *aml,
    size_t          pos,
    size_t          limit,
    uint64_t       *out_val,
    bool           *out_is_int,
    size_t         *out_pos
    )
{
    if (pos >= limit) {
        return false;
    }

    uint8_t op = aml[pos];
    unsigned width;

    *out_is_int = true;
    switch (op) {
    case OP_ZERO:
        *out_val = 0;
        *out_pos = pos + 1;
        return true;
    case OP_ONE:
        *out_val = 1;
        *out_pos = pos + 1;
        return true;
    case OP_ONES:
        *out_val = ~(uint64_t)0;
        *out_pos = pos + 1;
        return true;
    case OP_BYTE_PREFIX:  width = 1; break;
    case OP_WORD_PREFIX:  width = 2; break;
    case OP_DWORD_PREFIX: width = 4; break;
    case OP_QWORD_PREFIX: width = 8; break;

    case OP_BUFFER:
    case OP_PACKAGE:
    case OP_VAR_PACKAGE: {
        /* Not an integer, but well-formed and skippable. */
        size_t end, hdr;
        if (!pkg_length(aml, pos + 1, limit, &end, &hdr)) {
            return false;
        }
        *out_is_int = false;
        *out_pos = end;
        return true;
    }

    case OP_STRING_PREFIX: {
        size_t p = pos + 1;
        while (p < limit && aml[p] != '\0') {
            p++;
        }
        if (p >= limit) {
            return false;
        }
        *out_is_int = false;
        *out_pos = p + 1;
        return true;
    }

    default:
        /* Some other term -- a reference, an expression. Present, but
           not a literal, and we do not evaluate. */
        *out_is_int = false;
        *out_pos = pos;
        return false;
    }

    if (limit - pos < (size_t)width + 1) {
        return false;
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < width; i++) {
        v |= (uint64_t)aml[pos + 1 + i] << (8 * i);
    }
    *out_val = v;
    *out_pos = pos + 1 + width;
    return true;
}

/**
 * @brief Declared argument count for a method NameSeg, or 0.
 *
 * Matching is by NameSeg rather than full namespace path. ACPICA
 * resolves properly scoped names; that needs a namespace tree this
 * walker deliberately does not build. Two methods sharing a leaf name
 * with different arities would collide, and the first wins -- a real
 * limitation, recorded here rather than hidden, and one that costs at
 * most the contents of one package when it bites.
 */
static unsigned
method_argc(const AxlAmlWalk *w, const char seg[5])
{
    if (w == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < w->_mcount; i++) {
        if (w->_mseg[i][0] == seg[0] && w->_mseg[i][1] == seg[1]
            && w->_mseg[i][2] == seg[2] && w->_mseg[i][3] == seg[3]) {
            return w->_margc[i];
        }
    }
    return 0;
}

/**
 * @brief Step over one TermArg without evaluating it.
 *
 * Needed only for `If`/`Else` predicates: the walker must get PAST the
 * predicate to reach the body, and both measured DSDTs open with
 * `If (Zero) { External(...) ... }`, so failing here loses the entire
 * table.
 *
 * Handles literals, names, buffers/packages, locals/args, and the
 * logical and arithmetic operators that firmware predicates are
 * actually built from. Anything else returns false, and the caller
 * skips that whole package rather than failing the walk.
 *
 * @return true with @p out_pos advanced past the argument.
 */
static bool
skip_term_arg(
    const AxlAmlWalk *w,
    const uint8_t  *aml,
    size_t          pos,
    size_t          limit,
    unsigned        depth,
    size_t         *out_pos
    )
{
    if (pos >= limit || depth > 8) {
        return false;
    }

    uint8_t op = aml[pos];

    /* Locals, Args and the one-byte constants. */
    if ((op >= 0x60 && op <= 0x6E) || op == OP_ZERO || op == OP_ONE
        || op == OP_ONES) {
        *out_pos = pos + 1;
        return true;
    }

    switch (op) {
    case OP_BYTE_PREFIX:
    case OP_WORD_PREFIX:
    case OP_DWORD_PREFIX:
    case OP_QWORD_PREFIX:
    case OP_STRING_PREFIX:
    case OP_BUFFER:
    case OP_PACKAGE:
    case OP_VAR_PACKAGE: {
        uint64_t v;
        bool     is_int;
        return name_value(aml, pos, limit, &v, &is_int, out_pos);
    }
    default:
        break;
    }

    /* Unary, per the spec's productions: LNot(0x92) Operand,
       DerefOf(0x83) ObjReference, SizeOf(0x87) SuperName,
       Increment(0x75) / Decrement(0x76) SuperName.
       Not(0x80) is NOT here -- DefNot := NotOp Operand Target takes a
       target too, and treating it as unary under-consumes by one term
       and desyncs the rest of the body. */
    if (op == 0x92 || op == 0x83 || op == 0x87 || op == 0x75
        || op == 0x76) {
        return skip_term_arg(w, aml, pos + 1, limit, depth + 1, out_pos);
    }

    /* Conversions: ToBuffer, ToDecimalString, ToHexString, ToInteger.
       Each takes an operand and a Target. */
    if (op == 0x96 || op == 0x97 || op == 0x98 || op == 0x99) {
        size_t p;
        if (!skip_term_arg(w, aml, pos + 1, limit, depth + 1, &p)) {
            return false;
        }
        return skip_term_arg(w, aml, p, limit, depth + 1, out_pos);
    }

    /* Binary logical: LEqual, LGreater, LLess, LAnd, LOr. No target. */
    if (op == 0x93 || op == 0x94 || op == 0x95 || op == 0x90 || op == 0x91) {
        size_t p;
        if (!skip_term_arg(w, aml, pos + 1, limit, depth + 1, &p)) {
            return false;
        }
        return skip_term_arg(w, aml, p, limit, depth + 1, out_pos);
    }

    /* Binary with a Target operand: Add, Subtract, Multiply, And, Or,
       Xor, ShiftLeft, ShiftRight, Index. */
    if (op == 0x72 || op == 0x74 || op == 0x77 || op == 0x7B || op == 0x7D
        || op == 0x7F || op == 0x79 || op == 0x7A || op == 0x88
        || op == 0x73 || op == 0x9C || op == 0x80) {
        size_t p;
        if (!skip_term_arg(w, aml, pos + 1, limit, depth + 1, &p)) {
            return false;
        }
        if (!skip_term_arg(w, aml, p, limit, depth + 1, &p)) {
            return false;
        }
        return skip_term_arg(w, aml, p, limit, depth + 1, out_pos);
    }

    /* Extended-opcode expressions. CondRefOf dominates in practice:
       22 of the client DSDT's If predicates are `If (CondRefOf(X))`,
       and failing them cost every device inside those bodies. */
    if (op == OP_EXT_PREFIX && pos + 1 < limit) {
        uint8_t ext = aml[pos + 1];
        /* CondRefOf(Source, Result) and Acquire(Mutex, Timeout): two
           operands each. */
        if (ext == 0x12 || ext == 0x23) {
            size_t p;
            if (!skip_term_arg(w, aml, pos + 2, limit, depth + 1, &p)) {
                return false;
            }
            return skip_term_arg(w, aml, p, limit, depth + 1, out_pos);
        }
        return false;
    }

    /* A NameString here is either a plain reference or a method
       invocation. The grammar cannot tell them apart --
       `MethodInvocation := NameString TermArgList` carries no count --
       so the arity table built by axl_aml_walk_begin decides. An
       unknown name is treated as a plain reference, which is the
       right default: most names are. */
    if (op == NAME_ROOT || op == NAME_PARENT || op == NAME_DUAL
        || op == NAME_MULTI || op == '_' || (op >= 'A' && op <= 'Z')) {
        char   seg[5];
        size_t p;
        if (!name_string(aml, pos, limit, seg, &p)) {
            return false;
        }
        unsigned argc = method_argc(w, seg);
        for (unsigned i = 0; i < argc; i++) {
            if (!skip_term_arg(w, aml, p, limit, depth + 1, &p)) {
                return false;
            }
        }
        *out_pos = p;
        return true;
    }

    return false;
}


/**
 * @brief Step over one named object that cannot contain a `Device`.
 *
 * The parser's real job (spec §5b). Shared by the top-level walk and
 * the per-device body scan so the two cannot drift apart.
 *
 * **Not every named object carries a PkgLength**, and getting that
 * wrong is not a small error: `OperationRegion` has none, so treating
 * it as if it did reads a NameString byte as a length and
 * desynchronises every subsequent term. That single bug cost both
 * measured DSDTs their entire device list.
 *
 * @return true when @p out_pos was advanced past a recognised object.
 */
static bool
skip_object(
    const AxlAmlWalk *w,
    const uint8_t  *aml,
    size_t          pos,
    size_t          limit,
    size_t         *out_pos
    )
{
    char   seg[5];
    size_t p;

    if (pos >= limit) {
        return false;
    }

    uint8_t op = aml[pos];

    switch (op) {
    case OP_NAME: {
        if (!name_string(aml, pos + 1, limit, seg, &p)) {
            return false;
        }
        uint64_t v;
        bool     is_int;
        return name_value(aml, p, limit, &v, &is_int, out_pos);
    }

    case OP_ALIAS:
        if (!name_string(aml, pos + 1, limit, seg, &p)) {
            return false;
        }
        return name_string(aml, p, limit, seg, out_pos);

    case OP_EXTERNAL:
        /* NameString ObjectType(1) ArgumentCount(1) */
        if (!name_string(aml, pos + 1, limit, seg, &p) || p + 2 > limit) {
            return false;
        }
        *out_pos = p + 2;
        return true;

    /* CreateBitField / Byte / Word / DWord / QWord:
       TermArg TermArg NameString (CreateBitField takes the index as
       its second TermArg; the shape is the same). */
    case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F:
        if (!skip_term_arg(w, aml, pos + 1, limit, 0, &p)
            || !skip_term_arg(w, aml, p, limit, 0, &p)) {
            return false;
        }
        return name_string(aml, p, limit, seg, out_pos);

    /* Statements. An If body at namespace level legitimately holds
       executable terms, and firmware uses that: the client DSDT has 14
       `Store(...)` statements sitting between namespace declarations.
       We do not execute them -- we step over both operands. */
    case OP_STORE:
        if (!skip_term_arg(w, aml, pos + 1, limit, 0, &p)) {
            return false;
        }
        return skip_term_arg(w, aml, p, limit, 0, out_pos);

    case OP_METHOD:
    case OP_BUFFER:
    case OP_PACKAGE:
    case OP_VAR_PACKAGE: {
        size_t end, hdr;
        if (!pkg_length(aml, pos + 1, limit, &end, &hdr)) {
            return false;
        }
        *out_pos = end;
        return true;
    }

    case OP_EXT_PREFIX:
        break;   /* handled below */

    default:
        /* A method invocation can appear as a STATEMENT, not just as
           an argument -- firmware debug helpers like `ADBG("...")` sit
           between namespace declarations. skip_term_arg owns the
           NameString-plus-arity logic, so defer to it rather than
           duplicating the table lookup. */
        if (op == NAME_ROOT || op == NAME_PARENT || op == NAME_DUAL
            || op == NAME_MULTI || op == '_' || (op >= 'A' && op <= 'Z')) {
            return skip_term_arg(w, aml, pos, limit, 0, out_pos);
        }
        return false;
    }

    if (pos + 1 >= limit) {
        return false;
    }
    uint8_t ext = aml[pos + 1];

    switch (ext) {
    /* These carry a PkgLength. Processor / PowerResource / ThermalZone
       have TermLists that could in principle hold a Device; none of
       the measured firmware does that, and stepping over them keeps
       the walk simple. A device declared inside one is missed, which
       is why the walk reports itself incomplete rather than clean. */
    case OP_EXT_FIELD:
    case OP_EXT_INDEXFLD:
    case OP_EXT_BANKFLD:
    case OP_EXT_PROCESSOR:
    case OP_EXT_POWER_RES:
    case OP_EXT_THERMAL: {
        size_t end, hdr;
        if (!pkg_length(aml, pos + 2, limit, &end, &hdr)) {
            return false;
        }
        *out_pos = end;
        return true;
    }

    /* NO PkgLength. NameString RegionSpace(1) TermArg TermArg. */
    case OP_EXT_REGION:
        if (!name_string(aml, pos + 2, limit, seg, &p) || p + 1 > limit) {
            return false;
        }
        p += 1;   /* RegionSpace */
        if (!skip_term_arg(w, aml, p, limit, 0, &p)) {
            return false;
        }
        return skip_term_arg(w, aml, p, limit, 0, out_pos);

    /* NO PkgLength. NameString SyncFlags(1). */
    case OP_EXT_MUTEX:
        if (!name_string(aml, pos + 2, limit, seg, &p) || p + 1 > limit) {
            return false;
        }
        *out_pos = p + 1;
        return true;

    /* NO PkgLength. NameString. */
    case OP_EXT_EVENT:
        return name_string(aml, pos + 2, limit, seg, out_pos);

    /* NO PkgLength. NameString TermArg x3. */
    case OP_EXT_DATA_RGN:
        if (!name_string(aml, pos + 2, limit, seg, &p)) {
            return false;
        }
        for (unsigned i = 0; i < 3; i++) {
            if (!skip_term_arg(w, aml, p, limit, 0, &p)) {
                return false;
            }
        }
        *out_pos = p;
        return true;

    /* NO PkgLength. TermArg x3 NameString. */
    case OP_EXT_CREATEFLD:
        p = pos + 2;
        for (unsigned i = 0; i < 3; i++) {
            if (!skip_term_arg(w, aml, p, limit, 0, &p)) {
                return false;
            }
        }
        return name_string(aml, p, limit, seg, out_pos);

    default:
        return false;
    }
}

/* Which node field a NameSeg maps to, or none. */
typedef enum {
    FIELD_NONE = 0,
    FIELD_ADR,
    FIELD_SUN,
    FIELD_UID,
    FIELD_SEG,
    FIELD_BBN,
    FIELD_PLD
} InterestingField;

static InterestingField
classify(const char seg[5])
{
    if (axl_strcmp(seg, "_ADR") == 0) { return FIELD_ADR; }
    if (axl_strcmp(seg, "_SUN") == 0) { return FIELD_SUN; }
    if (axl_strcmp(seg, "_UID") == 0) { return FIELD_UID; }
    if (axl_strcmp(seg, "_SEG") == 0) { return FIELD_SEG; }
    if (axl_strcmp(seg, "_BBN") == 0) { return FIELD_BBN; }
    if (axl_strcmp(seg, "_PLD") == 0) { return FIELD_PLD; }
    return FIELD_NONE;
}

static void
node_set(
    AxlAmlNode       *n,
    InterestingField  f,
    AxlAmlValueKind   kind,
    uint64_t          value
    )
{
    AxlAmlValue *v = NULL;

    switch (f) {
    case FIELD_ADR: v = &n->adr; break;
    case FIELD_SUN: v = &n->sun; break;
    case FIELD_UID: v = &n->uid; break;
    case FIELD_SEG: v = &n->seg; break;
    case FIELD_BBN: v = &n->bbn; break;
    case FIELD_PLD: n->pld_kind = kind; return;
    case FIELD_NONE:
    default:        return;
    }

    v->kind  = kind;
    v->value = (kind == AXL_AML_VALUE_STATIC) ? value : 0;
}


/**
 * @brief Record one method's name and arity, first declaration wins.
 */
static void
method_record(AxlAmlWalk *w, const char seg[5], uint8_t argc)
{
    if (w->_mcount >= AXL_AML_METHOD_MAX) {
        return;
    }
    for (unsigned i = 0; i < w->_mcount; i++) {
        if (w->_mseg[i][0] == seg[0] && w->_mseg[i][1] == seg[1]
            && w->_mseg[i][2] == seg[2] && w->_mseg[i][3] == seg[3]) {
            return;
        }
    }
    for (unsigned k = 0; k < 5; k++) {
        w->_mseg[w->_mcount][k] = seg[k];
    }
    w->_margc[w->_mcount] = argc;
    w->_mcount++;
}

/**
 * @brief Pre-pass: record every Method declaration's name and arity.
 *
 * Descends into the containers a Method can be declared in and steps
 * over everything else. Recursion is bounded by @p depth.
 */
static void
collect_methods(
    AxlAmlWalk     *w,
    const uint8_t  *aml,
    size_t          pos,
    size_t          end,
    unsigned        depth
    )
{
    if (depth > AXL_AML_DEPTH_MAX) {
        return;
    }

    while (pos < end) {
        uint8_t op = aml[pos];
        size_t  pend, hdr, np;
        char    seg[5];

        if (op == OP_METHOD) {
            if (!pkg_length(aml, pos + 1, end, &pend, &hdr)) {
                return;
            }
            if (name_string(aml, pos + 1 + hdr, pend, seg, &np)
                && np < pend) {
                /* MethodFlags bits 0-2 are the argument count. */
                method_record(w, seg, (uint8_t)(aml[np] & 0x07));
                collect_methods(w, aml, np + 1, pend, depth + 1);
            }
            pos = pend;
            continue;
        }

        if (op == OP_SCOPE) {
            if (!pkg_length(aml, pos + 1, end, &pend, &hdr)
                || !name_string(aml, pos + 1 + hdr, pend, seg, &np)) {
                return;
            }
            collect_methods(w, aml, np, pend, depth + 1);
            pos = pend;
            continue;
        }

        if (op == OP_IF || op == OP_ELSE) {
            if (!pkg_length(aml, pos + 1, end, &pend, &hdr)) {
                return;
            }
            size_t body = pos + 1 + hdr;
            if (op == OP_IF && !skip_term_arg(w, aml, body, pend, 0, &body)) {
                pos = pend;
                continue;
            }
            collect_methods(w, aml, body, pend, depth + 1);
            pos = pend;
            continue;
        }

        if (op == OP_EXT_PREFIX && pos + 1 < end) {
            uint8_t ext = aml[pos + 1];
            if (ext == OP_EXT_DEVICE || ext == OP_EXT_PROCESSOR
                || ext == OP_EXT_POWER_RES || ext == OP_EXT_THERMAL) {
                if (!pkg_length(aml, pos + 2, end, &pend, &hdr)
                    || !name_string(aml, pos + 2 + hdr, pend, seg, &np)) {
                    return;
                }
                collect_methods(w, aml, np, pend, depth + 1);
                pos = pend;
                continue;
            }
        }

        if (!skip_object(w, aml, pos, end, &np)) {
            return;
        }
        pos = np;
    }
}

int
axl_aml_walk_begin(
    AxlAmlWalk           *walk,
    const AxlAcpiHeader  *table
    )
{
    if (walk == NULL || table == NULL) {
        return AXL_ERR;
    }
    if (table->length <= ACPI_HEADER_LEN) {
        return AXL_ERR;
    }

    walk->_aml       = (const uint8_t *)table + ACPI_HEADER_LEN;
    walk->_len       = (size_t)table->length - ACPI_HEADER_LEN;
    walk->_pos       = 0;
    walk->_truncated = false;
    walk->_skipped   = false;
    walk->_depth     = 0;
    walk->_mcount    = 0;

    /* Two rounds to a fixpoint. The first round cannot parse packages
       whose contents include calls to methods it has not seen yet, so
       it skips them; the second round has those arities and reaches
       further, finding methods the first round missed. Running it
       twice is what multi-pass namespace loaders do, and it is cheap:
       a structural walk with no allocation. */
    for (unsigned round = 0; round < 2; round++) {
        collect_methods(walk, walk->_aml, 0, walk->_len, 0);
    }
    return AXL_OK;
}

bool
axl_aml_walk_truncated(
    const AxlAmlWalk  *walk
    )
{
    return (walk != NULL) && (walk->_truncated || walk->_skipped);
}

/* Collect the objects declared directly inside a Device body, without
   descending into anything. Nested Devices are reached by the main
   walk, not from here. */
static void
scan_device_body(
    const AxlAmlWalk *w,
    const uint8_t  *aml,
    size_t          pos,
    size_t          end,
    AxlAmlNode     *out
    )
{
    while (pos < end) {
        uint8_t op = aml[pos];

        if (op == OP_NAME) {
            char   seg[5];
            size_t np;
            if (!name_string(aml, pos + 1, end, seg, &np)) {
                return;
            }
            InterestingField f = classify(seg);
            uint64_t v = 0;
            bool     is_int = false;
            size_t   vp;
            if (!name_value(aml, np, end, &v, &is_int, &vp)) {
                /* Payload we cannot read as a literal. Still present. */
                if (f != FIELD_NONE) {
                    node_set(out, f, AXL_AML_VALUE_METHOD, 0);
                }
                return;
            }
            if (f != FIELD_NONE) {
                /* A _PLD Name is a Buffer, so "static and present" is
                   the most that can be said about it. */
                /* A _PLD Name is a Buffer by definition, so "present
                   and static" is the most that can be said of it.
                   Anything else non-integer -- a string _UID, say --
                   is present and readable but has no number. */
                node_set(out, f,
                         is_int          ? AXL_AML_VALUE_STATIC
                         : f == FIELD_PLD ? AXL_AML_VALUE_STATIC
                                          : AXL_AML_VALUE_NON_INTEGER,
                         v);
            }
            pos = vp;
            continue;
        }

        if (op == OP_METHOD) {
            size_t mend, hdr;
            if (!pkg_length(aml, pos + 1, end, &mend, &hdr)) {
                return;
            }
            char   seg[5];
            size_t np;
            if (name_string(aml, pos + 1 + hdr, mend, seg, &np)) {
                InterestingField f = classify(seg);
                if (f != FIELD_NONE) {
                    /* Declared, but its value is computed at runtime.
                       The body is NOT entered. */
                    node_set(out, f, AXL_AML_VALUE_METHOD, 0);
                }
            }
            pos = mend;
            continue;
        }

        /* Anything else: step over it. Nested Devices are found by
           the main walk, which descends into device bodies, so this
           scan only has to not get lost. */
        size_t np2;
        if (op == OP_SCOPE || op == OP_IF || op == OP_ELSE) {
            size_t send, shdr;
            if (!pkg_length(aml, pos + 1, end, &send, &shdr)) {
                return;
            }
            pos = send;
            continue;
        }
        if (op == OP_EXT_PREFIX && pos + 1 < end
            && aml[pos + 1] == OP_EXT_DEVICE) {
            size_t dend, dhdr;
            if (!pkg_length(aml, pos + 2, end, &dend, &dhdr)) {
                return;
            }
            pos = dend;
            continue;
        }
        if (!skip_object(w, aml, pos, end, &np2)) {
            return;
        }
        pos = np2;
    }
}

bool
axl_aml_walk_next(
    AxlAmlWalk   *walk,
    AxlAmlNode   *out
    )
{
    if (walk == NULL || out == NULL) {
        return false;
    }

    const uint8_t *aml = walk->_aml;

    for (;;) {
        /* Leave any scopes whose package has ended. */
        while (walk->_depth > 0
               && walk->_pos >= walk->_end[walk->_depth - 1]) {
            walk->_depth--;
        }

        size_t limit = (walk->_depth > 0) ? walk->_end[walk->_depth - 1]
                                          : walk->_len;
        if (walk->_pos >= limit) {
            if (walk->_depth > 0) {
                continue;
            }
            return false;
        }

        uint8_t op = aml[walk->_pos];

        /* Scope / If / Else: descend, so devices inside are found. */
        if (op == OP_SCOPE || op == OP_IF || op == OP_ELSE) {
            size_t end, hdr;
            if (!pkg_length(aml, walk->_pos + 1, limit, &end, &hdr)) {
                walk->_truncated = true;
                return false;
            }
            size_t body = walk->_pos + 1 + hdr;

            /* Zero the WHOLE segment: only Scope fills it from a
               NameString, and If/Else would otherwise copy four
               uninitialised bytes into the path stack. */
            char seg[5];
            for (unsigned i = 0; i < 5; i++) {
                seg[i] = '\0';
            }
            if (op == OP_SCOPE) {
                size_t np;
                if (!name_string(aml, body, end, seg, &np)) {
                    walk->_truncated = true;
                    return false;
                }
                body = np;
            }
            /* An If carries a predicate before its body. We do not
               evaluate it, but we must step PAST it -- both measured
               DSDTs open with `If (Zero) { External(...) }`, so a
               walker that lands on the predicate dies on byte 4 of
               445 KB. Else has no predicate. */
            if (op == OP_IF) {
                size_t pp;
                if (!skip_term_arg(walk, aml, body, end, 0, &pp)) {
                    /* Predicate we cannot step over: skip this whole
                       package rather than abandoning the table. */
                    walk->_pos = end;
                    walk->_skipped = true;
                    continue;
                }
                body = pp;
            }

            if (walk->_depth >= AXL_AML_DEPTH_MAX) {
                walk->_truncated = true;
                return false;
            }
            unsigned d = walk->_depth;
            walk->_end[d]  = end;
            walk->_cond[d] = (op != OP_SCOPE)
                             || (d > 0 && walk->_cond[d - 1]);
            for (unsigned i = 0; i < 5; i++) {
                walk->_seg[d][i] = seg[i];
            }
            walk->_depth = d + 1;
            walk->_pos   = body;
            continue;
        }

        /* Device: yield it, then continue after its package. */
        if (op == OP_EXT_PREFIX && walk->_pos + 1 < limit
            && aml[walk->_pos + 1] == OP_EXT_DEVICE) {
            size_t end, hdr;
            if (!pkg_length(aml, walk->_pos + 2, limit, &end, &hdr)) {
                walk->_truncated = true;
                return false;
            }
            char   seg[5];
            size_t np;
            if (!name_string(aml, walk->_pos + 2 + hdr, end, seg, &np)) {
                walk->_truncated = true;
                return false;
            }

            /* Reset then fill: every object defaults to ABSENT, which
               is what makes "not declared" distinguishable from "zero". */
            for (size_t i = 0; i < sizeof(*out); i++) {
                ((uint8_t *)out)[i] = 0;
            }
            out->adr.kind = AXL_AML_VALUE_ABSENT;
            out->sun.kind = AXL_AML_VALUE_ABSENT;
            out->uid.kind = AXL_AML_VALUE_ABSENT;
            out->seg.kind = AXL_AML_VALUE_ABSENT;
            out->bbn.kind = AXL_AML_VALUE_ABSENT;
            out->pld_kind = AXL_AML_VALUE_ABSENT;

            /* Path: the enclosing scope segments, then this device. */
            size_t p = 0;
            out->path[p++] = '\\';
            bool over = false;
            for (unsigned i = 0; i < walk->_depth; i++) {
                if (walk->_seg[i][0] == '\0') {
                    continue;
                }
                if (p + 5 >= AXL_AML_PATH_MAX) {
                    over = true;
                    break;
                }
                if (p > 1) {
                    out->path[p++] = '.';
                }
                for (unsigned k = 0; k < 4 && walk->_seg[i][k]; k++) {
                    out->path[p++] = walk->_seg[i][k];
                }
            }
            if (!over && p + 5 < AXL_AML_PATH_MAX) {
                if (p > 1) {
                    out->path[p++] = '.';
                }
                for (unsigned k = 0; k < 4 && seg[k]; k++) {
                    out->path[p++] = seg[k];
                }
            } else {
                over = true;
            }
            out->path[p] = '\0';
            out->path_truncated = over;

            out->conditional = (walk->_depth > 0)
                               && walk->_cond[walk->_depth - 1];

            scan_device_body(walk, aml, np, end, out);

            /* Descend rather than jumping to `end`: devices nest, and
               jumping past the package loses every child. The server's
               SSDT5 declares 49 devices of which only 33 are top
               level. scan_device_body above has already collected THIS
               device's own objects; the main loop re-walks the same
               bytes to find children, which is a little redundant and
               much simpler than a second recursive collector. */
            /* Stop, do not yield. Returning true here without
               advancing _pos re-yields this same device forever -- the
               depth cap, which exists to bound a hostile table, was
               itself the unbounded path. */
            if (walk->_depth >= AXL_AML_DEPTH_MAX) {
                walk->_truncated = true;
                return false;
            }
            {
                unsigned d = walk->_depth;
                walk->_end[d]  = end;
                walk->_cond[d] = out->conditional;
                for (unsigned i = 0; i < 5; i++) {
                    walk->_seg[d][i] = seg[i];
                }
                walk->_depth = d + 1;
                walk->_pos   = np;
            }
            return true;
        }

        /* Everything else at this level: one shared skipper, so the
           body scan and the main walk cannot disagree about how wide
           an object is. */
        size_t nxt;
        if (skip_object(walk, aml, walk->_pos, limit, &nxt)) {
            walk->_pos = nxt;
            continue;
        }

        /* An opcode outside the recognised set. Guessing its width
           would desynchronise the walk, but abandoning the whole table
           is worse -- the spec's requirement is skip-don't-fail. We
           know the extent of the package we are inside, so give up on
           just that package and carry on with its siblings. Only at
           the very top level, where there is nothing to give up on, is
           this fatal. */
        if (walk->_depth > 0) {
            walk->_pos = walk->_end[walk->_depth - 1];
            walk->_skipped = true;
            continue;
        }
        walk->_truncated = true;
        return false;
    }
}
