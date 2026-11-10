/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * agent.c -- run whole Shell command lines sent over HTTP, and answer with
 * whether each ran, its status and its output. Nothing is typed: a test
 * harness, the guest pool (docs/AXL-Test-Guest-Pool-Design.md §5) or a
 * script on a lab machine drives a UEFI Shell the way `ssh host cmd` drives
 * a POSIX one.
 *
 * Usage: agent [--port N] [--out DIR] [--latency firmware|window|always]
 *
 *   --port N      TCP port to listen on (default 8080)
 *   --out DIR     a WRITABLE directory for the capture files (default: the
 *                 directory the agent was started from, from argv[0]; the
 *                 current directory when argv[0] names none). Made
 *                 absolute at start (a `\dir` on the current volume;
 *                 `fs0:` is that volume's root) -- lines may `cd` anywhere -- and
 *                 proved writable by creating and removing a probe file.
 *                 Refused at start, exit status 2, when it cannot be: no
 *                 current directory to resolve a relative one against, a
 *                 character the Shell would parse (space, tab, `"`, `%`,
 *                 `#`, `^`, `|`, `<`, `>`: the path is inserted into
 *                 command lines), too long, or not writable.
 *   --latency M   axl_net_set_latency mode (default always: ~1 ms round
 *                 trips for 1 ms of firmware tick -- which makes OVMF's USB
 *                 keyboard drop keys, see axl-net.h; pass firmware on a
 *                 machine someone types on)
 *
 * When the Shell has no current directory -- at start, or after a line
 * unmapped its volume -- the agent changes to the capture directory before
 * the next line: the Shell's status reset needs one, and without it every
 * outcome would be `unknown`.
 *
 * Runs in the Shell's foreground until the machine is reset or powered off,
 * and prints `AXL-AGENT-READY port=<n> nonest=<0|1>` once it is listening.
 * Every command runs through the hosting Shell (axl_shell_execute_status),
 * one at a time, in the agent's own TPL_APPLICATION context; a request that
 * arrives while one runs waits for it, and so does `/ping`.
 *
 * Only a Shell started `-nonest` can say whether a line ran: under the
 * default nested Shell every outcome is `unknown` (axl-shell.h states why).
 *
 * ---------------------------------------------------------------------------
 * The protocol (HTTP/1.1, keep-alive)
 * ---------------------------------------------------------------------------
 *
 * GET /ping  -> 200 text/plain `pong\n`
 *
 * POST /quit -> 200 text/plain `bye\n`, then the agent stops serving,
 *   frees everything and returns 0 to the Shell (a DEBUG build prints its
 *   leak verdict as it does).
 *
 * GET /info  -> 200 application/json, one object:
 *     {"agent":2,"version":"<axl version>","arch":"X64"|"AARCH64",
 *      "nonest":true|false,"tick_us":<n>,"shell_exiting":true|false,
 *      "port":<n>}
 *   `agent` is the protocol version: a change a client must know about
 *   bumps it. `nonest` and `tick_us` are read when asked (`set nonesting`
 *   changes the one, another image's timer request the other).
 *
 * POST /exec[?output=console], body = ONE command line, UTF-8, 1..4000
 * bytes, no CR, LF or NUL -> 200 application/json:
 *     {"outcome":"ran"|"not-run"|"unknown","status":"0x<HEX>",
 *      "execute":"0x<HEX>","shell_exiting":true|false,
 *      "canary":"ok"|"poisoned"|"inconclusive"|"skipped",
 *      "reset_ok":true|false,"ms":<n>,
 *      "stdout":<string>|null,"stdout_state":<state>,
 *      "stderr":<string>|null,"stderr_state":<state>}
 *   outcome, status AxlShellResult.outcome / .status
 *   execute         AxlShellResult.execute_status (Execute's own return)
 *   shell_exiting   an `exit` has run in the agent's Shell -- this line or
 *                   an earlier one (see "exit" below); sticky
 *   canary          what the check after this line found: `poisoned` (it
 *                   sets shell_exiting), `ok`, `inconclusive` (it could not
 *                   tell -- reported, never sticky), or `skipped` (the
 *                   agent's Shell nests, so there is nothing to poison)
 *   reset_ok        AxlShellResult.reset_ok (why an outcome is unknown)
 *   ms              the line's wall time in the guest, capture included
 *   stdout, stderr  what the line wrote to each, captured by inserting
 *                   ` > <file>` / ` 2> <file>` (UCS-2 redirects, converted
 *                   back to UTF-8 with the Shell's byte-order mark removed;
 *                   line ends stay as the Shell wrote them, CRLF)
 *   state           `captured` -- the string is all of it (empty when the
 *                     line wrote nothing, or did not run);
 *                   `console` -- null: ?output=console, the line wrote to
 *                     the console (the guest's serial log), not captured;
 *                   `truncated` -- the first 1 MiB of it, cut at a character
 *                     boundary;
 *                   `redirected` -- null: the line redirects that stream
 *                     itself, and a second redirect of one stream makes the
 *                     Shell refuse the whole line;
 *                   `unreadable` -- null: the capture file exists and could
 *                     not be read, or memory ran out converting it;
 *                   `lost` -- null: the line ran, or may have, but the
 *                     Shell never created the capture file. Measured under
 *                     a nesting Shell, where every outcome is `unknown`: a
 *                     line it refused before setting up any redirect (an
 *                     unknown command) -- there it means "no capture", not
 *                     "output went elsewhere". No `-nonest` line is known
 *                     to produce it (a `%var%` holding `#` does not: the
 *                     Shell strips comments before expanding variables).
 *   The UCS-2 capture is converted by its length, so a U+0000 the line
 *   wrote is kept (JSON `\u0000`); an odd final byte is dropped and the
 *   state is `truncated`.
 *
 * Where the capture goes. The Shell parses the line before running it:
 * `^` escapes the next character, `"` quotes, `#` starts a comment -- even
 * inside quotes; only `^#` is a literal `#` -- and an unquoted `|` (or
 * `|a`) a pipe. The captures go at the end
 * of the LAST pipe segment, before any comment -- so they capture that
 * segment's stdout and stderr; an earlier segment's stderr reaches the
 * console. A stream counts as redirected when a word of that segment
 * outside quotes is one of the Shell's operators for it (`>`, `>>`, `1>`,
 * `1>>` for stdout, `2>`, `2>>` for stderr, each optionally followed by `a`
 * or `v`); a redirect the scan missed would make the Shell see two and
 * refuse the line (`not-run`), never return other output. The capture
 * files are named per agent start (a random tag) and per line (a counter),
 * so no two lines and no two agents sharing a directory can ever read each
 * other's; each is deleted before the line runs and after it is read.
 * The capture directory is FOLLOWED when its volume's name moves (removing
 * a hot-plugged USB stick renumbers fsN:; `map -d` unmaps one): a marker
 * file, `agent-<tag>.home`, is looked for again on fs0:..fs63: under the
 * same path before each line, and a 500 says so when it is nowhere.
 *
 *   400 text/plain `agent: <reason>\n` -- an empty or blank line, over 4000
 *       bytes, a CR, LF or NUL in it, invalid UTF-8, an unclosed `"`, or a
 *       final `^` escaping nothing (either would swallow the capture).
 *       Nothing ran.
 *   403 text/plain `agent: refused: exit would end the Shell the agent runs
 *       in\n` -- `exit` as the line's first word, in any spelling the Shell
 *       accepts, or an alias for it (axl_shell_execute_status refuses it
 *       with AXL_DENIED). Nothing ran.
 *   409 text/plain `agent: the Shell is exiting (an exit ran): restore or
 *       reset the machine\n` -- after `shell_exiting`. Nothing ran.
 *   500 text/plain `agent: <reason>\n` -- the capture directory cannot be
 *       written to any more (nothing ran), or the Shell could not be asked
 *       (no Shell hosts the agent, out of memory).
 *
 * What the agent cannot protect against, stated rather than pretended:
 *   - Shell state PERSISTS between lines, as at a prompt: `cd`, `set`,
 *     `alias` and `map` carry over. `set nonesting False` makes every later
 *     outcome `unknown`; an alias named `cd` stops the status reset
 *     (`reset_ok` false).
 *   - `exit` anywhere but the first word -- inside a script the line runs,
 *     as a later pipe segment, or as a `%var%` that expands to it --
 *     POISONS the agent's Shell (measured, both arches): that line reports
 *     RAN 0 as usual, every LATER script stops after its first line still
 *     reporting RAN 0, and the Shell exits (to the firmware's setup menu)
 *     when the agent returns. It cannot be undone, so the agent DETECTS it:
 *     after every line in a `-nonest` Shell it runs a two-line canary
 *     script from the capture directory that sets a variable to 1, then 2,
 *     and reads it through the environment API -- 1 means only the first
 *     line ran. That sets `shell_exiting` on that line's answer and a 409
 *     on every line after; a pool restores the snapshot. No alias, lost
 *     directory or status reset can fake it, and an answer the canary
 *     cannot give is `inconclusive`, never sticky.
 *     (`exit /b` ends only its script and poisons nothing.)
 *   - `reset` is not refused: it resets the machine, the request gets no
 *     response and the connection drops -- a failure, never a status. A pool
 *     resets its guests over QMP.
 *   - A line that waits for a key (`pause`, `edit`) or loops waits forever,
 *     and the agent with it. The CLIENT owns the timeout: on expiry it
 *     replaces the guest (a pool) or resets the machine.
 *   - A line ending in an empty pipe segment (`echo hi |`) gets the
 *     captures as that whole segment and reads `not-run` (under `-nonest`;
 *     `unknown` and `lost` under a nesting Shell).
 *   - A capture file is read whole before the 1 MiB cut: an enormous output
 *     costs guest memory and capture-volume space while it is read.
 *   - POST is not idempotent: a client that retries after a timeout may run
 *     a line twice. After a snapshot restore or reset, reconnect.
 *
 * Any other method or path is the HTTP server's 404/405.
 */

#include <axl.h>

#define AGENT_PROTOCOL    2   /* 2: POST /exec?output=console */
#define AGENT_PORT        8080
#define AGENT_LINE_MAX    4000u
#define AGENT_STREAM_MAX  (1024u * 1024u)
#define AGENT_PATH_MAX    512u

typedef struct {
    size_t   port;
    char     out_dir[AGENT_PATH_MAX];
    char     tag[17];          /* per start: names the capture files */
    uint64_t counter;          /* per line */
    AxlLoop *loop;             /* the serving loop: POST /quit ends it */
    char     canary[AGENT_PATH_MAX + 48];   /* the canary script (see canary_check) */
    bool     shell_exiting;    /* sticky: an `exit` ran, the canary said so */
    AxlNetLatency latency;     /* the agent's own policy (--latency), between lines */
} Agent;

/* One captured stream, as the response reports it. */
typedef struct {
    const char *state;         /* captured, truncated, redirected, unreadable, lost */
    char       *text;          /* owned; NULL unless captured or truncated */
    size_t      len;           /* bytes of text (it may hold a NUL) */
} Capture;

/* What the scan of a line found (see the header's "Where the capture goes"). */
typedef struct {
    size_t insert;            /* where the captures go: the comment, or the end */
    bool   stdout_redirected;
    bool   stderr_redirected;
    bool   blank_before;      /* nothing but blanks before `insert` */
    const char *error;        /* a 400 reason, or NULL */
} LineScan;

static Agent g_agent;

static bool
is_blank(
    char c
    )
{
    return c == ' ' || c == '\t';
}

/* Is the word [w, w+n) one of the Shell's redirect operators? Sets *fd to
   1 (stdout) or 2 (stderr); `<` forms are not output and are ignored. */
static bool
redirect_word(
    const char *w,
    size_t      n,
    int        *fd
    )
{
    size_t i = 0;
    *fd = 1;
    if (i < n && (w[i] == '1' || w[i] == '2')) {
        *fd = w[i] - '0';
        i++;
    }
    if (i >= n || w[i] != '>') {
        return false;
    }
    i++;
    if (i < n && w[i] == '>') {
        i++;
    }
    if (i < n && (w[i] == 'a' || w[i] == 'A' || w[i] == 'v' || w[i] == 'V')) {
        i++;
    }
    return i == n;
}

/* Scan @p line the way the Shell will parse it (header comment). */
static LineScan
scan_line(
    const char *line,
    size_t      len
    )
{
    LineScan s = { .insert = len };
    size_t   seg = 0;          /* start of the last pipe segment */
    bool     quoted = false;
    size_t   i = 0;
    while (i < len) {
        char c = line[i];
        if (c == '^') {
            if (i + 1 >= len) {
                s.error = "a final ^ escapes nothing";
                return s;
            }
            i += 2;
            continue;
        }
        if (c == '#') {
            s.insert = i;   /* the Shell strips from here, quoted or not */
            break;
        }
        if (c == '"') {
            quoted = !quoted;
        } else if (!quoted && c == '|') {
            seg = i + 1;
            if (seg < len && (line[seg] == 'a' || line[seg] == 'A')
                && (seg + 1 >= len || is_blank(line[seg + 1]))) {
                seg++;
            }
        }
        i++;
    }
    if (quoted) {
        s.error = "an unclosed quote";
        return s;
    }
    /* The words of the last segment, up to the comment. A word holding a
       quote or an escape is never an operator. */
    s.blank_before = true;
    for (size_t k = 0; k < s.insert; k++) {
        if (!is_blank(line[k])) {
            s.blank_before = false;
            break;
        }
    }
    size_t k = seg;
    while (k < s.insert) {
        while (k < s.insert && is_blank(line[k])) {
            k++;
        }
        size_t start = k;
        bool   plain = true;
        bool   in_q  = false;
        while (k < s.insert && (in_q || !is_blank(line[k]))) {
            if (line[k] == '"') {
                in_q  = !in_q;
                plain = false;
            } else if (line[k] == '^') {
                plain = false;
                k++;
            }
            k++;
        }
        int fd = 0;
        if (k > start && plain && redirect_word(line + start, k - start, &fd)) {
            if (fd == 2) {
                s.stderr_redirected = true;
            } else {
                s.stdout_redirected = true;
            }
        }
    }
    return s;
}


/* Append one code point as UTF-8 if it fits in the cap; false if not. */
static bool
put_utf8(
    char    *out,
    size_t  *len,
    uint32_t cp
    )
{
    char   b[4];
    size_t n;
    if (cp < 0x80u) {
        b[0] = (char)cp;
        n    = 1;
    } else if (cp < 0x800u) {
        b[0] = (char)(0xC0u | (cp >> 6));
        b[1] = (char)(0x80u | (cp & 0x3Fu));
        n    = 2;
    } else if (cp < 0x10000u) {
        b[0] = (char)(0xE0u | (cp >> 12));
        b[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[2] = (char)(0x80u | (cp & 0x3Fu));
        n    = 3;
    } else {
        b[0] = (char)(0xF0u | (cp >> 18));
        b[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        b[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[3] = (char)(0x80u | (cp & 0x3Fu));
        n    = 4;
    }
    if (*len + n > AGENT_STREAM_MAX) {
        return false;   /* whole characters only */
    }
    axl_memcpy(out + *len, b, n);
    *len += n;
    return true;
}

/* UCS-2LE bytes (BOM already skipped) to UTF-8 by LENGTH -- a U+0000 is
   kept -- capped at AGENT_STREAM_MAX. An unpaired surrogate becomes
   U+FFFD. Sets c->state to captured or truncated, or unreadable on OOM. */
static void
convert_capture(
    const unsigned char *b,
    size_t               n,
    Capture             *c
    )
{
    size_t units = n / 2u;
    size_t room  = units * 3u < AGENT_STREAM_MAX ? units * 3u : AGENT_STREAM_MAX;
    c->text = axl_malloc(room + 1u);
    if (c->text == NULL) {
        c->state = "unreadable";
        return;
    }
    c->len   = 0;
    c->state = (n % 2u != 0) ? "truncated" : "captured";
    for (size_t i = 0; i < units; i++) {
        uint32_t u = (uint32_t)b[2u * i] | ((uint32_t)b[2u * i + 1u] << 8);
        if (u >= 0xD800u && u <= 0xDBFFu && i + 1 < units) {
            uint32_t lo = (uint32_t)b[2u * i + 2u] | ((uint32_t)b[2u * i + 3u] << 8);
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                u = 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
                i++;
            } else {
                u = 0xFFFDu;
            }
        } else if (u >= 0xD800u && u <= 0xDFFFu) {
            u = 0xFFFDu;
        }
        if (!put_utf8(c->text, &c->len, u)) {
            c->state = "truncated";
            break;
        }
    }
    c->text[c->len] = '\0';
}

/* Read, convert and remove one capture file. A file that does not exist
   is the empty capture when the line did not run, and `lost` when it may
   have: the Shell creates the file as it honours the redirect. */
static void
read_capture(
    const char *path,
    bool        may_have_run,
    Capture    *c
    )
{
    void  *raw = NULL;
    size_t n   = 0;
    c->text = NULL;
    c->len  = 0;
    if (axl_file_get_contents(path, &raw, &n) != AXL_OK) {
        if (axl_file_delete(path) != AXL_NOT_FOUND) {
            c->state = "unreadable";
        } else if (may_have_run) {
            c->state = "lost";
        } else {
            c->text  = axl_strdup("");
            c->state = c->text != NULL ? "captured" : "unreadable";
        }
        return;
    }
    const unsigned char *b = raw;
    size_t off = (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) ? 2u : 0u;
    convert_capture(b + off, n - off, c);
    axl_free(raw);
    axl_file_delete(path);
}

/* The empty capture, for a stream no redirect was placed for. */
static void
empty_capture(
    Capture *c
    )
{
    c->text  = axl_strdup("");
    c->len   = 0;
    c->state = c->text != NULL ? "captured" : "unreadable";
}

static void
put_stream(
    AxlJsonWriter *w,
    const char    *key,
    const char    *state_key,
    const Capture *c
    )
{
    axl_json_key(w, key);
    if (c->text != NULL) {
        axl_json_strn(w, c->text, c->len);
    } else {
        axl_json_null(w);
    }
    axl_json_key(w, state_key);
    axl_json_str(w, c->state);
}

static int
reply_text(
    AxlHttpResponse *resp,
    size_t           code,
    const char      *text
    )
{
    axl_http_response_set_status(resp, code);
    axl_http_response_set_text(resp, text);
    return AXL_OK;
}

static int
reply_json(
    AxlHttpResponse *resp,
    AxlString       *s,
    bool             ok
    )
{
    if (!ok) {
        return reply_text(resp, 500, "agent: out of memory\n");
    }
    axl_http_response_set_json(resp, axl_string_str(s));
    return AXL_OK;
}

static bool
nonest_now(void)
{
    char *v  = axl_getenv("nonesting");
    bool  on = v != NULL && axl_strcasecmp(v, "True") == 0;
    axl_free(v);
    return on;
}

static int
on_ping(
    AxlHttpRequest  *req,
    AxlHttpResponse *resp,
    void            *data
    )
{
    (void)req;
    (void)data;
    axl_http_response_set_text(resp, "pong\n");
    return AXL_OK;
}

static bool
quit_now(
    void *data
    )
{
    (void)data;
    axl_loop_quit(g_agent.loop);
    return false;
}

/* The reply goes out first: the loop ends on a timeout after it. */
static int
on_quit(
    AxlHttpRequest  *req,
    AxlHttpResponse *resp,
    void            *data
    )
{
    (void)req;
    (void)data;
    if (axl_loop_add_timeout(g_agent.loop, 50, quit_now, NULL) == 0) {
        return reply_text(resp, 500, "agent: cannot schedule the quit\n");
    }
    axl_http_response_set_text(resp, "bye\n");
    return AXL_OK;
}

static int
on_info(
    AxlHttpRequest  *req,
    AxlHttpResponse *resp,
    void            *data
    )
{
    (void)req;
    (void)data;
#if defined(__x86_64__)
    static const char arch[] = "X64";
#elif defined(__aarch64__)
    static const char arch[] = "AARCH64";
#else
    static const char arch[] = "unknown";
#endif
    AxlString    *s = axl_string_new(NULL);
    AxlJsonWriter w;
    axl_json_writer_init(&w, s, AXL_JSON_STRICT);
    axl_json_obj_begin(&w);
    axl_json_key(&w, "agent");
    axl_json_uint(&w, AGENT_PROTOCOL);
    axl_json_key(&w, "version");
    axl_json_str(&w, axl_version());
    axl_json_key(&w, "arch");
    axl_json_str(&w, arch);
    axl_json_key(&w, "nonest");
    axl_json_bool(&w, nonest_now());
    axl_json_key(&w, "tick_us");
    axl_json_uint(&w, axl_timer_period_us());
    axl_json_key(&w, "shell_exiting");
    axl_json_bool(&w, g_agent.shell_exiting);
    axl_json_key(&w, "port");
    axl_json_uint(&w, g_agent.port);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    int rc = reply_json(resp, s, s != NULL && !axl_json_writer_error(&w));
    axl_string_free(s);
    return rc;
}

/* The 400 reasons decided before the line is scanned. */
static const char *
check_body(
    const AxlHttpRequest *req
    )
{
    const char *b = req->body;
    size_t      n = req->body_size;
    if (b == NULL || n == 0) {
        return "an empty line";
    }
    if (n > AGENT_LINE_MAX) {
        return "a line over 4000 bytes";
    }
    bool blank = true;
    for (size_t i = 0; i < n; i++) {
        if (b[i] == '\r' || b[i] == '\n' || b[i] == '\0') {
            return "a CR, LF or NUL in the line";
        }
        blank = blank && is_blank(b[i]);
    }
    if (blank) {
        return "a blank line";
    }
    if (!axl_utf8_validate(b, n, NULL)) {
        return "invalid UTF-8";
    }
    return NULL;
}

/* The canary: a two-line script that sets a variable to 1, then to 2. An
   `exit` anywhere -- a script, a pipe segment, a variable expanding to it --
   leaves the Shell exiting (measured: every later script stops after its
   first line and reports RAN 0, and the Shell itself exits when the agent
   returns), so only the first line runs: 1 is POISONED, 2 is healthy. The
   variable is cleared and read through the environment API, never a Shell
   line or %lasterror%, so a user's alias for `cd`, a lost current directory
   or a status reset that could not run cannot make a healthy Shell look
   poisoned. Anything else (the script deleted, `alias set`) is
   INCONCLUSIVE: the script is rewritten and run once more, and an
   inconclusive answer is reported but never sticky. Only a -nonest Shell
   has the agent's Shell to poison; a line that turned nesting off is put
   back to -nonest for the canary, which must run in the agent's Shell. */
#define AGENT_CANARY_VAR "AXLAGENTCANARY"
static const char g_canary_text[] =
    "@set -v " AGENT_CANARY_VAR " 1\r\n@set -v " AGENT_CANARY_VAR " 2\r\n";

typedef enum {
    CANARY_SKIPPED,           /* the agent's Shell nests: nothing to poison */
    CANARY_OK,
    CANARY_POISONED,
    CANARY_INCONCLUSIVE
} CanaryVerdict;

static const char *const g_canary_names[] = { "skipped", "ok", "poisoned", "inconclusive" };

static bool
write_canary(void)
{
    return axl_file_set_contents(g_agent.canary, g_canary_text, sizeof(g_canary_text) - 1)
           == AXL_OK;
}

static CanaryVerdict
canary_once(void)
{
    axl_unsetenv(AGENT_CANARY_VAR);
    AxlShellResult r;
    if (axl_shell_execute_status(g_agent.canary, &r) != AXL_OK) {
        return CANARY_INCONCLUSIVE;
    }
    char *v = axl_getenv(AGENT_CANARY_VAR);
    CanaryVerdict verdict = CANARY_INCONCLUSIVE;
    if (v != NULL && axl_strcmp(v, "2") == 0) {
        verdict = CANARY_OK;
    } else if (v != NULL && axl_strcmp(v, "1") == 0) {
        verdict = CANARY_POISONED;
    }
    axl_free(v);
    axl_unsetenv(AGENT_CANARY_VAR);
    return verdict;
}

/* @p nonest_before: whether the agent's Shell was -nonest before the line. */
static CanaryVerdict
canary_check(
    bool nonest_before
    )
{
    if (!nonest_before) {
        return CANARY_SKIPPED;
    }
    bool restore = !nonest_now();
    if (restore && axl_setenv("nonesting", "True", true) != AXL_OK) {
        return CANARY_INCONCLUSIVE;
    }
    CanaryVerdict v = canary_once();
    if (v == CANARY_INCONCLUSIVE && write_canary()) {
        v = canary_once();
    }
    if (restore) {
        axl_setenv("nonesting", "False", true);   /* the line's choice stands */
    }
    return v;
}

/* The marker file that names the capture directory wherever its volume is
   mapped now: the capture paths hold a volume NAME, and names move. */
static void
home_path(
    char       *buf,
    size_t      size,
    const char *dir
    )
{
    axl_snprintf(buf, size, "%sagent-%s.home", dir, g_agent.tag);
}

/* Is the capture directory still where its name says? If not, find the
   marker on fs0:..fs63: under the same path, and move every capture path
   there. False when it is nowhere. */
static bool
follow_home(void)
{
    char       path[AGENT_PATH_MAX + 64];
    AxlFsEntry entry;
    home_path(path, sizeof(path), g_agent.out_dir);
    if (axl_file_info(path, &entry) == AXL_OK) {
        return true;
    }
    const char *colon = axl_strchr(g_agent.out_dir, ':');
    if (colon == NULL) {
        return false;
    }
    char rest[AGENT_PATH_MAX];
    axl_snprintf(rest, sizeof(rest), "%s", colon + 1);
    for (int i = 0; i < 64; i++) {
        char dir[AGENT_PATH_MAX];
        axl_snprintf(dir, sizeof(dir), "fs%d:%s", i, rest);
        home_path(path, sizeof(path), dir);
        if (axl_file_info(path, &entry) == AXL_OK) {
            axl_snprintf(g_agent.out_dir, sizeof(g_agent.out_dir), "%s", dir);
            axl_snprintf(g_agent.canary, sizeof(g_agent.canary), "%sagent-%s.canary.nsh",
                         g_agent.out_dir, g_agent.tag);
            return true;
        }
    }
    return false;
}

static int
on_exec(
    AxlHttpRequest  *req,
    AxlHttpResponse *resp,
    void            *data
    )
{
    (void)data;
    if (g_agent.shell_exiting) {
        return reply_text(resp, 409,
                          "agent: the Shell is exiting (an exit ran): restore or reset the machine\n");
    }
    if (!follow_home()) {
        return reply_text(resp, 500,
                          "agent: the capture directory's volume is no longer mapped\n");
    }
    /* A line may leave the Shell without a current directory (unmapping
       its volume does); the status reset needs one. */
    char *cwd_now = axl_get_current_dir();
    if (cwd_now == NULL) {
        axl_chdir(g_agent.out_dir);
    }
    axl_free(cwd_now);
    char msg[96];
    /* ?output=console: no captures -- the line writes to the console (the
       serial log) as it runs, so a line that crashes or hangs still leaves
       all it printed. Protocol 2. */
    bool console = false;
    if (req->query != NULL && req->query[0] != '\0') {
        if (axl_strcmp(req->query, "output=console") != 0) {
            axl_snprintf(msg, sizeof(msg), "agent: unknown query `%.40s` (only output=console)\n",
                         req->query);
            return reply_text(resp, 400, msg);
        }
        console = true;
    }
    const char *bad = check_body(req);
    LineScan    sc  = { 0 };
    if (bad == NULL) {
        sc  = scan_line(req->body, req->body_size);
        bad = sc.error;
    }
    if (bad != NULL) {
        axl_snprintf(msg, sizeof(msg), "agent: %s\n", bad);
        return reply_text(resp, 400, msg);
    }

    uint64_t id = ++g_agent.counter;
    char out_path[AGENT_PATH_MAX + 48];
    char err_path[AGENT_PATH_MAX + 48];
    axl_snprintf(out_path, sizeof(out_path), "%sagent-%s-%llu.out", g_agent.out_dir,
                 g_agent.tag, (unsigned long long)id);
    axl_snprintf(err_path, sizeof(err_path), "%sagent-%s-%llu.err", g_agent.out_dir,
                 g_agent.tag, (unsigned long long)id);
    bool cap_out = !console && !sc.stdout_redirected && !sc.blank_before;
    bool cap_err = !console && !sc.stderr_redirected && !sc.blank_before;
    /* The names are new, so a file there is not expected; one that cannot
       be removed means the directory has stopped taking writes. */
    if ((cap_out && axl_file_delete(out_path) == AXL_ERR)
        || (cap_err && axl_file_delete(err_path) == AXL_ERR)) {
        return reply_text(resp, 500, "agent: the capture directory cannot be written\n");
    }

    /* line[0, insert) + captures + " " + line[insert, end) */
    AxlString *cmd = axl_string_new(NULL);
    if (cmd == NULL) {
        return reply_text(resp, 500, "agent: out of memory\n");
    }
    /* Every piece or none: a capture lost to OOM would run the line with
       its output on the console. */
    const char *body = req->body;
    bool built = axl_string_append_len(cmd, body, sc.insert) == AXL_OK
                 && (!cap_out || axl_string_append_printf(cmd, " > %s", out_path) == AXL_OK)
                 && (!cap_err || axl_string_append_printf(cmd, " 2> %s", err_path) == AXL_OK)
                 && (sc.insert >= req->body_size
                     || (axl_string_append(cmd, " ") == AXL_OK
                         && axl_string_append_len(cmd, body + sc.insert,
                                                  req->body_size - sc.insert) == AXL_OK));
    if (!built) {
        axl_string_free(cmd);
        return reply_text(resp, 500, "agent: out of memory\n");
    }

    AxlShellResult r;
    bool     nonest_before = nonest_now();
    uint64_t t0 = axl_time_get_ms();
    /* THE LINE RUNS ON THE FIRMWARE'S TICK. The agent's own --latency (a
       1 ms tick for ~1 ms round trips) is the agent's business: a line --
       a pooled test's program -- must see the machine a booted guest gives
       it. Held through the line it cost ~0.15 cores idle and changed every
       network test's timing (pool design §6.7, step 7). Restored before the
       reply, which is the agent's own traffic again. */
    AxlNetLatency fw;
    axl_net_latency_defaults(&fw);
    if (axl_net_set_latency(&fw) != AXL_OK) {
        axl_printerr("agent: cannot drop to the firmware tick for a line\n");
    }
    int rc = axl_shell_execute_status(axl_string_str(cmd), &r);
    if (axl_net_set_latency(&g_agent.latency) != AXL_OK) {
        axl_printerr("agent: cannot restore its latency policy after a line\n");
    }
    axl_string_free(cmd);
    if (rc == AXL_DENIED) {
        return reply_text(resp, 403,
                          "agent: refused: exit would end the Shell the agent runs in\n");
    }
    if (rc != AXL_OK) {
        return reply_text(resp, 500, "agent: the Shell could not be asked\n");
    }

    bool    may_have_run = r.outcome != AXL_SHELL_OUTCOME_NOT_RUN;
    Capture out = { .state = console ? "console" : "redirected" };
    Capture err = { .state = console ? "console" : "redirected" };
    if (cap_out) {
        read_capture(out_path, may_have_run, &out);
    } else if (sc.blank_before && !console) {
        empty_capture(&out);
    }
    if (cap_err) {
        read_capture(err_path, may_have_run, &err);
    } else if (sc.blank_before && !console) {
        empty_capture(&err);
    }
    uint64_t ms = axl_time_get_ms() - t0;
    CanaryVerdict canary = canary_check(nonest_before);
    if (canary == CANARY_POISONED) {
        g_agent.shell_exiting = true;   /* sticky: nothing after this is a result */
    }

    static const char *const outcome[] = { "unknown", "ran", "not-run" };
    char hex[24];
    AxlString    *s = axl_string_new(NULL);
    AxlJsonWriter w;
    axl_json_writer_init(&w, s, AXL_JSON_STRICT);
    axl_json_obj_begin(&w);
    axl_json_key(&w, "outcome");
    axl_json_str(&w, (unsigned)r.outcome <= AXL_SHELL_OUTCOME_NOT_RUN
                     ? outcome[r.outcome] : "unknown");
    axl_snprintf(hex, sizeof(hex), "0x%llX", (unsigned long long)r.status);
    axl_json_key(&w, "status");
    axl_json_str(&w, hex);
    axl_snprintf(hex, sizeof(hex), "0x%llX", (unsigned long long)r.execute_status);
    axl_json_key(&w, "execute");
    axl_json_str(&w, hex);
    axl_json_key(&w, "shell_exiting");
    axl_json_bool(&w, g_agent.shell_exiting);
    axl_json_key(&w, "canary");
    axl_json_str(&w, g_canary_names[canary]);
    axl_json_key(&w, "reset_ok");
    axl_json_bool(&w, r.reset_ok);
    axl_json_key(&w, "ms");
    axl_json_uint(&w, ms);
    put_stream(&w, "stdout", "stdout_state", &out);
    put_stream(&w, "stderr", "stderr_state", &err);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    axl_free(out.text);
    axl_free(err.text);
    int ret = reply_json(resp, s, s != NULL && !axl_json_writer_error(&w));
    axl_string_free(s);
    return ret;
}

/* Resolve the capture directory to an absolute path with a trailing
   separator, in g_agent.out_dir. NULL on success, else why not. */
static const char *
resolve_out_dir(
    const char *dir,
    const char *cwd
    )
{
    char full[AGENT_PATH_MAX];
    if (axl_strchr(dir, ':') != NULL) {
        if (axl_strlen(dir) + 2 > sizeof(full)) {
            return "is too long";
        }
        axl_snprintf(full, sizeof(full), "%s", dir);
    } else if (dir[0] == '\\') {
        /* Rooted on the current volume: that volume's name, then the path. */
        const char *colon = cwd != NULL ? axl_strchr(cwd, ':') : NULL;
        if (colon == NULL) {
            return "is rooted, and the Shell has no current volume (pass an absolute --out)";
        }
        size_t vl = (size_t)(colon - cwd) + 1u;
        if (vl + axl_strlen(dir) + 2 > sizeof(full)) {
            return "is too long";
        }
        axl_snprintf(full, sizeof(full), "%.*s%s", (int)vl, cwd, dir);
    } else {
        if (cwd == NULL) {
            return "is relative, and the Shell has no current directory (pass an absolute --out)";
        }
        size_t      cl  = axl_strlen(cwd);
        const char *sep = (cl > 0 && cwd[cl - 1] == '\\') || dir[0] == '\0' ? "" : "\\";
        if (cl + axl_strlen(dir) + 3 > sizeof(full)) {
            return "is too long";
        }
        axl_snprintf(full, sizeof(full), "%s%s%s", cwd, sep, dir);
    }
    for (const char *p = full; *p != '\0'; p++) {
        if (axl_strchr(" \t\"%#^|<>", *p) != NULL) {
            return "holds a character the Shell would parse";
        }
    }
    /* `fs0:` alone is that volume's ROOT here, `fs0:\`, never a path
       relative to it. */
    size_t n = axl_strlen(full);
    const char *sep = (n > 0 && full[n - 1] == '\\') ? "" : "\\";
    axl_snprintf(g_agent.out_dir, sizeof(g_agent.out_dir), "%s%s", full, sep);
    return NULL;
}

/* Create and remove a probe file: an unwritable directory would otherwise
   turn every line into a refused redirect. */
static bool
out_dir_writable(void)
{
    char probe[AGENT_PATH_MAX + 48];
    axl_snprintf(probe, sizeof(probe), "%sagent-%s.probe", g_agent.out_dir, g_agent.tag);
    if (axl_file_set_contents(probe, "probe", 5) != AXL_OK) {
        return false;
    }
    return axl_file_delete(probe) == AXL_OK;
}

static void
make_tag(void)
{
    uint64_t v = 0;
    if (axl_rng_bytes(&v, sizeof(v)) != AXL_OK) {
        v = axl_time_get_us() * 0x9E3779B97F4A7C15ULL ^ (uint64_t)g_agent.port;
    }
    axl_snprintf(g_agent.tag, sizeof(g_agent.tag), "%016llx", (unsigned long long)v);
}

/* A decimal port, 1..65535, all digits. */
static bool
parse_port(
    const char *s,
    size_t     *port
    )
{
    uint32_t v = 0;
    if (s[0] == '\0') {
        return false;
    }
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || v > 65535u) {
            return false;
        }
        v = v * 10u + (uint32_t)(*p - '0');
    }
    if (v == 0 || v > 65535u) {
        return false;
    }
    *port = v;
    return true;
}

static void
usage(void)
{
    axl_printf("usage: agent [--port N] [--out DIR] [--latency firmware|window|always]\n"
               "Runs Shell command lines sent over HTTP (POST /exec); see tools/agent.c.\n");
}

/* The agent's whole serving life, once the options are settled. */
static int
serve(
    const AxlNetLatency *lat
    )
{
    if (axl_net_set_latency(lat) != AXL_OK) {
        axl_printerr("agent: cannot set the network latency policy\n");
        return 1;
    }
    g_agent.latency = *lat;
    if (axl_net_auto_init(AXL_NET_NIC_AUTO, 30) != AXL_OK) {
        axl_printerr("agent: network bring-up failed\n");
        return 1;
    }
    AxlHttpServer *server = axl_http_server_new(g_agent.port);
    if (server == NULL
        || axl_http_server_add_routes(server,
               "GET",  "/ping", on_ping, NULL,
               "GET",  "/info", on_info, NULL,
               "POST", "/exec", on_exec, NULL,
               "POST", "/quit", on_quit, NULL,
               NULL) != AXL_OK) {
        axl_printerr("agent: cannot create the HTTP server\n");
        axl_http_server_free(server, AXL_TEARDOWN_RESET);
        return 1;
    }
    axl_http_server_set_keep_alive(server, true);
    g_agent.loop = axl_loop_new();
    if (g_agent.loop == NULL || axl_http_server_start(server, g_agent.loop) != AXL_OK) {
        axl_printerr("agent: cannot start the HTTP server on port %u\n",
                     (unsigned)g_agent.port);
        axl_http_server_free(server, AXL_TEARDOWN_RESET);
        axl_loop_free(g_agent.loop);
        return 1;
    }
    axl_printf("AXL-AGENT-READY port=%u nonest=%d\n", (unsigned)g_agent.port,
               nonest_now() ? 1 : 0);
    axl_loop_run(g_agent.loop);
    /* The server's sources live on the loop: it goes first. */
    axl_http_server_free(server, AXL_TEARDOWN_RESET);
    axl_loop_free(g_agent.loop);
    g_agent.loop = NULL;
    return 0;
}

int
main(
    int   argc,
    char *argv[]
    )
{
    if (axl_version_handle("agent", argc, argv)) {
        return 0;
    }
    g_agent.port = AGENT_PORT;
    const char   *out_arg = NULL;
    AxlNetLatency lat     = { .mode = AXL_NET_LATENCY_ALWAYS };
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool takes_val = axl_strcmp(a, "--port") == 0 || axl_strcmp(a, "--out") == 0
                         || axl_strcmp(a, "--latency") == 0;
        if (axl_strcmp(a, "-h") == 0 || axl_strcmp(a, "--help") == 0) {
            usage();
            return 0;
        }
        if (!takes_val) {
            axl_printerr("agent: unknown option %s\n", a);
            return 2;
        }
        if (i + 1 >= argc) {
            axl_printerr("agent: %s needs a value\n", a);
            return 2;
        }
        const char *v = argv[++i];
        if (axl_strcmp(a, "--port") == 0) {
            if (!parse_port(v, &g_agent.port)) {
                axl_printerr("agent: bad port %s (1..65535)\n", v);
                return 2;
            }
        } else if (axl_strcmp(a, "--out") == 0) {
            out_arg = v;
        } else if (axl_strcmp(v, "firmware") == 0) {
            lat.mode = AXL_NET_LATENCY_FIRMWARE;
        } else if (axl_strcmp(v, "window") == 0) {
            lat.mode = AXL_NET_LATENCY_WINDOW;
        } else if (axl_strcmp(v, "always") == 0) {
            lat.mode = AXL_NET_LATENCY_ALWAYS;
        } else {
            axl_printerr("agent: unknown latency mode %s\n", v);
            return 2;
        }
    }
    make_tag();

    /* The capture directory: --out, else where argv[0] says the agent was
       started from, else the current directory -- made absolute. */
    char       *cwd     = axl_get_current_dir();
    char       *argv0_d = NULL;
    const char *dir     = out_arg;
    if (dir == NULL) {
        if (argc > 0 && argv[0] != NULL && axl_strchr(argv[0], '\\') != NULL) {
            argv0_d = axl_path_get_dirname(argv[0]);
        }
        dir = argv0_d != NULL ? argv0_d : "";
    }
    const char *why = resolve_out_dir(dir, cwd);
    if (why == NULL && !out_dir_writable()) {
        why = "is not writable";
    }
    if (why != NULL) {
        axl_printerr("agent: the capture directory %s %s\n", dir[0] != '\0' ? dir : "(current)", why);
        axl_free(argv0_d);
        axl_free(cwd);
        return 2;
    }
    axl_free(argv0_d);
    /* The Shell's status reset needs a current directory. */
    bool no_cwd = cwd == NULL;
    axl_free(cwd);
    if (no_cwd && axl_chdir(g_agent.out_dir) != AXL_OK) {
        axl_printerr("agent: no current directory, and cannot change to %s\n", g_agent.out_dir);
        return 1;
    }
    axl_snprintf(g_agent.canary, sizeof(g_agent.canary), "%sagent-%s.canary.nsh",
                 g_agent.out_dir, g_agent.tag);
    char home[AGENT_PATH_MAX + 64];
    home_path(home, sizeof(home), g_agent.out_dir);
    if (!write_canary() || axl_file_set_contents(home, g_agent.tag, axl_strlen(g_agent.tag)) != AXL_OK) {
        axl_printerr("agent: cannot write the canary script %s or the marker %s\n",
                     g_agent.canary, home);
        return 1;
    }
    int rc = serve(&lat);
    follow_home();                      /* wherever the directory is by now */
    home_path(home, sizeof(home), g_agent.out_dir);
    axl_file_delete(g_agent.canary);
    axl_file_delete(home);
    return rc;
}
