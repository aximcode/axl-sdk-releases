/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-chan-codec.c
    RFC 4254 channel message codec (see axl-ssh-internal.h).

    Pure byte-to-struct and struct-to-byte: no connection state and no socket,
    so the unit test drives every function directly. The readers consume bytes
    a peer chose, so each one checks its message type, every length, and --
    except where a message's tail is type-specific data handed back unread
    (axl_ssh_chan_open_parse's and axl_ssh_chan_req_parse's `rest`, checked
    per type after them) -- that nothing is left over: a
    trailing byte means the peer and this parser disagree about the message
    layout, and carrying on would parse the next message at a bogus
    offset.
**/

#include "axl-ssh-internal.h"
#include <axl/axl-mem.h>

/* ---- shared readers ------------------------------------------------- */

/* Read a string that sshd would read with sshbuf_get_cstring: a NUL inside is
   a protocol error, but ONE NUL as the very last byte is tolerated and not
   counted. Matching that exactly keeps a client that works against sshd
   working here. */
static int
get_cstring(const uint8_t *p, size_t len, size_t *off, const uint8_t **out,
            uint32_t *out_len)
{
    const uint8_t *s = NULL;
    uint32_t       n = 0;

    if (axl_ssh_get_string(p, len, off, &s, &n) != AXL_OK) {
        return AXL_ERR;
    }
    if (n > 0 && s[n - 1] == 0) {
        n--;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] == 0) {
            return AXL_ERR;
        }
    }
    *out     = s;
    *out_len = n;
    return AXL_OK;
}

/* The message must be exactly consumed. */
static int
at_end(size_t len, size_t off)
{
    return off == len ? AXL_OK : AXL_ERR;
}

/* Begin a whole-message parse: check the type byte and step past it. */
static int
begin(const uint8_t *p, size_t len, uint8_t type, size_t *off)
{
    if (p == NULL || len < 1 || p[0] != type) {
        return AXL_ERR;
    }
    *off = 1;
    return AXL_OK;
}

/* Read @a n consecutive uint32 values. */
static int
get_u32s(const uint8_t *p, size_t len, size_t *off, uint32_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (axl_ssh_get_u32(p, len, off, &out[i]) != AXL_OK) {
            return AXL_ERR;
        }
    }
    return AXL_OK;
}

/* ---- parsers --------------------------------------------------------- */

int
axl_ssh_chan_open_parse(const uint8_t *p, size_t len, AxlSshChanOpen *out)
{
    size_t   off = 0;
    uint32_t v[3];

    if (out == NULL || begin(p, len, AXL_SSH_MSG_CHANNEL_OPEN, &off) != AXL_OK
        || axl_ssh_get_string(p, len, &off, &out->type, &out->type_len) != AXL_OK
        || get_u32s(p, len, &off, v, 3) != AXL_OK) {
        return AXL_ERR;
    }
    /* No end-of-message check: what follows is the type's own data (see
       AxlSshChanOpen), judged by whoever accepts that type. */
    out->sender     = v[0];
    out->window     = v[1];
    out->max_packet = v[2];
    out->rest       = p + off;
    out->rest_len   = len - off;
    return AXL_OK;
}

int
axl_ssh_chan_req_parse(const uint8_t *p, size_t len, AxlSshChanReq *out)
{
    size_t off = 0;

    if (out == NULL || begin(p, len, AXL_SSH_MSG_CHANNEL_REQUEST, &off) != AXL_OK
        || axl_ssh_get_u32(p, len, &off, &out->recipient) != AXL_OK
        || axl_ssh_get_string(p, len, &off, &out->name, &out->name_len) != AXL_OK
        || off >= len) {
        return AXL_ERR;
    }
    out->want_reply = p[off++] != 0;
    out->rest       = p + off;
    out->rest_len   = len - off;
    return AXL_OK;
}

int
axl_ssh_chan_pty_parse(const uint8_t *rest, size_t len, char *term,
                       size_t term_cap, uint32_t dims[4])
{
    size_t         off = 0;
    const uint8_t *t   = NULL;
    uint32_t       tn  = 0;
    const uint8_t *modes = NULL;
    uint32_t       mn  = 0;
    uint32_t       d[4];

    if (rest == NULL || term == NULL || term_cap == 0 || dims == NULL
        || get_cstring(rest, len, &off, &t, &tn) != AXL_OK
        || tn >= term_cap
        || get_u32s(rest, len, &off, d, 4) != AXL_OK
        || axl_ssh_get_string(rest, len, &off, &modes, &mn) != AXL_OK
        || at_end(len, off) != AXL_OK) {
        return AXL_ERR;
    }
    axl_memcpy(term, t, tn);
    term[tn] = '\0';
    axl_memcpy(dims, d, sizeof d);
    return AXL_OK;
}

int
axl_ssh_chan_winch_parse(const uint8_t *rest, size_t len, uint32_t dims[4])
{
    size_t   off = 0;
    uint32_t d[4];

    if (rest == NULL || dims == NULL
        || get_u32s(rest, len, &off, d, 4) != AXL_OK
        || at_end(len, off) != AXL_OK) {
        return AXL_ERR;
    }
    axl_memcpy(dims, d, sizeof d);
    return AXL_OK;
}

int
axl_ssh_chan_exec_parse(const uint8_t *rest, size_t len, const uint8_t **cmd,
                        uint32_t *cmd_len)
{
    size_t off = 0;

    if (rest == NULL || cmd == NULL || cmd_len == NULL
        || get_cstring(rest, len, &off, cmd, cmd_len) != AXL_OK
        || at_end(len, off) != AXL_OK) {
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_ssh_chan_u32_msg_parse(const uint8_t *p, size_t len, uint32_t *recipient)
{
    size_t off = 0;

    if (p == NULL || recipient == NULL || len < 1
        || (p[0] != AXL_SSH_MSG_CHANNEL_EOF && p[0] != AXL_SSH_MSG_CHANNEL_CLOSE)
        || begin(p, len, p[0], &off) != AXL_OK
        || axl_ssh_get_u32(p, len, &off, recipient) != AXL_OK
        || at_end(len, off) != AXL_OK) {
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_ssh_chan_adjust_parse(const uint8_t *p, size_t len, uint32_t *recipient,
                          uint32_t *bytes)
{
    size_t off = 0;

    if (recipient == NULL || bytes == NULL
        || begin(p, len, AXL_SSH_MSG_CHANNEL_WINDOW_ADJUST, &off) != AXL_OK
        || axl_ssh_get_u32(p, len, &off, recipient) != AXL_OK
        || axl_ssh_get_u32(p, len, &off, bytes) != AXL_OK
        || at_end(len, off) != AXL_OK) {
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_ssh_chan_data_parse(const uint8_t *p, size_t len, uint32_t *recipient,
                        const uint8_t **data, uint32_t *data_len)
{
    size_t off = 0;

    if (recipient == NULL || data == NULL || data_len == NULL
        || begin(p, len, AXL_SSH_MSG_CHANNEL_DATA, &off) != AXL_OK
        || axl_ssh_get_u32(p, len, &off, recipient) != AXL_OK
        || axl_ssh_get_string(p, len, &off, data, data_len) != AXL_OK
        || at_end(len, off) != AXL_OK) {
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_ssh_chan_ext_data_parse(const uint8_t *p, size_t len, uint32_t *recipient,
                            uint32_t *code, const uint8_t **data,
                            uint32_t *data_len)
{
    size_t off = 0;

    if (recipient == NULL || code == NULL || data == NULL || data_len == NULL
        || begin(p, len, AXL_SSH_MSG_CHANNEL_EXTENDED_DATA, &off) != AXL_OK
        || axl_ssh_get_u32(p, len, &off, recipient) != AXL_OK
        || axl_ssh_get_u32(p, len, &off, code) != AXL_OK
        || axl_ssh_get_string(p, len, &off, data, data_len) != AXL_OK
        || at_end(len, off) != AXL_OK) {
        return AXL_ERR;
    }
    return AXL_OK;
}

/* ---- builders --------------------------------------------------------- */

/* The message type byte followed by the recipient channel, which every
   builder starts with. */
static int
put_head(AxlString *b, uint8_t type, uint32_t recipient)
{
    char t = (char)type;

    if (b == NULL) {
        return AXL_ERR;
    }
    return axl_string_append_len(b, &t, 1) == AXL_OK
                   && axl_ssh_put_u32(b, recipient) == AXL_OK
               ? AXL_OK : AXL_ERR;
}

int
axl_ssh_chan_open_confirm_build(AxlString *b, uint32_t recipient,
                                uint32_t sender, uint32_t window,
                                uint32_t max_packet)
{
    return put_head(b, AXL_SSH_MSG_CHANNEL_OPEN_CONFIRMATION, recipient) == AXL_OK
                   && axl_ssh_put_u32(b, sender) == AXL_OK
                   && axl_ssh_put_u32(b, window) == AXL_OK
                   && axl_ssh_put_u32(b, max_packet) == AXL_OK
               ? AXL_OK : AXL_ERR;
}

int
axl_ssh_chan_open_failure_build(AxlString *b, uint32_t recipient,
                                uint32_t reason)
{
    return put_head(b, AXL_SSH_MSG_CHANNEL_OPEN_FAILURE, recipient) == AXL_OK
                   && axl_ssh_put_u32(b, reason) == AXL_OK
                   && axl_ssh_put_string(b, "", 0) == AXL_OK    /* description */
                   && axl_ssh_put_string(b, "", 0) == AXL_OK    /* language */
               ? AXL_OK : AXL_ERR;
}

int
axl_ssh_chan_data_build(AxlString *b, uint32_t recipient, bool stderr_,
                        const void *d, size_t n)
{
    if (d == NULL && n != 0) {
        return AXL_ERR;
    }
    if (put_head(b, stderr_ ? AXL_SSH_MSG_CHANNEL_EXTENDED_DATA
                            : AXL_SSH_MSG_CHANNEL_DATA, recipient) != AXL_OK
        || (stderr_ && axl_ssh_put_u32(b, AXL_SSH_EXTENDED_DATA_STDERR) != AXL_OK)) {
        return AXL_ERR;
    }
    return axl_ssh_put_string(b, d != NULL ? d : "", n);
}

int
axl_ssh_chan_adjust_build(AxlString *b, uint32_t recipient, uint32_t bytes)
{
    return put_head(b, AXL_SSH_MSG_CHANNEL_WINDOW_ADJUST, recipient) == AXL_OK
                   && axl_ssh_put_u32(b, bytes) == AXL_OK
               ? AXL_OK : AXL_ERR;
}

int
axl_ssh_chan_u32_msg_build(AxlString *b, uint8_t type, uint32_t recipient)
{
    if (type != AXL_SSH_MSG_CHANNEL_EOF && type != AXL_SSH_MSG_CHANNEL_CLOSE
        && type != AXL_SSH_MSG_CHANNEL_SUCCESS
        && type != AXL_SSH_MSG_CHANNEL_FAILURE) {
        return AXL_ERR;
    }
    return put_head(b, type, recipient);
}

int
axl_ssh_chan_exit_status_build(AxlString *b, uint32_t recipient, uint32_t status)
{
    static const char NAME[] = "exit-status";
    char              no_reply = 0;

    return put_head(b, AXL_SSH_MSG_CHANNEL_REQUEST, recipient) == AXL_OK
                   && axl_ssh_put_string(b, NAME, sizeof NAME - 1) == AXL_OK
                   && axl_string_append_len(b, &no_reply, 1) == AXL_OK
                   && axl_ssh_put_u32(b, status) == AXL_OK
               ? AXL_OK : AXL_ERR;
}

int
axl_ssh_keepalive_build(AxlString *b, bool on_channel, uint32_t recipient)
{
    static const char NAME[] = "keepalive@openssh.com";
    char              want   = 1;

    if (on_channel) {
        if (put_head(b, AXL_SSH_MSG_CHANNEL_REQUEST, recipient) != AXL_OK) {
            return AXL_ERR;
        }
    } else {
        char hdr = (char)AXL_SSH_MSG_GLOBAL_REQUEST;
        if (b == NULL || axl_string_append_len(b, &hdr, 1) != AXL_OK) {
            return AXL_ERR;
        }
    }
    return axl_ssh_put_string(b, NAME, sizeof NAME - 1) == AXL_OK
                   && axl_string_append_len(b, &want, 1) == AXL_OK
               ? AXL_OK : AXL_ERR;
}
