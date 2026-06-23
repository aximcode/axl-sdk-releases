/** @file test-uefi-types.c
    Compile-time verification of AXL native UEFI type headers.
    Not shipped -- used to validate Phase N1 is complete.

    Build: gcc -c -ffreestanding -fshort-wchar -Wall -Wextra \
               -Iinclude -DAXL_BACKEND_NATIVE test/test-uefi-types.c
**/

#include <uefi/axl-uefi.h>

// ===================================================================
// Type size assertions
// ===================================================================

_Static_assert(sizeof(UINTN) == 8, "UINTN must be 8 bytes");
_Static_assert(sizeof(INTN) == 8, "INTN must be 8 bytes");
_Static_assert(sizeof(EFI_STATUS) == 8, "EFI_STATUS must be 8 bytes");
_Static_assert(sizeof(EFI_HANDLE) == 8, "EFI_HANDLE must be pointer-sized");
_Static_assert(sizeof(EFI_EVENT) == 8, "EFI_EVENT must be pointer-sized");
_Static_assert(sizeof(EFI_GUID) == 16, "EFI_GUID must be 16 bytes");
_Static_assert(sizeof(BOOLEAN) == 1, "BOOLEAN must be 1 byte");
_Static_assert(sizeof(CHAR16) == 2, "CHAR16 must be 2 bytes");
_Static_assert(sizeof(EFI_IPv4_ADDRESS) == 4, "IPv4 must be 4 bytes");
_Static_assert(sizeof(EFI_INPUT_KEY) == 4, "EFI_INPUT_KEY must be 4 bytes");

// ===================================================================
// Status code smoke tests
// ===================================================================

_Static_assert(EFI_SUCCESS == 0, "EFI_SUCCESS must be 0");
_Static_assert(EFI_ERROR(EFI_NOT_FOUND), "EFI_NOT_FOUND must be an error");
_Static_assert(!EFI_ERROR(EFI_SUCCESS), "EFI_SUCCESS must not be an error");

// ===================================================================
// System table field access
// ===================================================================

static void
test_system_table(
    EFI_SYSTEM_TABLE  *st
    )
{
    // Console output
    st->ConOut->OutputString(st->ConOut, (CHAR16 *)0);
    st->ConOut->SetAttribute(st->ConOut, 0);
    (void)st->ConOut->Mode->Attribute;

    // Console input
    EFI_INPUT_KEY key;
    st->ConIn->ReadKeyStroke(st->ConIn, &key);
    (void)st->ConIn->WaitForKey;

    // Configuration table walk (SMBIOS)
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        (void)st->ConfigurationTable[i].VendorGuid;
        (void)st->ConfigurationTable[i].VendorTable;
    }
}

// ===================================================================
// Boot Services function pointer access
// ===================================================================

static void
test_boot_services(
    EFI_BOOT_SERVICES  *bs
    )
{
    void *buf;
    bs->AllocatePool(EfiBootServicesData, 256, &buf);
    bs->FreePool(buf);

    EFI_EVENT evt;
    bs->CreateEvent(EVT_TIMER, TPL_APPLICATION, NULL, NULL, &evt);
    bs->SetTimer(evt, TimerPeriodic, 10000000ULL);
    bs->SignalEvent(evt);
    bs->CheckEvent(evt);
    bs->CloseEvent(evt);

    UINTN index;
    bs->WaitForEvent(1, &evt, &index);

    void *reg;
    EFI_GUID guid = {0};
    bs->RegisterProtocolNotify(&guid, evt, &reg);

    void *iface;
    bs->HandleProtocol((EFI_HANDLE)0, &guid, &iface);
    bs->LocateProtocol(&guid, NULL, &iface);

    UINTN count;
    EFI_HANDLE *handles;
    bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles);

    bs->Stall(1000);
    bs->Exit((EFI_HANDLE)0, EFI_ABORTED, 0, NULL);
}

// ===================================================================
// Runtime Services
// ===================================================================

static void
test_runtime_services(
    EFI_RUNTIME_SERVICES  *rt
    )
{
    EFI_TIME time;
    rt->GetTime(&time, NULL);
}

// ===================================================================
// Protocol struct access
// ===================================================================

static void
test_shell_protocol(
    EFI_SHELL_PROTOCOL  *shell
    )
{
    SHELL_FILE_HANDLE fh;
    UINTN size = 0;
    UINT64 pos;
    char buf[1];

    shell->OpenFileByName((CHAR16 *)0, &fh, EFI_FILE_MODE_READ);
    shell->ReadFile(fh, &size, buf);
    shell->WriteFile(fh, &size, buf);
    shell->GetFilePosition(fh, &pos);
    shell->SetFilePosition(fh, pos);
    shell->GetFileSize(fh, &pos);
    (void)shell->GetFileInfo(fh);
    shell->DeleteFileByName((CHAR16 *)0);
    shell->CloseFile(fh);
    (void)shell->ExecutionBreak;
}

static void
test_tcp4_protocol(
    EFI_TCP4_PROTOCOL  *tcp4
    )
{
    EFI_TCP4_CONFIG_DATA config = {0};
    tcp4->Configure(tcp4, &config);

    EFI_TCP4_CONNECTION_TOKEN conn = {0};
    tcp4->Connect(tcp4, &conn);

    EFI_TCP4_LISTEN_TOKEN listen = {0};
    tcp4->Accept(tcp4, &listen);

    EFI_TCP4_IO_TOKEN io = {0};
    tcp4->Transmit(tcp4, &io);
    tcp4->Receive(tcp4, &io);

    EFI_TCP4_CLOSE_TOKEN close = {0};
    tcp4->Close(tcp4, &close);

    tcp4->Cancel(tcp4, &conn.CompletionToken);
    tcp4->Poll(tcp4);

    EFI_TCP4_CONNECTION_STATE state;
    tcp4->GetModeData(tcp4, &state, NULL, NULL, NULL, NULL);
}

static void
test_ip4_protocol(
    EFI_IP4_PROTOCOL  *ip4
    )
{
    EFI_IP4_CONFIG_DATA config = {0};
    ip4->Configure(ip4, &config);

    EFI_IP4_COMPLETION_TOKEN token = {0};
    ip4->Transmit(ip4, &token);
    ip4->Receive(ip4, &token);
    ip4->Cancel(ip4, &token);
    ip4->Poll(ip4);
}

static void
test_dns4_protocol(
    EFI_DNS4_PROTOCOL  *dns4
    )
{
    EFI_DNS4_CONFIG_DATA config = {0};
    dns4->Configure(dns4, &config);

    EFI_DNS4_COMPLETION_TOKEN token = {0};
    dns4->HostNameToIp(dns4, (CHAR16 *)0, &token);
    dns4->Poll(dns4);
    dns4->Cancel(dns4, &token);
}

static void
test_service_binding(
    EFI_SERVICE_BINDING_PROTOCOL  *sb
    )
{
    EFI_HANDLE child = NULL;
    sb->CreateChild(sb, &child);
    sb->DestroyChild(sb, child);
}

static void
test_mp_protocol(
    EFI_MP_SERVICES_PROTOCOL  *mp
    )
{
    UINTN num, enabled;
    mp->GetNumberOfProcessors(mp, &num, &enabled);

    UINTN me;
    mp->WhoAmI(mp, &me);

    EFI_PROCESSOR_INFORMATION info;
    mp->GetProcessorInfo(mp, 0, &info);
    (void)(info.StatusFlag & PROCESSOR_ENABLED_BIT);
}

static void
test_smbios_protocol(
    EFI_SMBIOS_PROTOCOL  *smbios
    )
{
    EFI_SMBIOS_HANDLE handle = SMBIOS_HANDLE_PI_RESERVED;
    EFI_SMBIOS_TYPE type = 0;
    EFI_SMBIOS_TABLE_HEADER *record;
    smbios->GetNext(smbios, &handle, &type, &record, NULL);
}

static void
test_ip4_config2(
    EFI_IP4_CONFIG2_PROTOCOL  *cfg
    )
{
    UINTN size = 0;
    cfg->GetData(cfg, Ip4Config2DataTypeDnsServer, &size, NULL);
}

// ===================================================================
// GUID references (verify all compile)
// ===================================================================

static void
test_guids(void)
{
    (void)gEfiTcp4ServiceBindingProtocolGuid;
    (void)gEfiTcp4ProtocolGuid;
    (void)gEfiIp4ServiceBindingProtocolGuid;
    (void)gEfiIp4ProtocolGuid;
    (void)gEfiIp4Config2ProtocolGuid;
    (void)gEfiDns4ServiceBindingProtocolGuid;
    (void)gEfiDns4ProtocolGuid;
    (void)gEfiShellProtocolGuid;
    (void)gEfiMpServicesProtocolGuid;
    (void)gEfiSmbiosProtocolGuid;
    (void)SMBIOS_TABLE_GUID;
    (void)SMBIOS3_TABLE_GUID;
}

// ===================================================================
// GUID comparison utility
// ===================================================================

static void
test_guid_equal(void)
{
    EFI_GUID a = gEfiTcp4ProtocolGuid;
    EFI_GUID b = gEfiTcp4ProtocolGuid;
    (void)axl_efi_guid_equal(&a, &b);
}
