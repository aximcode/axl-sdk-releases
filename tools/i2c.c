/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file i2c.c
    Low-level I2C / SMBus explorer — UEFI port of the Linux
    `i2c-tools` surface (`i2cdetect`, `i2cget`, `i2cset`,
    `i2cdump`) layered on top of `AxlSmbus`.

    The motivating gap: `memspd scan` was a one-off solution to
    "which controller(s) actually carry slaves we care about."
    Any future tool that needs a non-SPD SMBus device — FRU
    EEPROMs at 0xA0..0xAE, fan controllers, voltage monitors,
    temperature sensors at 0x4C/0x4D, USB-C TCPCs, PCIe retimers
    — needs the same enumerate / probe / get / set machinery.
    Centralizing that here means future investigations are
    minutes instead of hours.

    Verb summary:
      list             — `i2cdetect -l` — every controller, indexed
      probe <bus>      — `i2cdetect -y -r` — slave-ack grid
      get <bus> <slv> <reg> [<count>]
                       — single-byte read; with count, hex dump
      set <bus> <slv> <reg> <byte> [<byte>...]
                       — write one or more bytes (refuses without
                         --force; writes can brick devices)
      dump <bus> <slv> — full 256-byte hex dump

    Bus indexing matches `axl_smbus_visit_all` order: SMBus HC
    handles first, then I2C Master handles, then PIIX4 ports.
**/

#include <axl.h>

AXL_LOG_DOMAIN("i2c");

// ---------------------------------------------------------------------------
// Per-bus operation types
// ---------------------------------------------------------------------------

typedef enum {
    OP_PROBE,
    OP_GET,
    OP_SET,
    OP_DUMP,
} OpKind;

typedef enum {
    PROBE_QUICK,    /* SMBus QUICK — just address + R/W ACK. Linux i2cdetect default. */
    PROBE_READ,     /* SMBus byte-data read at register 0. Linux i2cdetect -r. */
    PROBE_AUTO,     /* QUICK with safe-byte-read for 0x30-0x37 + 0x50-0x5F.
                       Matches Linux's exact default (which switches mode
                       per-address to avoid accidentally erasing EEPROMs). */
} ProbeMode;

typedef struct {
    OpKind   op;
    size_t   target_bus;
    bool     target_found;
    int      rc;

    /* OP_PROBE */
    ProbeMode probe_mode;
    uint8_t   probe_first;   /* 0x03 default */
    uint8_t   probe_last;    /* 0x77 default */

    /* OP_GET / OP_SET / OP_DUMP */
    uint8_t  slave;
    uint8_t  reg;            /* OP_GET, OP_SET — start register */
    size_t   read_count;     /* OP_GET — bytes to read (1 = byte read) */

    /* OP_SET */
    const uint8_t *write_data;
    size_t         write_len;
} BusOp;

// ---------------------------------------------------------------------------
// list
// ---------------------------------------------------------------------------

/* Format mirrors Linux's `i2cdetect -l`:
 *     i2c-N\tkind\tname\ttype
 * where:
 *   kind is "smbus" / "i2c" — short bus-type label
 *   name is the per-instance description from axl_smbus_describe()
 *         (e.g., "AMD FCH PIIX4 AUX port 1 at 0xB20")
 *   type is "SMBus adapter" / "I2C adapter".
 * Tabs separate columns so it pipes cleanly through `column -t`. */
static void
list_visit(
    AxlSmbus  *s,
    size_t     index,
    void      *user
    )
{
    (void)user;
    AxlSmbusTransport tk = axl_smbus_transport(s);
    const char *kind, *type;
    switch (tk) {
        case AXL_SMBUS_TRANSPORT_HC:
            kind = "smbus";  type = "SMBus adapter"; break;
        case AXL_SMBUS_TRANSPORT_I2C:
            kind = "i2c  ";  type = "I2C adapter";   break;
        case AXL_SMBUS_TRANSPORT_PIIX4:
            kind = "smbus";  type = "SMBus adapter"; break;
        default:
            kind = "?    ";  type = "?";             break;
    }
    axl_printf("i2c-%-2zu\t%s     \t%-36s\t%s\n",
               index, kind, axl_smbus_describe(s), type);
}

static int
do_list(
    AxlArgs  *a
    )
{
    (void)a;
    size_t n = axl_smbus_visit_all(list_visit, NULL);
    if (n == 0) {
        axl_printf("(no SMBus / I2C controllers published)\n");
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Per-bus operation framework
// ---------------------------------------------------------------------------

/* Decide which SMBus operation to use for a given probe address —
 * matches Linux i2cdetect's exact mode-per-address logic.
 *
 * - PROBE_READ:  byte-data read at register 0 (Linux's `-r` flag).
 *                Triggers device-specific behavior — generally less
 *                safe than receive byte but reveals more.
 * - PROBE_QUICK: SMBus QUICK write everywhere — pure address-ACK
 *                probe. Linux's `-q` flag.
 * - PROBE_AUTO (default): QUICK write everywhere EXCEPT
 *                0x30..0x37 and 0x50..0x5F where it uses Receive
 *                Byte (no-command 1-byte read). The EEPROM-prone
 *                ranges get the safer probe to avoid accidentally
 *                triggering writes — matches Linux i2cdetect's
 *                default mode exactly. */
static int
probe_one(AxlSmbus *s, ProbeMode mode, uint8_t addr)
{
    if (mode == PROBE_READ) {
        uint8_t byte0;
        return axl_smbus_read_byte(s, addr, 0x00, &byte0);
    }
    bool eeprom_safe_zone =
        (addr >= 0x30 && addr <= 0x37) ||
        (addr >= 0x50 && addr <= 0x5F);
    if (mode == PROBE_AUTO && eeprom_safe_zone) {
        uint8_t b;
        return axl_smbus_receive_byte(s, addr, &b);
    }
    return axl_smbus_quick(s, addr, /*is_read=*/false);
}

/* Format mirrors Linux's `i2cdetect -y -r` 16-column hex grid; the
 * trailing "N slave(s) acknowledged" summary is richer than Linux
 * gives — kept because it makes empty buses obviously distinguishable
 * from missing-output bugs at a glance. */
static void
op_probe(AxlSmbus *s, BusOp *op)
{
    axl_printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    int responders = 0;
    for (uint8_t row = 0; row < 8; row++) {
        axl_printf("%02x:", (unsigned)(row << 4));
        for (uint8_t col = 0; col < 16; col++) {
            uint8_t addr = (uint8_t)((row << 4) | col);
            if (addr < op->probe_first || addr > op->probe_last) {
                axl_printf("   ");
                continue;
            }
            if (probe_one(s, op->probe_mode, addr) == AXL_OK) {
                axl_printf(" %02x", (unsigned)addr);
                responders++;
            } else {
                axl_printf(" --");
            }
        }
        axl_printf("\n");
    }
    axl_printf("\n%d slave(s) acknowledged\n", responders);
}

static void
op_get(AxlSmbus *s, BusOp *op)
{
    if (op->read_count <= 1) {
        uint8_t value;
        if (axl_smbus_read_byte(s, op->slave, op->reg, &value) != AXL_OK) {
            axl_printf("read failed (slave 0x%02X reg 0x%02X)\n",
                       op->slave, op->reg);
            return;
        }
        axl_printf("0x%02X\n", (unsigned)value);
        op->rc = 0;
        return;
    }

    /* count > 1 — auto-incrementing byte reads. Block-read is a
     * different SMBus protocol that some devices don't speak;
     * byte-stream reads are what i2cdump uses and what 24Cxx /
     * SPD EEPROMs / FRU flash all support. */
    if (op->read_count > 256) {
        op->read_count = 256;
    }
    uint8_t buf[256];
    size_t got = 0;
    for (size_t i = 0; i < op->read_count; i++) {
        if (axl_smbus_read_byte(s, op->slave, (uint8_t)(op->reg + i),
                                &buf[i]) != AXL_OK)
        {
            break;
        }
        got++;
    }
    if (got == 0) {
        axl_printf("read failed (slave 0x%02X reg 0x%02X)\n",
                   op->slave, op->reg);
        return;
    }
    axl_hexdump(NULL, buf, got, 16, 1);
    if (got < op->read_count) {
        axl_printf("(only %zu of %zu bytes read — slave NACKed mid-stream)\n",
                   got, op->read_count);
    }
    op->rc = 0;
}

static void
op_set(AxlSmbus *s, BusOp *op)
{
    int rc;
    if (op->write_len == 1) {
        rc = axl_smbus_write_byte(s, op->slave, op->reg, op->write_data[0]);
    } else {
        rc = axl_smbus_write_block(s, op->slave, op->reg,
                                   op->write_data, op->write_len);
    }
    if (rc != AXL_OK) {
        axl_printf("write failed (slave 0x%02X reg 0x%02X, %zu bytes)\n",
                   op->slave, op->reg, op->write_len);
        return;
    }
    axl_printf("OK — wrote %zu byte(s) to slave 0x%02X reg 0x%02X\n",
               op->write_len, op->slave, op->reg);
    op->rc = 0;
}

static void
op_dump(AxlSmbus *s, BusOp *op)
{
    /* Full 256-byte image, auto-incrementing register addresses.
     * Stops on first NACK and prints what we got — matches
     * i2cdump's behavior on devices smaller than 256 bytes. */
    uint8_t buf[256];
    size_t got = 0;
    for (size_t r = 0; r < 256; r++) {
        if (axl_smbus_read_byte(s, op->slave, (uint8_t)r,
                                &buf[r]) != AXL_OK)
        {
            break;
        }
        got++;
    }
    if (got == 0) {
        axl_printf("dump failed: slave 0x%02X did not ack\n", op->slave);
        return;
    }
    axl_hexdump(NULL, buf, got, 16, 1);
    if (got < 256) {
        axl_printf("(slave returned %zu bytes before NACKing)\n", got);
    }
    op->rc = 0;
}

static void
bus_op_visit(
    AxlSmbus  *s,
    size_t     index,
    void      *user
    )
{
    BusOp *op = (BusOp *)user;
    if (index != op->target_bus) {
        return;
    }
    op->target_found = true;
    switch (op->op) {
        case OP_PROBE: op_probe(s, op); op->rc = 0; break;
        case OP_GET:   op_get(s, op);   break;
        case OP_SET:   op_set(s, op);   break;
        case OP_DUMP:  op_dump(s, op);  break;
    }
}

static int
run_bus_op(BusOp *op)
{
    op->target_found = false;
    op->rc           = 1;
    axl_smbus_visit_all(bus_op_visit, op);
    if (!op->target_found) {
        axl_printf("bus %zu not found (use `i2c list` for available controllers)\n",
                   op->target_bus);
        return 2;
    }
    return op->rc;
}

// ---------------------------------------------------------------------------
// Verbs
// ---------------------------------------------------------------------------

static int
do_probe(
    AxlArgs  *a
    )
{
    bool quick = axl_args_get_bool(a, "quick");
    bool read  = axl_args_get_bool(a, "read");
    bool all   = axl_args_get_bool(a, "all");

    if (quick && read) {
        axl_printf("--quick and --read are mutually exclusive\n");
        return 2;
    }

    BusOp op = {
        .op          = OP_PROBE,
        .target_bus  = (size_t)axl_args_get_uint(a, "bus"),
        .probe_mode  = read  ? PROBE_READ
                     : quick ? PROBE_QUICK
                             : PROBE_AUTO,
        /* --all extends the range to the full SMBus 7-bit address
         * space (0x00..0x7F). Default omits the reserved 0x00..0x02
         * + 0x78..0x7F ranges per the spec. */
        .probe_first = all ? 0x00 : 0x03,
        .probe_last  = all ? 0x7F : 0x77,
    };

    /* Optional positional [first] [last] override the defaults.
     * Matches Linux i2cdetect: `i2cdetect <bus> <first>` probes
     * <first>..0x77, not just the single address — only when both
     * [first] and [last] are given does the range narrow. */
    int n = axl_args_get_pos_count(a);
    if (n >= 1) {
        const char *s = axl_args_get_pos(a, 0);
        uint8_t v;
        if (s == NULL || axl_str_to_u8(s, 0, &v, NULL) != AXL_OK ||
            v > 0x7F)
        {
            axl_printf("invalid first address: %s (must be 0..0x7F)\n",
                       s != NULL ? s : "(null)");
            return 2;
        }
        op.probe_first = v;
    }
    if (n >= 2) {
        const char *s = axl_args_get_pos(a, 1);
        uint8_t v;
        if (s == NULL || axl_str_to_u8(s, 0, &v, NULL) != AXL_OK ||
            v > 0x7F)
        {
            axl_printf("invalid last address: %s (must be 0..0x7F)\n",
                       s != NULL ? s : "(null)");
            return 2;
        }
        op.probe_last = v;
    }
    if (n > 2) {
        axl_printf("usage: i2c probe <bus> [first] [last]\n");
        return 2;
    }
    if (op.probe_first > op.probe_last) {
        axl_printf("first (0x%02x) > last (0x%02x)\n",
                   op.probe_first, op.probe_last);
        return 2;
    }

    return run_bus_op(&op);
}

static int
do_get(
    AxlArgs  *a
    )
{
    BusOp op = {
        .op         = OP_GET,
        .target_bus = (size_t)axl_args_get_uint(a, "bus"),
        .slave      = (uint8_t)axl_args_get_uint(a, "slave"),
        .reg        = (uint8_t)axl_args_get_uint(a, "reg"),
        .read_count = (size_t)axl_args_get_uint(a, "count"),
    };
    if (op.read_count == 0) {
        op.read_count = 1;
    }
    return run_bus_op(&op);
}

static int
do_set(
    AxlArgs  *a
    )
{
    bool force = axl_args_get_bool(a, "force");
    if (!force) {
        axl_printf("Refusing to write without --force.\n");
        axl_printf("Writes can brick devices — confirm intent with --force.\n");
        return 2;
    }

    int n = axl_args_get_pos_count(a);
    if (n < 1) {
        axl_printf("at least one byte value required after <reg>\n");
        return 2;
    }
    if (n > AXL_SMBUS_BLOCK_MAX) {
        axl_printf("too many bytes (%d); max is %u\n",
                   n, (unsigned)AXL_SMBUS_BLOCK_MAX);
        return 2;
    }
    /* Parse positional bytes into a stack buffer. Strict base=0
     * accepts "0x", "0", and decimal — matches i2cset's default. */
    uint8_t  bytes[AXL_SMBUS_BLOCK_MAX];
    for (int i = 0; i < n; i++) {
        const char *s = axl_args_get_pos(a, i);
        if (s == NULL) {
            return 2;
        }
        if (axl_str_to_u8(s, 0, &bytes[i], NULL) != AXL_OK) {
            axl_printf("invalid byte value: %s (must be 0..255)\n", s);
            return 2;
        }
    }

    BusOp op = {
        .op         = OP_SET,
        .target_bus = (size_t)axl_args_get_uint(a, "bus"),
        .slave      = (uint8_t)axl_args_get_uint(a, "slave"),
        .reg        = (uint8_t)axl_args_get_uint(a, "reg"),
        .write_data = bytes,
        .write_len  = (size_t)n,
    };
    return run_bus_op(&op);
}

static int
do_dump(
    AxlArgs  *a
    )
{
    BusOp op = {
        .op         = OP_DUMP,
        .target_bus = (size_t)axl_args_get_uint(a, "bus"),
        .slave      = (uint8_t)axl_args_get_uint(a, "slave"),
    };
    return run_bus_op(&op);
}

// ---------------------------------------------------------------------------
// AxlArgs declaration
// ---------------------------------------------------------------------------

static const AxlArgDesc probe_args[] = {
    { .name = "bus", .type = AXL_ARG_U32, .base = 0, .required = true,
      .help = "Bus index" },
    { .name = "range", .type = AXL_ARG_MULTI,
      .help = "[first] [last] — optional address range "
              "(default 0x03..0x77, or 0x00..0x7F with --all)" },
    {0}
};

static const AxlArgDesc probe_flags[] = {
    { .name = "quick", .short_name = 'q', .type = AXL_ARG_BOOL,
      .help = "Use SMBus QUICK write at every address (Linux i2cdetect default)" },
    { .name = "read",  .short_name = 'r', .type = AXL_ARG_BOOL,
      .help = "Use SMBus byte-data read at register 0 "
              "(Linux i2cdetect -r)" },
    { .name = "all",   .short_name = 'a', .type = AXL_ARG_BOOL,
      .help = "Probe full address range 0x00..0x7F including reserved" },
    {0}
};

static const AxlArgDesc bus_slave_args[] = {
    { .name = "bus",   .type = AXL_ARG_U32, .base = 0, .required = true,
      .help = "Bus index" },
    { .name = "slave", .type = AXL_ARG_U8,  .base = 0, .required = true,
      .min = 0x03, .max = 0x77,
      .help = "7-bit slave address (0x03..0x77)" },
    {0}
};

static const AxlArgDesc get_args[] = {
    { .name = "bus",   .type = AXL_ARG_U32, .base = 0, .required = true,
      .help = "Bus index" },
    { .name = "slave", .type = AXL_ARG_U8,  .base = 0, .required = true,
      .min = 0x03, .max = 0x77,
      .help = "7-bit slave address" },
    { .name = "reg",   .type = AXL_ARG_U8,  .base = 0, .required = true,
      .help = "Register / command byte" },
    { .name = "count", .type = AXL_ARG_U32, .base = 0,
      .default_value = "1",
      .help = "Number of bytes to read (auto-incrementing); default 1" },
    {0}
};

static const AxlArgDesc set_args[] = {
    { .name = "bus",   .type = AXL_ARG_U32, .base = 0, .required = true,
      .help = "Bus index" },
    { .name = "slave", .type = AXL_ARG_U8,  .base = 0, .required = true,
      .min = 0x03, .max = 0x77,
      .help = "7-bit slave address" },
    { .name = "reg",   .type = AXL_ARG_U8,  .base = 0, .required = true,
      .help = "Register / command byte" },
    { .name = "bytes", .type = AXL_ARG_MULTI,
      .help = "One or more byte values (decimal, 0x.. hex, or 0.. octal)" },
    {0}
};

static const AxlArgDesc set_flags[] = {
    { .name = "force", .short_name = 'f', .type = AXL_ARG_BOOL,
      .help = "Confirm intent — writes can brick devices" },
    {0}
};

static const AxlArgsNode verbs[] = {
    { .name = "list",  .handler = do_list,
      .help = "Enumerate every published SMBus / I2C controller" },
    { .name = "probe", .handler = do_probe, .positionals = probe_args,
      .flags = probe_flags,
      .help = "Walk slave addresses; print which respond. "
              "Default: SMBus QUICK with safe-byte-read for "
              "0x30-0x37 + 0x50-0x5F (matches Linux i2cdetect)" },
    { .name = "get",   .handler = do_get,   .positionals = get_args,
      .help = "Read 1+ bytes from <slave>:<reg> on <bus>" },
    { .name = "set",   .handler = do_set,   .positionals = set_args,
      .flags = set_flags,
      .help = "Write 1+ bytes to <slave>:<reg> (--force required)" },
    { .name = "dump",  .handler = do_dump,  .positionals = bus_slave_args,
      .help = "Full 256-byte hex dump of <slave> on <bus>" },
    {0}
};

AXL_TOOL_MAIN(i2c)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name = "i2c",
        .help = "Low-level I2C / SMBus explorer "
                "(list / probe / get / set / dump)",
        .verbs = verbs,
    });
}
