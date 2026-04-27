/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend-native.c
    AxlBackend implementation.

    Uses AXL's own UEFI type headers (include/uefi/).
    Global firmware table pointers (gST, gBS, gRT) are initialized
    by the CRT0 (axl-crt0-native.c) before any backend function is
    called.
**/

#include "axl-backend.h"
#include <axl/axl-log.h>
#include <stdarg.h>

AXL_LOG_DOMAIN("backend");

// ===================================================================
// Global firmware table pointers (set by CRT0)
// ===================================================================

EFI_SYSTEM_TABLE     *gST = NULL;
EFI_BOOT_SERVICES    *gBS = NULL;
EFI_RUNTIME_SERVICES *gRT = NULL;
EFI_HANDLE            gImageHandle = NULL;

// ===================================================================
// Shell protocol (cached on first file operation)
// ===================================================================

static EFI_SHELL_PROTOCOL  *mShell = NULL;
static bool                 mShellLocated = false;

static EFI_SHELL_PROTOCOL *
get_shell(void)
{
    if (!mShellLocated) {
        mShellLocated = true;
        EFI_GUID guid = gEfiShellProtocolGuid;
        gBS->LocateProtocol(&guid, NULL, (VOID **)&mShell);
    }
    return mShell;
}

// ===================================================================
// Memory
// ===================================================================

/**
 * @brief Allocate memory from the firmware allocator.
 *
 * @return pointer to allocated memory, or NULL on failure.
 */
void *
axl_backend_alloc(
    size_t  size  ///< bytes to allocate
    )
{
    VOID       *ptr = NULL;
    EFI_STATUS  status;

    status = gBS->AllocatePool(EfiBootServicesData, (UINTN)size, &ptr);
    return EFI_ERROR(status) ? NULL : ptr;
}

/**
 * @brief Allocate zeroed memory from the firmware allocator.
 *
 * @return pointer to zero-filled memory, or NULL on failure.
 */
void *
axl_backend_alloc_zero(
    size_t  size  ///< bytes to allocate
    )
{
    void *ptr = axl_backend_alloc(size);
    if (ptr != NULL) {
        uint8_t *p = (uint8_t *)ptr;
        for (size_t i = 0; i < size; i++) {
            p[i] = 0;
        }
    }
    return ptr;
}

/**
 * @brief Free firmware-allocated memory. NULL-safe.
 */
void
axl_backend_free(
    void  *ptr  ///< pointer from axl_backend_alloc, or NULL
    )
{
    if (ptr != NULL) {
        gBS->FreePool(ptr);
    }
}

// ===================================================================
// Console
// ===================================================================

/**
 * @brief Write a UCS-2 string to the console. NULL-safe.
 */
void
axl_backend_console_write(
    const unsigned short  *str  ///< UCS-2 string to output
    )
{
    if (gST != NULL && gST->ConOut != NULL && str != NULL) {
        gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)str);
    }
}

/**
 * @brief Set console text attribute (color/style).
 */
void
axl_backend_console_set_attr(
    uint32_t  attr  ///< attribute bitmask
    )
{
    if (gST != NULL && gST->ConOut != NULL) {
        gST->ConOut->SetAttribute(gST->ConOut, attr);
    }
}

/**
 * @brief Get current console text attribute.
 *
 * @return current attribute bitmask, or 0 if unavailable.
 */
uint32_t
axl_backend_console_get_attr(
    void
    )
{
    if (gST != NULL && gST->ConOut != NULL) {
        return (uint32_t)gST->ConOut->Mode->Attribute;
    }
    return 0;
}

// ===================================================================
// Time
// ===================================================================

/**
 * @brief Get the current date/time from firmware.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_get_time(
    AxlTime  *time  ///< (out) receives current time
    )
{
    EFI_TIME    efi_time;
    EFI_STATUS  status;

    if (time == NULL) {
        return -1;
    }

    status = gRT->GetTime(&efi_time, NULL);
    if (EFI_ERROR(status)) {
        return -1;
    }

    time->year       = efi_time.Year;
    time->month      = efi_time.Month;
    time->day        = efi_time.Day;
    time->hour       = efi_time.Hour;
    time->minute     = efi_time.Minute;
    time->second     = efi_time.Second;
    time->nanosecond = efi_time.Nanosecond;
    return 0;
}

// ===================================================================
// Low-level platform I/O (for AxlIpmi, future AxlPci/AxlSpd)
// ===================================================================

int
axl_backend_io_read8(uint16_t port, uint8_t *value)
{
    if (value == NULL) {
        return -1;
    }
#if defined(__x86_64__) || defined(__i386__)
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    *value = v;
    return 0;
#else
    (void)port;
    return -1;
#endif
}

int
axl_backend_io_write8(uint16_t port, uint8_t value)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
    return 0;
#else
    (void)port;
    (void)value;
    return -1;
#endif
}

// ===================================================================
// File I/O (via EFI_SHELL_PROTOCOL)
// ===================================================================

/**
 * @brief Open a file by UCS-2 path.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_file_open(
    const unsigned short  *path,        ///< UCS-2 file path
    uint64_t               mode,        ///< EFI file mode flags
    uint64_t               attributes,  ///< EFI file attributes
    AxlFileHandle         *handle       ///< (out) receives file handle
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    SHELL_FILE_HANDLE    fh;
    EFI_STATUS           status;

    (void)attributes;

    if (path == NULL || handle == NULL) {
        return -1;
    }

    shell = get_shell();
    if (shell == NULL) {
        return -1;
    }

    status = shell->OpenFileByName((CHAR16 *)path, &fh, mode);
    if (EFI_ERROR(status)) {
        axl_debug("file open failed: status=0x%llx", (unsigned long long)status);
        return -1;
    }
    *handle = (AxlFileHandle)fh;
    return 0;
}

/**
 * @brief Close a file handle. NULL-safe.
 *
 * @return 0 on success.
 */
int
axl_backend_file_close(
    AxlFileHandle  *handle  ///< file handle (set to NULL on close)
    )
{
    EFI_SHELL_PROTOCOL  *shell;

    if (handle == NULL || *handle == NULL) {
        return 0;
    }

    shell = get_shell();
    if (shell != NULL) {
        shell->CloseFile((SHELL_FILE_HANDLE)*handle);
    }
    *handle = NULL;
    return 0;
}

/**
 * @brief Read from a file.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_file_read(
    AxlFileHandle  handle,  ///< file handle
    size_t        *size,    ///< (in/out) bytes to read / bytes read
    void          *buf      ///< destination buffer
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;
    UINTN                usize;

    if (size == NULL || buf == NULL) {
        return -1;
    }

    shell = get_shell();
    if (shell == NULL) {
        return -1;
    }

    usize = *size;
    status = shell->ReadFile((SHELL_FILE_HANDLE)handle, &usize, buf);
    *size = usize;
    return EFI_ERROR(status) ? -1 : 0;
}

/**
 * @brief Write to a file.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_file_write(
    AxlFileHandle  handle,  ///< file handle
    size_t        *size,    ///< (in/out) bytes to write / bytes written
    const void    *buf      ///< source buffer
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;
    UINTN                usize;

    if (size == NULL || buf == NULL) {
        return -1;
    }

    shell = get_shell();
    if (shell == NULL) {
        return -1;
    }

    usize = *size;
    status = shell->WriteFile((SHELL_FILE_HANDLE)handle, &usize,
                               (VOID *)buf);
    *size = usize;
    return EFI_ERROR(status) ? -1 : 0;
}

/**
 * @brief Get current file position.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_file_get_position(
    AxlFileHandle  handle,  ///< file handle
    uint64_t      *pos      ///< (out) receives position
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;
    UINT64               efi_pos;

    if (pos == NULL) {
        return -1;
    }

    shell = get_shell();
    if (shell == NULL) {
        return -1;
    }

    status = shell->GetFilePosition((SHELL_FILE_HANDLE)handle, &efi_pos);
    if (!EFI_ERROR(status)) {
        *pos = efi_pos;
    }
    return EFI_ERROR(status) ? -1 : 0;
}

/**
 * @brief Set file position.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_file_set_position(
    AxlFileHandle  handle,  ///< file handle
    uint64_t       pos      ///< byte offset from start
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL) {
        return -1;
    }

    status = shell->SetFilePosition((SHELL_FILE_HANDLE)handle, pos);
    return EFI_ERROR(status) ? -1 : 0;
}

/**
 * @brief Delete a file by UCS-2 path.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_backend_file_delete(
    const unsigned short  *path  ///< UCS-2 file path
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;

    if (path == NULL) {
        return -1;
    }

    shell = get_shell();
    if (shell == NULL) {
        return -1;
    }

    status = shell->DeleteFileByName((CHAR16 *)path);
    return EFI_ERROR(status) ? -1 : 0;
}

/**
 * @brief Get file size.
 *
 * @return file size in bytes, or -1 on error.
 */
int64_t
axl_backend_file_get_size(
    AxlFileHandle  handle  ///< file handle
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    UINT64               size;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL) {
        return -1;
    }

    status = shell->GetFileSize((SHELL_FILE_HANDLE)handle, &size);
    return EFI_ERROR(status) ? -1 : (int64_t)size;
}

/**
 * @brief Check if a path is a directory.
 *
 * @return true if path is a directory.
 */
bool
axl_backend_file_is_dir(
    const unsigned short  *path  ///< UCS-2 path
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    SHELL_FILE_HANDLE    fh;
    EFI_FILE_INFO       *info;
    EFI_STATUS           status;
    bool                 is_dir = false;

    shell = get_shell();
    if (shell == NULL) {
        return false;
    }

    status = shell->OpenFileByName((CHAR16 *)path, &fh,
                                    AXL_FILE_MODE_READ);
    if (EFI_ERROR(status)) {
        return false;
    }

    info = (EFI_FILE_INFO *)shell->GetFileInfo(fh);
    if (info != NULL) {
        is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
        axl_backend_free(info);
    }
    shell->CloseFile(fh);
    return is_dir;
}

int
axl_backend_file_rename(
    const unsigned short  *old_path,
    const unsigned short  *new_path
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    SHELL_FILE_HANDLE    fh;
    EFI_FILE_INFO       *info;
    EFI_STATUS           status;
    size_t               new_len;
    size_t               info_size;
    EFI_FILE_INFO       *new_info;
    size_t               i;

    shell = get_shell();
    if (shell == NULL || old_path == NULL || new_path == NULL) {
        return -1;
    }

    /* Open the existing file */
    status = shell->OpenFileByName((CHAR16 *)old_path, &fh,
                                    AXL_FILE_MODE_READ | AXL_FILE_MODE_WRITE);
    if (EFI_ERROR(status)) {
        return -1;
    }

    /* Get current file info */
    info = (EFI_FILE_INFO *)shell->GetFileInfo(fh);
    if (info == NULL) {
        shell->CloseFile(fh);
        return -1;
    }

    /* Build new info with the new filename */
    for (new_len = 0; new_path[new_len] != 0; new_len++) {}
    info_size = sizeof(EFI_FILE_INFO) + (new_len + 1) * sizeof(CHAR16);

    new_info = (EFI_FILE_INFO *)axl_backend_alloc(info_size);
    if (new_info == NULL) {
        axl_backend_free(info);
        shell->CloseFile(fh);
        return -1;
    }

    /* Copy metadata, replace filename */
    *new_info = *info;
    new_info->Size = info_size;
    for (i = 0; i <= new_len; i++) {
        new_info->FileName[i] = (CHAR16)new_path[i];
    }

    status = shell->SetFileInfo(fh, (CONST EFI_FILE_INFO *)new_info);
    axl_backend_free(new_info);
    axl_backend_free(info);
    shell->CloseFile(fh);

    return EFI_ERROR(status) ? -1 : 0;
}

int
axl_backend_file_mkdir(
    const unsigned short  *path
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    SHELL_FILE_HANDLE    fh;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL || path == NULL) {
        return -1;
    }

    status = shell->CreateFile((CHAR16 *)path, EFI_FILE_DIRECTORY, &fh);
    if (EFI_ERROR(status)) {
        return -1;
    }
    shell->CloseFile(fh);
    return 0;
}

int
axl_backend_file_rmdir(
    const unsigned short  *path
    )
{
    /* DeleteFileByName works for both files and empty directories */
    return axl_backend_file_delete(path);
}

int
axl_backend_file_stat(
    const unsigned short  *path,
    uint64_t              *size,
    uint64_t              *alloc_size,
    bool                  *is_dir,
    bool                  *read_only
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    SHELL_FILE_HANDLE    fh;
    EFI_FILE_INFO       *info;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL || path == NULL) {
        return -1;
    }

    status = shell->OpenFileByName((CHAR16 *)path, &fh,
                                    AXL_FILE_MODE_READ);
    if (EFI_ERROR(status)) {
        return -1;
    }

    info = (EFI_FILE_INFO *)shell->GetFileInfo(fh);
    if (info == NULL) {
        shell->CloseFile(fh);
        return -1;
    }

    if (size != NULL) {
        *size = info->FileSize;
    }
    if (alloc_size != NULL) {
        *alloc_size = info->PhysicalSize;
    }
    if (is_dir != NULL) {
        *is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
    }
    if (read_only != NULL) {
        *read_only = (info->Attribute & EFI_FILE_READ_ONLY) != 0;
    }

    axl_backend_free(info);
    shell->CloseFile(fh);
    return 0;
}

// ===================================================================
// Shell environment, working directory, and command execution
// ===================================================================

const unsigned short *
axl_backend_shell_getenv(
    const unsigned short  *name
    )
{
    EFI_SHELL_PROTOCOL  *shell;

    shell = get_shell();
    if (shell == NULL) {
        return NULL;
    }
    return (const unsigned short *)shell->GetEnv((CONST CHAR16 *)name);
}

int
axl_backend_shell_setenv(
    const unsigned short  *name,
    const unsigned short  *value,
    bool                   volatile_var
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL || name == NULL) {
        return -1;
    }

    status = shell->SetEnv((CONST CHAR16 *)name, (CONST CHAR16 *)value,
                            volatile_var ? TRUE : FALSE);
    return EFI_ERROR(status) ? -1 : 0;
}

const unsigned short *
axl_backend_shell_getcwd(
    void
    )
{
    EFI_SHELL_PROTOCOL  *shell;

    shell = get_shell();
    if (shell == NULL) {
        return NULL;
    }
    return (const unsigned short *)shell->GetCurDir(NULL);
}

int
axl_backend_shell_chdir(
    const unsigned short  *path
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL || path == NULL) {
        return -1;
    }

    status = shell->SetCurDir(NULL, (CONST CHAR16 *)path);
    return EFI_ERROR(status) ? -1 : 0;
}

int
axl_backend_shell_execute(
    const unsigned short  *command
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL || command == NULL) {
        return -1;
    }

    status = shell->Execute(NULL, (CHAR16 *)command, NULL, NULL);
    return EFI_ERROR(status) ? -1 : 0;
}

// ===================================================================
// Wide-string operations (self-implemented — no external library)
// ===================================================================

/**
 * @brief Get UCS-2 string length.
 *
 * @return number of characters (not including NUL).
 */
size_t
axl_backend_wcslen(
    const unsigned short  *s  ///< UCS-2 string
    )
{
    size_t n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

/**
 * @brief Compare two UCS-2 strings.
 *
 * @return <0, 0, or >0.
 */
int
axl_backend_wcscmp(
    const unsigned short  *a,  ///< first string
    const unsigned short  *b   ///< second string
    )
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)*a - (int)*b;
}

/**
 * @brief Copy UCS-2 string with size limit.
 */
void
axl_backend_wcscpy(
    unsigned short        *dst,        ///< destination buffer
    const unsigned short  *src,        ///< source string
    size_t                 dst_count   ///< destination buffer size in characters
    )
{
    size_t i;
    if (dst_count == 0) {
        return;
    }
    for (i = 0; i < dst_count - 1 && src[i] != 0; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

/**
 * @brief Convert UCS-2 character to uppercase.
 *
 * @return uppercase character.
 */
unsigned short
axl_backend_towupper(
    unsigned short  c  ///< character to convert
    )
{
    if (c >= L'a' && c <= L'z') {
        return c - L'a' + L'A';
    }
    return c;
}

/**
 * @brief Find substring in UCS-2 string.
 *
 * @return pointer to first occurrence, or NULL if not found.
 */
const unsigned short *
axl_backend_wcsstr(
    const unsigned short  *haystack,  ///< string to search
    const unsigned short  *needle     ///< substring to find
    )
{
    const unsigned short  *h = haystack;
    const unsigned short  *n = needle;
    size_t                 nlen;

    if (h == NULL || n == NULL) {
        return NULL;
    }

    nlen = 0;
    while (n[nlen] != 0) {
        nlen++;
    }
    if (nlen == 0) {
        return haystack;
    }

    for (; *h != 0; h++) {
        if (*h == *n) {
            size_t i;
            for (i = 1; i < nlen && h[i] == n[i]; i++) {
            }
            if (i == nlen) {
                return (const unsigned short *)h;
            }
        }
    }

    return NULL;
}

// ===================================================================
// Events and timers
// ===================================================================

int
axl_backend_event_create_timer(
    AxlEventHandle  *event
    )
{
    EFI_STATUS  status;

    if (event == NULL) {
        return -1;
    }

    status = gBS->CreateEvent(EVT_TIMER, TPL_APPLICATION,
                              NULL, NULL, (EFI_EVENT *)event);
    return EFI_ERROR(status) ? -1 : 0;
}

int
axl_backend_event_create(
    AxlEventHandle  *event
    )
{
    EFI_STATUS  status;

    if (event == NULL) {
        return -1;
    }

    status = gBS->CreateEvent(0, TPL_APPLICATION,
                              NULL, NULL, (EFI_EVENT *)event);
    return EFI_ERROR(status) ? -1 : 0;
}

void
axl_backend_event_close(
    AxlEventHandle  event
    )
{
    if (event != NULL) {
        gBS->CloseEvent((EFI_EVENT)event);
    }
}

int
axl_backend_event_set_timer(
    AxlEventHandle  event,
    int             type,
    uint64_t        interval_100ns
    )
{
    EFI_STATUS  status;

    status = gBS->SetTimer((EFI_EVENT)event, (EFI_TIMER_DELAY)type,
                           interval_100ns);
    return EFI_ERROR(status) ? -1 : 0;
}

int
axl_backend_event_wait(
    size_t          count,
    AxlEventHandle  *events,
    size_t          *fired_index
    )
{
    EFI_STATUS  status;
    UINTN       index;

    status = gBS->WaitForEvent((UINTN)count, (EFI_EVENT *)events, &index);
    if (EFI_ERROR(status)) {
        return -1;
    }
    *fired_index = (size_t)index;
    return 0;
}

int
axl_backend_event_check(
    AxlEventHandle  event
    )
{
    EFI_STATUS  status;

    status = gBS->CheckEvent((EFI_EVENT)event);
    if (status == EFI_SUCCESS) {
        return 0;
    }
    if (status == EFI_NOT_READY) {
        return 1;
    }
    return -1;
}

int
axl_backend_event_register_protocol_notify(
    void            *guid,
    AxlEventHandle   event,
    void           **registration
    )
{
    EFI_STATUS  status;

    status = gBS->RegisterProtocolNotify((EFI_GUID *)guid,
                                         (EFI_EVENT)event,
                                         registration);
    return EFI_ERROR(status) ? -1 : 0;
}

// ===================================================================
// Console input
// ===================================================================

AxlEventHandle
axl_backend_console_wait_for_key(
    void
    )
{
    if (gST == NULL || gST->ConIn == NULL) {
        return NULL;
    }
    return (AxlEventHandle)gST->ConIn->WaitForKey;
}

int
axl_backend_console_read_key(
    uint16_t  *scan_code,
    uint16_t  *unicode_char
    )
{
    EFI_INPUT_KEY  key;
    EFI_STATUS     status;

    if (gST == NULL || gST->ConIn == NULL) {
        return -1;
    }

    status = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
    if (EFI_ERROR(status)) {
        return -1;
    }

    if (scan_code != NULL) {
        *scan_code = key.ScanCode;
    }
    if (unicode_char != NULL) {
        *unicode_char = key.UnicodeChar;
    }
    return 0;
}

bool
axl_backend_shell_break_flag(
    void
    )
{
    EFI_SHELL_PROTOCOL  *shell;
    EFI_STATUS           status;

    shell = get_shell();
    if (shell == NULL) {
        return false;
    }

    status = gBS->CheckEvent(shell->ExecutionBreak);
    return (status == EFI_SUCCESS);
}

// ---------------------------------------------------------------------------
// Serial Ctrl-C bridge
//
// EDK2's TerminalDxe (the serial-console driver) delivers raw 0x03
// bytes from the wire with KeyShiftState=0 — serial protocol carries
// no shift-state info and OVMF can't synthesize it. The Shell's
// CtrlCNotifyHandle{1..4} all require KeyShiftState to indicate Ctrl
// pressed, so they never match on serial input → ExecutionBreak
// never signals → axl_loop_run waits forever.
//
// Fix: register our own SimpleTextInputEx KeyNotify on ConsoleInHandle
// for {UnicodeChar=0x03, KeyShiftState=0}. When it fires, signal
// shell->ExecutionBreak directly (the event the loop already waits
// on). Lazy-init on first call to axl_backend_shell_break_event so
// drivers and apps that never use the loop pay nothing.
// ---------------------------------------------------------------------------

static bool   mSerialCtrlCInstalled = false;
static VOID  *mSerialCtrlCHandle    = NULL;

static EFI_STATUS EFIAPI
serial_ctrl_c_notify(
    IN EFI_KEY_DATA  *KeyData
    )
{
    (void)KeyData;
    EFI_SHELL_PROTOCOL *shell = get_shell();
    if (shell != NULL && shell->ExecutionBreak != NULL) {
        gBS->SignalEvent(shell->ExecutionBreak);
    }
    return EFI_SUCCESS;
}

static void
install_serial_ctrl_c_notify(void)
{
    if (mSerialCtrlCInstalled) {
        return;
    }
    mSerialCtrlCInstalled = true;   /* one-shot regardless of outcome */

    EFI_GUID guid = gEfiSimpleTextInputExProtocolGuid;

    /* Register on every handle exposing SimpleTextInputEx — each
     * physical console driver (TerminalDxe per serial port + any
     * keyboard driver). ConSplitter aggregates these but its
     * forward-to-children path can race with attach order; doing
     * this directly avoids the timing-sensitive case. */
    EFI_HANDLE *handles = NULL;
    UINTN       handle_count = 0;
    EFI_STATUS  status = gBS->LocateHandleBuffer(
        ByProtocol, &guid, NULL, &handle_count, &handles);
    if (EFI_ERROR(status) || handles == NULL || handle_count == 0) {
        axl_warning("serial Ctrl-C: LocateHandleBuffer found 0 (status=0x%lx)",
                    (unsigned long)status);
        return;
    }

    EFI_KEY_DATA key_data = {0};
    key_data.Key.UnicodeChar         = 0x03;       /* ETX, raw Ctrl-C byte */
    key_data.Key.ScanCode            = 0;
    key_data.KeyState.KeyShiftState  = 0;          /* matches TerminalDxe */
    key_data.KeyState.KeyToggleState = 0;

    UINTN installed = 0;
    for (UINTN i = 0; i < handle_count; i++) {
        EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *simple_ex = NULL;
        if (gBS->HandleProtocol(handles[i], &guid, (void **)&simple_ex) != 0
            || simple_ex == NULL) {
            continue;
        }
        VOID *notify_handle = NULL;
        if (simple_ex->RegisterKeyNotify(simple_ex, &key_data,
                                         serial_ctrl_c_notify,
                                         &notify_handle) == 0) {
            installed++;
            /* Save the first handle for symmetry — most common case. */
            if (mSerialCtrlCHandle == NULL) {
                mSerialCtrlCHandle = notify_handle;
            }
        }
    }
    gBS->FreePool(handles);

    if (installed == 0) {
        axl_warning("serial Ctrl-C: registered on 0 handles");
    } else {
        axl_info("serial Ctrl-C bridge installed on %lu handle%s",
                 (unsigned long)installed, installed == 1 ? "" : "s");
    }
}

AxlEventHandle
axl_backend_shell_break_event(
    void
    )
{
    EFI_SHELL_PROTOCOL *shell;

    shell = get_shell();
    if (shell == NULL) {
        return NULL;
    }

    /* Make sure serial Ctrl-C reaches ExecutionBreak. Real-keyboard
     * Ctrl-C still goes through the Shell's own Ctrl-modifier notify
     * unchanged; this is purely additive for the serial-console
     * case (BMC, IPMI SoL, qemu -nographic, etc.). */
    install_serial_ctrl_c_notify();

    return (AxlEventHandle)shell->ExecutionBreak;
}

// ===================================================================
// MP Services (full implementation — direct protocol calls)
// ===================================================================

struct AxlMpContext {
    EFI_MP_SERVICES_PROTOCOL  *mp;
    UINTN                     *ap_numbers;  ///< maps index to processor number
    size_t                     count;
};

AxlMpContext *
axl_backend_mp_init(
    size_t  *worker_count
    )
{
    EFI_STATUS                  status;
    EFI_MP_SERVICES_PROTOCOL   *mp;
    UINTN                       num_proc;
    UINTN                       num_enabled;
    UINTN                       bsp_number;
    UINTN                       i;
    size_t                      slot;
    AxlMpContext               *ctx;
    EFI_GUID                    mp_guid = gEfiMpServicesProtocolGuid;

    if (worker_count != NULL) {
        *worker_count = 0;
    }

    status = gBS->LocateProtocol(&mp_guid, NULL, (void **)&mp);
    if (EFI_ERROR(status)) {
        return NULL;
    }

    status = mp->GetNumberOfProcessors(mp, &num_proc, &num_enabled);
    if (EFI_ERROR(status) || num_enabled <= 1) {
        return NULL;
    }

    status = mp->WhoAmI(mp, &bsp_number);
    if (EFI_ERROR(status)) {
        return NULL;
    }

    ctx = axl_backend_alloc_zero(sizeof(AxlMpContext));
    if (ctx == NULL) {
        return NULL;
    }

    ctx->mp = mp;
    ctx->ap_numbers = axl_backend_alloc_zero(
                          (num_enabled - 1) * sizeof(UINTN));
    if (ctx->ap_numbers == NULL) {
        axl_backend_free(ctx);
        return NULL;
    }

    /* Enumerate enabled APs (skip BSP) */
    slot = 0;
    for (i = 0; i < num_proc && slot < num_enabled - 1; i++) {
        EFI_PROCESSOR_INFORMATION  proc_info;

        if (i == bsp_number) {
            continue;
        }

        status = mp->GetProcessorInfo(mp, i, &proc_info);
        if (EFI_ERROR(status) ||
            !(proc_info.StatusFlag & PROCESSOR_ENABLED_BIT)) {
            continue;
        }

        ctx->ap_numbers[slot] = i;
        slot++;
    }

    ctx->count = slot;
    if (ctx->count == 0) {
        axl_backend_free(ctx->ap_numbers);
        axl_backend_free(ctx);
        return NULL;
    }

    if (worker_count != NULL) {
        *worker_count = ctx->count;
    }
    return ctx;
}

int
axl_backend_mp_start_ap(
    AxlMpContext  *ctx,
    size_t         ap_index,
    AxlApProc      proc,
    void          *arg
    )
{
    EFI_STATUS  status;
    EFI_EVENT   ap_event;

    if (ctx == NULL || ap_index >= ctx->count || proc == NULL) {
        return -1;
    }

    /* Create temp event for non-blocking StartupThisAP */
    status = gBS->CreateEvent(0, TPL_APPLICATION, NULL, NULL, &ap_event);
    if (EFI_ERROR(status)) {
        return -1;
    }

    status = ctx->mp->StartupThisAP(
                 ctx->mp,
                 (EFI_AP_PROCEDURE)proc,
                 ctx->ap_numbers[ap_index],
                 ap_event,
                 0,
                 arg,
                 NULL);

    gBS->CloseEvent(ap_event);
    return EFI_ERROR(status) ? -1 : 0;
}

size_t
axl_backend_mp_get_ap_number(
    AxlMpContext  *ctx,
    size_t         ap_index
    )
{
    if (ctx == NULL || ap_index >= ctx->count) {
        return 0;
    }
    return (size_t)ctx->ap_numbers[ap_index];
}

void
axl_backend_mp_cleanup(
    AxlMpContext  *ctx
    )
{
    if (ctx == NULL) {
        return;
    }
    axl_backend_free(ctx->ap_numbers);
    axl_backend_free(ctx);
}

// ===================================================================
// Miscellaneous Boot Services
// ===================================================================

void
axl_backend_stall(
    uint64_t  microseconds
    )
{
    gBS->Stall((UINTN)microseconds);
}

void
axl_backend_event_signal(
    AxlEventHandle  event
    )
{
    if (event != NULL) {
        gBS->SignalEvent((EFI_EVENT)event);
    }
}

// ===================================================================
// App termination
// ===================================================================

void
axl_backend_boot_exit(int rc)
{
    /* Map C-style rc to an EFI_STATUS the firmware can use. */
    EFI_STATUS status = (rc == 0) ? EFI_SUCCESS : EFI_ABORTED;
    gBS->Exit(gImageHandle, status, 0, NULL);

    /* gBS->Exit is specified as NORETURN for the image's own handle;
     * if it ever does return (e.g. firmware bug) we still have to not
     * return from this NORETURN function. Spin to satisfy the compiler
     * and trap obvious misbehavior. */
    for (;;) {
        gBS->Stall(1000000);
    }
}
