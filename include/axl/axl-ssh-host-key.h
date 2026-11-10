/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-ssh-host-key.h
 * @brief An SSH server's host key: where it is kept, how a provisioned one is
 *        read, and the fingerprint operators compare.
 *
 * Two halves, and which half a function is in is part of its contract:
 *
 * **Provider-free** -- axl_ssh_host_key_parse(), axl_ssh_host_key_read_file(),
 * axl_ssh_host_key_nv_read(), axl_ssh_host_key_nv_write(),
 * axl_ssh_host_key_nv_delete(), axl_ssh_host_key_fingerprint(),
 * axl_ssh_host_key_public_line() and axl_ssh_host_key_record_clear() use
 * SHA-256 and base64 only. An image can call them without linking the
 * Ed25519 provider (~120 KB; see axl_pk_enable_ed25519()). That is what lets
 * a small launcher check, print and provision a key that a resident driver
 * serves.
 *
 * **Provider-bound** -- axl_ssh_host_key_load(),
 * axl_ssh_host_key_record_from_key() and axl_ssh_host_key_nv_load_or_create()
 * need the Ed25519 provider, as AxlSsh itself does.
 *
 * **Where a key is kept by default.** A UEFI variable, NON-VOLATILE and
 * BOOT-SERVICES ONLY: #AXL_SSH_HOST_KEY_VAR under #AXL_SSH_HOST_KEY_GUID.
 * The identity belongs to the MACHINE, not to the medium sshd runs from: a
 * stick or BMC virtual media moved to another machine finds or makes that
 * machine's own key. An application with a configuration store of its own
 * keeps its key there instead, through axl_ssh_host_key_record_from_key()
 * and axl_ssh_host_key_load(); the nv functions are for the default store.
 *
 * **What boot-services-only buys, and what it does not.** With no RUNTIME
 * access the key cannot be read through UEFI variable services after
 * ExitBootServices: an operating system's efivarfs does not list it, and it
 * cannot create, overwrite or delete it either. It is NOT secret from a
 * privileged operating system: the variable is stored unencrypted in the
 * platform's variable flash, which on many platforms the OS can read
 * directly. And clearing NVRAM -- "load defaults", many firmware updates --
 * removes it, so the next start makes a new key, which every client then
 * sees as a changed one.
 *
 * **A stored key that is present but unusable is never replaced
 * silently.** Regenerating would hide tampering and teach every client to
 * accept a changed key -- the habit host-key checking exists to break. The
 * caller is told why; replacing it is an explicit act
 * (axl_ssh_host_key_nv_delete(), then a load-or-create).
 *
 * **Every buffer that held private bytes is zeroed before it is freed, on
 * every path** -- the file, its transcoding, the base64 text and the decoded
 * key, and every variable payload. A provider-free caller that only wants
 * the public half (to print a fingerprint) still reads the private half into
 * an #AxlSshHostKeyRecord, and clears it with
 * axl_ssh_host_key_record_clear().
 *
 * The fingerprint is OpenSSH's own SHA256 form, so it compares character for
 * character with `ssh-keygen -lf` and with what ssh(1) prints at first
 * contact.
 */

#ifndef AXL_SSH_HOST_KEY_H
#define AXL_SSH_HOST_KEY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <axl/axl-crypto.h>
#include <axl/axl-macros.h>
#include <axl/axl-sys.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Name of the default host-key variable. The AxlSsh prefix makes it
    recognisable in `dmpstore` output, where the GUID alone means nothing. */
#define AXL_SSH_HOST_KEY_VAR  "AxlSshHostKeyEd25519"

/** @brief Vendor GUID of #AXL_SSH_HOST_KEY_VAR, as an AxlGuid initializer.
    Its textual form is #AXL_SSH_HOST_KEY_GUID_STR. */
#define AXL_SSH_HOST_KEY_GUID \
    AXL_GUID(0xef4ea128, 0x0f65, 0x4399, 0x98, 0x84, 0x9b, 0x0d, 0xe9, 0xf5, 0x4e, 0x9e)

/** @brief #AXL_SSH_HOST_KEY_GUID as text, in the form the Shell's
    `dmpstore -guid` and `setvar -guid` take. */
#define AXL_SSH_HOST_KEY_GUID_STR  "ef4ea128-0f65-4399-9884-9b0de9f54e9e"

/** @brief Raw Ed25519 public key length (RFC 8032). */
#define AXL_SSH_ED25519_PUBLIC_LEN  32u

/** @brief Ed25519 private key length as PKCS#8 v1 DER (RFC 8410): a fixed
    16-byte prefix and the 32-byte seed. */
#define AXL_SSH_HOST_KEY_PKCS8_LEN  48u

/** @brief Room for a fingerprint with its NUL: `SHA256:` and 43 base64
    characters, no padding. */
#define AXL_SSH_FINGERPRINT_SIZE  51u

/** @brief Room for a public line with its NUL: `ssh-ed25519 ` and 68 base64
    characters. */
#define AXL_SSH_PUBLIC_LINE_SIZE  81u

/** @brief Largest private-key file axl_ssh_host_key_parse() and
    axl_ssh_host_key_read_file() accept, in bytes as stored: about forty
    times a real Ed25519 key, which is ~400 bytes (~830 as UTF-16). */
#define AXL_SSH_HOST_KEY_FILE_MAX  16384u

/**
 * @brief One host key, as the functions in this header hand it over.
 *
 * An in-memory form only. It is NOT the layout of the stored variable,
 * which is a private, versioned format (see axl_ssh_host_key_nv_write()),
 * so this struct may grow without stored keys becoming unreadable.
 *
 * Holds PRIVATE material: clear it with axl_ssh_host_key_record_clear()
 * once used, on every path.
 */
typedef struct {
    uint8_t pkcs8[AXL_SSH_HOST_KEY_PKCS8_LEN];       ///< the private key, PKCS#8 DER
    uint8_t public_key[AXL_SSH_ED25519_PUBLIC_LEN];  ///< its public key, raw
} AxlSshHostKeyRecord;

/** @brief Where axl_ssh_host_key_nv_load_or_create() got its key. */
typedef enum {
    AXL_SSH_HOST_KEY_LOADED    = 0, /**< the stored key */
    AXL_SSH_HOST_KEY_GENERATED = 1, /**< none was stored; a new one was made
                                         and stored (a first start) */
    AXL_SSH_HOST_KEY_EPHEMERAL = 2  /**< none was stored; a new one was made
                                         and NOT stored -- the write failed,
                                         it did not read back, or the key
                                         came from weak entropy. It serves
                                         for now and the next start makes
                                         another: tell the operator */
} AxlSshHostKeyOrigin;

/**
 * @brief OpenSSH's fingerprint of an Ed25519 public key: `SHA256:` and the
 *        unpadded base64 of SHA-256 over the `ssh-ed25519` key blob.
 *
 * The same string `ssh-keygen -lf` prints (between the bit count and the
 * comment) and ssh(1) prints in "ED25519 key fingerprint is ...".
 * Provider-free.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL argument, @a len not
 *     #AXL_SSH_ED25519_PUBLIC_LEN, or @a cap below
 *     #AXL_SSH_FINGERPRINT_SIZE (@a out untouched); #AXL_NO_RESOURCES out
 *     of memory.
 */
AXL_WARN_UNUSED int
axl_ssh_host_key_fingerprint(
    const uint8_t *public_key,  ///< raw Ed25519 public key
    size_t         len,         ///< #AXL_SSH_ED25519_PUBLIC_LEN
    char          *out,         ///< [out] NUL-terminated fingerprint
    size_t         cap          ///< size of @a out, at least #AXL_SSH_FINGERPRINT_SIZE
);

/**
 * @brief The key as one OpenSSH public-key line, `ssh-ed25519 AAAA...`,
 *        with no comment.
 *
 * Prefixed with a host pattern it is a known_hosts line
 * (`[host]:2222 ssh-ed25519 AAAA...`). Provider-free.
 *
 * @return as axl_ssh_host_key_fingerprint(), with #AXL_SSH_PUBLIC_LINE_SIZE.
 */
AXL_WARN_UNUSED int
axl_ssh_host_key_public_line(
    const uint8_t *public_key,  ///< raw Ed25519 public key
    size_t         len,         ///< #AXL_SSH_ED25519_PUBLIC_LEN
    char          *out,         ///< [out] NUL-terminated line
    size_t         cap          ///< size of @a out, at least #AXL_SSH_PUBLIC_LINE_SIZE
);

/**
 * @brief Parse a private-key file's contents into a record. Provider-free.
 *
 * Accepted: **openssh-key-v1** (`-----BEGIN OPENSSH PRIVATE KEY-----`), what
 * `ssh-keygen -t ed25519 -N ''` writes -- one `ssh-ed25519` key, cipher and
 * KDF `none`. Nothing else: a PKCS#8 file holds only the seed, and deriving
 * the public key from it needs the Ed25519 provider, so it is refused with
 * the command that converts it.
 *
 * The text may be UTF-8 (a BOM allowed) or UTF-16LE with a BOM, with LF or
 * CRLF line ends, and need not end in a newline -- what a FAT volume edited
 * from Windows or the Shell's `edit` holds. OpenSSH refuses all of those
 * (its BEGIN marker is compared at offset 0, LF included); accepting them
 * changes only how the same bytes are carried. Anything before the BEGIN
 * line other than a BOM is refused, as OpenSSH refuses it; text after the
 * END line is ignored, as OpenSSH ignores it; spaces and tabs inside the
 * base64 are skipped, as OpenSSH's b64_pton skips them.
 *
 * Inside, everything is checked as OpenSSH's sshkey.c checks it
 * (private2_uudecode, private2_decrypt, sshkey_parse_private2): the magic,
 * one key, a fully consumed envelope public key, the encrypted length a
 * multiple of 8 with nothing after it, equal check values, a 32-byte public
 * and 64-byte secret key, the comment within bounds and free of NUL, the
 * 1, 2, 3...
 * padding, and the envelope's public key equal to the private section's.
 * Stricter than OpenSSH in two places: KDF options must be empty for KDF
 * `none`, and the 64-byte secret's public half must equal the public key.
 *
 * The key type is read from the envelope before the cipher, so an
 * encrypted key of another type is refused for its type.
 *
 * On failure @a *why is one of these static strings:
 *   - "not a private key file"
 *   - "file too large to be a host key" (over #AXL_SSH_HOST_KEY_FILE_MAX)
 *   - "passphrase-protected host keys are not supported"
 *   - "PKCS#8 keys are not supported; convert with ssh-keygen -p -N '' -f FILE"
 *   - "<type> host keys are not supported; only ssh-ed25519", for each of
 *     ssh-rsa, ssh-dss, ecdsa-sha2-nistp256, ecdsa-sha2-nistp384,
 *     ecdsa-sha2-nistp521, sk-ssh-ed25519@openssh.com,
 *     sk-ecdsa-sha2-nistp256@openssh.com and the `-cert-v01@openssh.com`
 *     certificate form of each and of ssh-ed25519; and for a traditional
 *     PEM file, "ssh-rsa", "ssh-dss" or "ecdsa" by its header
 *   - "unknown host key type; only ssh-ed25519 is supported"
 *   - "more than one key in the file"
 *   - "truncated or malformed key"
 *   - "corrupt key (check values differ)"
 *   - "corrupt key (bad padding)"
 *   - "corrupt key (public and private halves differ)"
 *   - "trailing data after the key"
 *
 * @return #AXL_OK with @a out filled; #AXL_UNSUPPORTED for a well-formed key
 *     that is refused (passphrase-protected, PKCS#8, another type);
 *     #AXL_INVALID for a NULL argument or anything malformed, corrupt or too
 *     large; #AXL_NO_RESOURCES out of memory. On failure @a out is zeroed.
 */
AXL_WARN_UNUSED int
axl_ssh_host_key_parse(
    const void          *data,  ///< file contents
    size_t               len,   ///< bytes in @a data
    AxlSshHostKeyRecord *out,   ///< [out] the key
    const char         **why    ///< [out] reason on failure, or NULL
);

/**
 * @brief Read @a path whole and axl_ssh_host_key_parse() it. Provider-free.
 *
 * The size is checked BEFORE the file is read, so a large file is refused
 * unread. FAT has no permissions, so there is no counterpart of OpenSSH's
 * "UNPROTECTED PRIVATE KEY FILE" check: a key file on an ESP or a stick is
 * readable by the operating system, by every UEFI application and by
 * whoever holds the medium. axl_ssh_host_key_nv_write() moves a key into
 * the variable store instead.
 *
 * @return as axl_ssh_host_key_parse(), plus #AXL_NOT_FOUND when @a path does
 *     not exist or is a directory (why "no such file"), and #AXL_IO_ERROR
 *     when it exists and cannot be read (why "cannot read the file").
 */
AXL_WARN_UNUSED int
axl_ssh_host_key_read_file(
    const char          *path,  ///< absolute path, e.g. `fs0:\ssh_host_ed25519_key`
    AxlSshHostKeyRecord *out,   ///< [out] the key
    const char         **why    ///< [out] reason on failure, or NULL
);

/**
 * @brief Read the stored host key WITHOUT loading it. Provider-free.
 *
 * Checks everything except that the public key belongs to the private key
 * (that needs the provider; axl_ssh_host_key_load() checks it): attributes
 * exactly non-volatile and boot-services, compared as the firmware reports
 * them (every bit, including those AxlNvstore does not model); the size;
 * the format's magic and version; and that the private key is an Ed25519
 * PKCS#8.
 *
 * Reasons on failure:
 *   - "the stored key is an authenticated variable; only its signer can
 *     delete it" -- refused, never used;
 *   - "the stored key is readable by the OS (runtime access)" -- refused,
 *     never used: after ExitBootServices the OS can create a variable under
 *     this GUID only with runtime access, so such a variable may be one it
 *     planted;
 *   - "the stored key has the wrong attributes (not NV+BS)";
 *   - "the stored key has the wrong size";
 *   - "the stored key is not in AxlSsh's format";
 *   - "the stored key's format version is not supported";
 *   - "the stored key is not an Ed25519 key";
 *   - "cannot read the variable".
 *
 * @return #AXL_OK with @a out filled; #AXL_NOT_FOUND when no such variable
 *     exists -- the ONLY result meaning "absent"; #AXL_ERR when it exists and
 *     is unusable; #AXL_IO_ERROR when the firmware failed to read it, which
 *     says nothing about whether a key is there, so a caller must not treat
 *     it as absent; #AXL_INVALID on a NULL argument; #AXL_NO_RESOURCES out
 *     of memory. On failure @a out is zeroed.
 */
AXL_WARN_UNUSED int
axl_ssh_host_key_nv_read(
    AxlSshHostKeyRecord *out,  ///< [out] the key
    const char         **why   ///< [out] reason on failure, or NULL
);

/**
 * @brief Store a key in the default variable, if none is stored. The
 *        provisioning act: a key read from a file goes here, so the file
 *        can leave the medium. Provider-free.
 *
 * Writes #AXL_SSH_HOST_KEY_VAR non-volatile and boot-services only, then
 * reads it back and compares. The stored value is a private format: a
 * 4-byte magic and version, the PKCS#8 and the public key. A write that
 * does not read back identically is deleted again (best effort), so the
 * next start does not load a key no client saw. Like
 * axl_ssh_host_key_nv_read(), this cannot tell whether the public key
 * belongs to the private key; axl_ssh_host_key_load() checks that when the
 * key is next loaded.
 *
 * Refuses to replace a stored key: delete it first
 * (axl_ssh_host_key_nv_delete()), which is the act that says "I mean to
 * change this machine's identity".
 *
 * Reasons on failure: "a host key is already stored", "cannot read the
 * variable", "cannot write the variable", "the variable did not read back",
 * and "the key is not an Ed25519 key".
 *
 * @return #AXL_OK; #AXL_EXISTS when a variable of that name is already
 *     there, whatever its attributes; #AXL_IO_ERROR when the store could
 *     not be read, written or read back; #AXL_INVALID on a NULL argument
 *     or a record whose private key is not an Ed25519 PKCS#8;
 *     #AXL_NO_RESOURCES out of memory.
 */
AXL_WARN_UNUSED int
axl_ssh_host_key_nv_write(
    const AxlSshHostKeyRecord *rec,  ///< the key to store
    const char               **why   ///< [out] reason on failure, or NULL
);

/**
 * @brief Delete the stored host key: the explicit act that replaces one.
 *        The next axl_ssh_host_key_nv_load_or_create() makes a new key.
 *        Provider-free.
 *
 * Deletes whatever variable is there, whatever its attributes -- except
 * one the firmware will not let an unsigned request delete: an
 * authenticated variable (only its signer can) or one it write-protects.
 * Recovering from that takes a HostKey file in its place, or clearing
 * NVRAM in firmware setup.
 *
 * @return #AXL_OK when deleted or already absent; #AXL_DENIED when the
 *     firmware refuses the delete as above; #AXL_IO_ERROR when it fails
 *     otherwise.
 */
AXL_WARN_UNUSED int
axl_ssh_host_key_nv_delete(
    void
);

/**
 * @brief Zero a record, private half included, in a way the compiler
 *        cannot elide. NULL-safe. Provider-free.
 */
void
axl_ssh_host_key_record_clear(
    AxlSshHostKeyRecord *rec  ///< record to clear, or NULL
);

/**
 * @brief Load a record as a signing key, checking its public key is the one
 *        its private key derives. Provider-bound.
 *
 * Why "the private key does not match its public key" is checked here: a
 * flipped bit in the private half otherwise loads cleanly as a DIFFERENT
 * key -- a silent host-key change. It protects against corruption, not
 * against a writer who stores a consistent record; nothing pre-boot can.
 *
 * Reasons on failure: "the private key does not match its public key" and
 * "the private key is not a usable Ed25519 key".
 *
 * @return #AXL_OK with @a *key set (free with axl_pk_key_free()); #AXL_ERR on
 *     a mismatch or a PKCS#8 the provider refuses; #AXL_NO_RESOURCES out of
 *     memory; #AXL_INVALID on a NULL argument. @a *key is NULL on failure.
 */
AXL_WARN_UNUSED int
axl_ssh_host_key_load(
    const AxlSshHostKeyRecord *rec,  ///< from parse, read_file or nv_read
    AxlPkKey                 **key,  ///< [out] the key
    const char               **why   ///< [out] reason on failure, or NULL
);

/**
 * @brief Fill a record from a key, for an application that keeps its host
 *        key in a store of its own. Provider-bound.
 *
 * The inverse of axl_ssh_host_key_load(): generate with
 * axl_pk_key_new(#AXL_PK_ED25519), fill a record from it, store the record's
 * two fields however the application stores secrets, and later rebuild the
 * record and load it. Clear the record when done.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL argument, a key that is not
 *     #AXL_PK_ED25519, or one with no private half (@a out zeroed).
 */
AXL_WARN_UNUSED int
axl_ssh_host_key_record_from_key(
    const AxlPkKey      *key,  ///< an Ed25519 private key
    AxlSshHostKeyRecord *out   ///< [out] the key as a record
);

/**
 * @brief Load the stored host key, or make and store one if none exists.
 *        Provider-bound.
 *
 * Absent (#AXL_NOT_FOUND from axl_ssh_host_key_nv_read(), and nothing else):
 * a new key is generated and stored with axl_ssh_host_key_nv_write(). It is
 * still returned, as #AXL_SSH_HOST_KEY_EPHEMERAL with @a *why naming the
 * reason, when it is not stored:
 *   - the write or the read-back failed (axl_ssh_host_key_nv_write()'s
 *     reasons) -- serving beats refusing; telling the operator is the
 *     caller's job;
 *   - "weak entropy; the key is not saved": the random generator was ever
 *     seeded from a clock in this image (axl_crypto_set_weak_entropy_allowed()
 *     permits that on a platform with no RNG), so the key is guessable by
 *     anyone who can bound when the machine booted. Stored, it would outlive
 *     the boot -- and every later boot, even once an RNG appears -- which is
 *     the one thing that setting's own contract forbids.
 *
 * Present but unusable, or unreadable: refused, NEVER regenerated.
 *
 * @return #AXL_OK with @a *key and @a *origin set; #AXL_ERR when a stored key
 *     exists and is unusable (axl_ssh_host_key_nv_read()'s and
 *     axl_ssh_host_key_load()'s reasons); #AXL_IO_ERROR when the store could
 *     not be read; #AXL_NO_RESOURCES when a key could not be generated (no
 *     entropy, see axl_pk_key_new(); why "cannot generate a key") or out of
 *     memory; #AXL_INVALID on a NULL argument. @a *key is NULL on failure.
 */
AXL_WARN_UNUSED int
axl_ssh_host_key_nv_load_or_create(
    AxlPkKey           **key,     ///< [out] the key (free with axl_pk_key_free)
    AxlSshHostKeyOrigin *origin,  ///< [out] loaded, generated or ephemeral
    const char         **why      ///< [out] reason on failure or EPHEMERAL, or NULL
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SSH_HOST_KEY_H */
