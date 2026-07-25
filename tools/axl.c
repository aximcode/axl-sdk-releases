/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl.c
    Busybox-style dispatcher — single binary that hosts all axl-sdk
    tools, selected via subcommand.

    Build with `make axl-busybox`. The build defines AXL_BUSYBOX,
    which causes each tool's AXL_TOOL_MAIN(name) macro to expand to
    `int axl_tool_<name>_main(int argc, char **argv)` instead of
    the standalone `int main(...)`. All tool .o files link into a
    single axl.efi alongside this dispatcher.

    Run as:
        axl.efi cat [args...]
        axl.efi grep [args...]
        axl.efi --help        (lists every tool with one-line help)
        axl.efi <tool> --help (forwards --help to the named tool)

    AXL_BUSYBOX is purely opt-in. The default `make tools` build
    leaves each tool as its own .efi; this binary is an alternative
    deployment shape for users who'd rather copy one file to a USB
    stick than 18.

    The dispatcher's argv shift hides itself from the dispatched
    tool: when the user runs `axl.efi cat foo`, the cat tool sees
    argv = ["cat", "foo"], argc = 2 — so each tool's existing
    AxlArgs verb tree (which uses argv[0] as the program name in
    --help output) renders identically to the standalone build.
**/

#include <axl.h>

/* Each tool's AXL_TOOL_MAIN(name) expands to this function in
   AXL_BUSYBOX builds. Declare them all so we can build the
   dispatch table below without per-tool headers. The names must
   match the .c filenames (and the AXL_TOOL_MAIN argument). */
extern int axl_tool_cat_main(int argc, char **argv);
extern int axl_tool_cut_main(int argc, char **argv);
extern int axl_tool_dmidecode_main(int argc, char **argv);
extern int axl_tool_fetch_main(int argc, char **argv);
extern int axl_tool_find_main(int argc, char **argv);
extern int axl_tool_grep_main(int argc, char **argv);
extern int axl_tool_hexdump_main(int argc, char **argv);
extern int axl_tool_i2c_main(int argc, char **argv);
extern int axl_tool_ipmi_main(int argc, char **argv);
extern int axl_tool_lspci_main(int argc, char **argv);
extern int axl_tool_lsproto_main(int argc, char **argv);
extern int axl_tool_lsusb_main(int argc, char **argv);
extern int axl_tool_memspd_main(int argc, char **argv);
extern int axl_tool_mkfixture_main(int argc, char **argv);
extern int axl_tool_mkrd_main(int argc, char **argv);
extern int axl_tool_netinfo_main(int argc, char **argv);
extern int axl_tool_rfbrowse_main(int argc, char **argv);
extern int axl_tool_rndisfix_main(int argc, char **argv);
extern int axl_tool_sysinfo_main(int argc, char **argv);
extern int axl_tool_timetest_main(int argc, char **argv);
extern int axl_tool_tr_main(int argc, char **argv);

typedef int (*ToolFn)(int argc, char **argv);

typedef struct {
    const char *name;
    ToolFn      fn;
    const char *help;
} Tool;

/* Sorted alphabetically — order is what `axl --help` prints. */
static const Tool tools[] = {
    { "cat",       axl_tool_cat_main,       "Concatenate and print files"        },
    { "cut",       axl_tool_cut_main,       "Remove sections from each line"     },
    { "dmidecode", axl_tool_dmidecode_main, "Decode SMBIOS / DMI tables"         },
    { "fetch",     axl_tool_fetch_main,     "HTTP client (curl-style)"           },
    { "find",      axl_tool_find_main,      "Recursive file finder"              },
    { "grep",      axl_tool_grep_main,      "Pattern search in files"            },
    { "hexdump",   axl_tool_hexdump_main,   "Hex/ASCII file viewer"              },
    { "i2c",       axl_tool_i2c_main,       "Low-level I2C / SMBus explorer"     },
    { "ipmi",      axl_tool_ipmi_main,      "IPMI / BMC command tool"            },
    { "lspci",     axl_tool_lspci_main,     "List PCI devices + bridges"         },
    { "lsproto",   axl_tool_lsproto_main,   "List UEFI protocols by spec name"   },
    { "lsusb",     axl_tool_lsusb_main,     "List USB devices and topology"      },
    { "memspd",    axl_tool_memspd_main,    "Read DDR4 / DDR5 SPD via SMBus"     },
    { "mkfixture", axl_tool_mkfixture_main, "Build a test fixture image"        },
    { "mkrd",      axl_tool_mkrd_main,      "Create / list / remove RAM disks"   },
    { "netinfo",   axl_tool_netinfo_main,   "Network diagnostics + ping"         },
    { "rfbrowse",  axl_tool_rfbrowse_main,  "Redfish browser"                    },
    { "rndisfix",  axl_tool_rndisfix_main,  "RNDIS NIC quirk handler"            },
    { "sysinfo",   axl_tool_sysinfo_main,   "Firmware / SMBIOS / memory inventory"},
    { "timetest",  axl_tool_timetest_main,  "Time / monotonic clock probes"      },
    { "tr",        axl_tool_tr_main,        "Translate/squeeze/delete bytes"     },
};

#define TOOL_COUNT (sizeof(tools) / sizeof(tools[0]))

static void
print_usage(void)
{
    axl_printf("Usage: axl <tool> [args...]\n\n");
    axl_printf("Available tools:\n");
    for (size_t i = 0; i < TOOL_COUNT; i++) {
        axl_printf("  %-10s  %s\n", tools[i].name, tools[i].help);
    }
    axl_printf("\nRun `axl <tool> --help` for tool-specific options.\n");
}

int
main(int argc, char **argv)
{
    if (argc < 2 ||
        axl_streql(argv[1], "--help") ||
        axl_streql(argv[1], "-h"))
    {
        print_usage();
        return (argc < 2) ? 1 : 0;
    }

    /* The multiplexer's own version. Scoped to argv[1] so `axl mkrd --version`
       still dispatches to mkrd (whose own wrapper answers it) rather than being
       intercepted here. */
    if (axl_streql(argv[1], "--version") || axl_streql(argv[1], "-V")) {
        axl_printf("axl %s\n", axl_version());
        return 0;
    }

    for (size_t i = 0; i < TOOL_COUNT; i++) {
        if (axl_streql(argv[1], tools[i].name)) {
            /* Shift argv so the dispatched tool sees argv[0] = its
               own name. argc/argv past argv[1] become the tool's
               argc/argv directly — its AxlArgs verb tree parses as
               if the tool had been invoked as a standalone .efi. */
            return tools[i].fn(argc - 1, argv + 1);
        }
    }

    axl_printf("axl: unknown tool '%s'\n\n", argv[1]);
    print_usage();
    return 1;
}
