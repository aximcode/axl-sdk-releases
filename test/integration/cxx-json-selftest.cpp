/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cxx-json-selftest.cpp
    Fixture for test-cxx-json-qemu.sh: the C++ JSON API -- phase C6 of
    AXL-Cxx-Design.md section 9.

    Driven by a verb:

      read     axl::json_document / json_value navigation and extraction
      chain    the simdjson-style error chaining, which is C6's core decision
      range    range-for over arrays and objects
      write    axl::json_writer, RAII scopes, templated add
      splice   the reader->writer bridge (axl_json_write_token)
      scan     axl::json_scanner, the streaming read face
      own      parse vs parse_owning, and the lifetime difference

    Every line printed is asserted with grep -Fxq, so the text below IS the
    contract. Nothing here prints a value it did not compute.

    Sequencing rule, learned the hard way three times in cxx-seam-selftest.cpp:
    C++ argument evaluation order is UNSPECIFIED and gcc goes right-to-left, so
    any axl_printf argument list with more than one side-effecting call gets
    sequenced into const locals first.
**/

#include <stdint.h>

#include <algorithm>
#include <ranges>
#include <string>

#include <axl/axl-json.h>
#include <axl/axl-json.hpp>
#include <axl/axl-mem.h>
#include <axl/axl-stream.h>
#include <axl/axl-str.h>
#include <axl/axl-string.h>

// A document exercising every shape the API navigates: nesting, an array of
// objects, escapes in BOTH keys and values, a null, a bool, a float, and a
// key long enough that a fixed buffer would truncate it.
static const char *kDoc =
    "{"
      "\"name\":\"axl\","
      "\"esc\":\"a\\u0041b\","
      "\"net\":{\"listen\":{\"port\":8080,\"host\":\"::1\"}},"
      "\"items\":[{\"id\":1},{\"id\":2},{\"id\":3}],"
      "\"flags\":[true,false],"
      "\"scale\":1.5,"
      "\"nothing\":null,"
      "\"headers\":{\"Accept\":\"*/*\",\"X-\\u0041\":\"esc-key\"},"
      "\"a-very-long-key-that-no-fixed-buffer-would-hold-intact\":\"long\""
    "}";

// ---------------------------------------------------------------------------
// read
// ---------------------------------------------------------------------------

static void
verb_read(void)
{
    auto doc = axl::json_document::parse(kDoc);
    axl_printf("read: parsed %d\n", (int)doc.has_value());
    if (!doc) { return; }

    const axl::json_value &d = *doc;

    const auto name = d["name"].as_string();
    axl_printf("read: name %s %d\n",
               name.value_or("<none>").c_str(), (int)name.has_value());

    // The escape must be DECODED, and sized exactly -- this is what the new
    // axl_json_get_string_len exists for. Source is 7 bytes, decoded is 3.
    const auto esc = d["esc"].as_string();
    axl_printf("read: esc %s %d\n",
               esc.value_or("").c_str(), (int)esc.value_or("").size());

    // A key too long for any fixed buffer. Before C6's C additions this could
    // not be read into a std::string at all.
    const auto lng = d["a-very-long-key-that-no-fixed-buffer-would-hold-intact"]
                         .as_string();
    axl_printf("read: longkey %s\n", lng.value_or("<none>").c_str());

    const auto port = d["net"]["listen"]["port"].as<int64_t>();
    axl_printf("read: port %d %d\n",
               (int)port.value_or(-1), (int)port.has_value());

    const auto scale = d["scale"].as<double>();
    axl_printf("read: scale %d %d\n",
               (int)(scale.value_or(0) * 100), (int)scale.has_value());

    const auto on = d["flags"].array().begin()->as<bool>();
    axl_printf("read: bool %d\n", (int)on.value_or(false));

    // A JSON null EXISTS. Conflating it with an absent key is the distinction
    // the C reader keeps and this keeps with it.
    axl_printf("read: null %d %d %d\n",
               (int)d["nothing"].exists(),
               (int)d["nothing"].is_null(),
               (int)d["absent"].exists());

    axl_printf("read: types %d %d %d %d\n",
               (int)d["net"].is_object(), (int)d["items"].is_array(),
               (int)d["name"].is_string(), (int)d["scale"].is_number());

    axl_printf("read: done\n");
}

// ---------------------------------------------------------------------------
// chain -- C6's central decision
// ---------------------------------------------------------------------------

static void
verb_chain(void)
{
    auto doc = axl::json_document::parse(kDoc);
    if (!doc) { axl_printf("chain: parse FAILED\n"); return; }
    const axl::json_value &d = *doc;

    // A missing FIRST step propagates through two more lookups untouched, and
    // the error that surfaces is the one from where it actually went wrong.
    const auto miss = d["nope"]["listen"]["port"].as<int64_t>();
    axl_printf("chain: missing %d %d\n",
               (int)miss.has_value(),
               (int)(miss.error() == AXL_NOT_FOUND));

    // Indexing a NON-OBJECT is a different failure from a missing key, and
    // collapsing them would make a typo and a schema mismatch identical.
    const auto wrong = d["name"]["port"].as<int64_t>();
    axl_printf("chain: notobject %d\n", (int)(wrong.error() == AXL_INVALID));

    // A present key read as the WRONG TYPE.
    const auto badtype = d["name"].as<int64_t>();
    axl_printf("chain: badtype %d %d\n",
               (int)badtype.has_value(), (int)(badtype.error() == AXL_INVALID));

    // The error is the FIRST one in the chain, not the last: "nope" is
    // missing, and the later ["port"] on an errored value must not overwrite
    // AXL_NOT_FOUND with AXL_INVALID.
    const axl::json_value chained = d["nope"]["also-nope"];
    axl_printf("chain: first %d\n", (int)(chained.error() == AXL_NOT_FOUND));

    // A default-constructed value names a failure rather than reading as a
    // valid empty document.
    const axl::json_value empty;
    axl_printf("chain: default %d %d\n",
               (int)empty.exists(), (int)(empty.error() == AXL_NOT_FOUND));

    // value_or on the chain is the one-liner the model exists to enable.
    axl_printf("chain: valueor %d %d\n",
               (int)d["net"]["listen"]["port"].as<int64_t>().value_or(-1),
               (int)d["net"]["missing"]["port"].as<int64_t>().value_or(-1));

    axl_printf("chain: done\n");
}

// ---------------------------------------------------------------------------
// range
// ---------------------------------------------------------------------------

static void
verb_range(void)
{
    auto doc = axl::json_document::parse(kDoc);
    if (!doc) { axl_printf("range: parse FAILED\n"); return; }
    const axl::json_value &d = *doc;

    int64_t sum = 0;
    int     n   = 0;
    for (axl::json_value v : d["items"].array()) {
        sum += v["id"].as<int64_t>().value_or(0);
        n++;
    }
    axl_printf("range: array %d %d\n", n, (int)sum);

    // A views pipeline over the array range. AXL-Cxx-Design section 2's trap
    // is an iterator that satisfies std::sort and is then rejected by
    // views::filter, so the compile IS the assertion.
    int odd = 0;
    for (axl::json_value v : d["items"].array()
             | std::views::filter([](const axl::json_value &e) {
                   return e["id"].as<int64_t>().value_or(0) % 2 == 1;
               })) {
        odd += (int)v["id"].as<int64_t>().value_or(0);
    }
    axl_printf("range: filtered %d\n", odd);

    // Object iteration yields OWNED keys, decoded. "X-A" is the key
    // "X-A", and the peek is what sizes it.
    char keys[128];
    size_t k = 0;
    int pairs = 0;
    for (auto &&m : d["headers"].object()) {
        for (size_t i = 0; i < m.key.size() && k < sizeof(keys) - 2; i++) {
            keys[k++] = m.key[i];
        }
        keys[k++] = ';';
        pairs++;
    }
    keys[k] = '\0';
    axl_printf("range: object %d %s\n", pairs, keys);

    // The long key survives object iteration too -- a fixed buffer would
    // have truncated it and reported so only afterwards.
    std::string longest;
    for (auto &&m : d.object()) {
        if (m.key.size() > longest.size()) { longest = m.key; }
    }
    axl_printf("range: longest %d %s\n", (int)longest.size(), longest.c_str());

    // Ranging over something that is not a container yields nothing rather
    // than misbehaving.
    axl_printf("range: notarray %d %d\n",
               (int)std::ranges::distance(d["name"].array()),
               (int)std::ranges::distance(d["name"].object()));
    axl_printf("range: missing %d\n",
               (int)std::ranges::distance(d["absent"].array()));

    axl_printf("range: done\n");
}

// ---------------------------------------------------------------------------
// write
// ---------------------------------------------------------------------------

static void
verb_write(void)
{
    AxlString *out = axl_string_new("");
    {
        axl::json_writer w{out};
        {
            auto o = w.object();
            w.add("name", "axl");
            w.add("port", 8080);
            w.add("scale", 1.5);
            w.add("on", true);
            w.add("off", false);
            w.add("nil", nullptr);
            {
                auto a = w.array("items");
                w.add(1);
                w.add(2u);
                w.add("three");
            }
            {
                auto inner = w.object("nested");
                w.add("deep", 1);
            }
        }
        const auto n = w.finish();
        axl_printf("write: ok %d %d\n", (int)n.has_value(),
                   (int)(n.value_or(0) == axl_string_len(out)));
    }
    axl_printf("write: doc %s\n", axl_string_str(out));
    axl_string_free(out);

    // The scopes close on EVERY path out, including an early return -- which
    // is the whole reason they exist. A hand-balanced C writer leaks the
    // container and reports it only at finish().
    AxlString *e = axl_string_new("");
    {
        axl::json_writer w{e};
        auto o = w.object();
        w.add("a", 1);
        // falls out of scope here; `}` below closes the object
    }
    axl_printf("write: earlyexit %s\n", axl_string_str(e));
    axl_string_free(e);

    // An UNCLOSED container is a finish() error. Built by hand, because the
    // scope type makes this unreachable by construction.
    AxlString *u = axl_string_new("");
    {
        axl::json_writer w{u};
        axl_json_obj_begin(w.get());
        const auto n = w.finish();
        axl_printf("write: unclosed %d %d\n",
                   (int)n.has_value(), (int)w.failed());
    }
    axl_string_free(u);

    // std::string and string_view both route to the counted emitter.
    AxlString *s = axl_string_new("");
    {
        axl::json_writer w{s};
        {
            auto a = w.array();
            w.add(std::string("str"));
            w.add(std::string_view("view"));
            const char *np = nullptr;
            w.add(np);
        }
        (void)w.finish();
    }
    axl_printf("write: strings %s\n", axl_string_str(s));
    axl_string_free(s);

    axl_printf("write: done\n");
}

// ---------------------------------------------------------------------------
// splice -- the reader->writer bridge
// ---------------------------------------------------------------------------

static void
verb_splice(void)
{
    auto doc = axl::json_document::parse(kDoc);
    if (!doc) { axl_printf("splice: parse FAILED\n"); return; }
    const axl::json_value &d = *doc;

    AxlString *out = axl_string_new("");
    {
        axl::json_writer w{out};
        {
            auto o = w.object();
            w.splice("items", d["items"]);       // a whole subtree, verbatim
            w.add("added", 1);
        }
        (void)w.finish();
    }
    axl_printf("splice: doc %s\n", axl_string_str(out));

    // The spliced output must REPARSE and carry the same values -- an
    // exact-string check alone would not prove it is still valid JSON.
    const auto round = axl::json_document::parse_owning(
        std::string(axl_string_str(out), axl_string_len(out)));
    int64_t sum = 0;
    if (round) {
        for (axl::json_value v : (*round)["items"].array()) {
            sum += v["id"].as<int64_t>().value_or(0);
        }
    }
    axl_printf("splice: round %d %d\n", (int)round.has_value(), (int)sum);
    axl_string_free(out);

    // Splicing an ERRORED value emits nothing rather than corrupting the
    // document -- the alternative is a key with no value, which is not JSON.
    AxlString *m = axl_string_new("");
    {
        axl::json_writer w{m};
        {
            auto o = w.object();
            w.splice("gone", d["absent"]);
            w.add("kept", 2);
        }
        const auto n = w.finish();
        axl_printf("splice: missing %s %d\n",
                   axl_string_str(m), (int)n.has_value());
    }
    axl_string_free(m);

    axl_printf("splice: done\n");
}

// ---------------------------------------------------------------------------
// scan -- the streaming read face
// ---------------------------------------------------------------------------

static void
verb_scan(void)
{
    int keys = 0, strings = 0, numbers = 0, objs = 0, arrs = 0;
    bool saw_port = false;
    {
        axl::json_scanner sc{kDoc};
        for (const axl::json_event &ev : sc) {
            switch (ev.kind) {
            case AXL_JSON_EV_KEY:
                keys++;
                if (ev.equals("port")) { saw_port = true; }
                break;
            case AXL_JSON_EV_STRING: strings++; break;
            case AXL_JSON_EV_NUMBER: numbers++; break;
            case AXL_JSON_EV_OBJ_BEGIN: objs++; break;
            case AXL_JSON_EV_ARR_BEGIN: arrs++; break;
            default: break;
            }
        }
        axl_printf("scan: counts %d %d %d %d %d %d\n",
                   keys, strings, numbers, objs, arrs, (int)saw_port);
        axl_printf("scan: clean %d\n", (int)!sc.failed());
    }

    // equals() compares DECODED, so an escaped key matches its plain
    // spelling -- the reason axl_json_event_equals exists.
    {
        axl::json_scanner sc{"{\"X-\\u0041\":1}"};
        bool matched = false;
        for (const axl::json_event &ev : sc) {
            if (ev.kind == AXL_JSON_EV_KEY && ev.equals("X-A")) { matched = true; }
        }
        axl_printf("scan: escapedkey %d\n", (int)matched);
    }

    // text_copy() is the way to keep a key past the next advance; the
    // borrowed view dies there by contract.
    {
        axl::json_scanner sc{"{\"kept\":1,\"also\":2}"};
        std::string first;
        for (const axl::json_event &ev : sc) {
            if (ev.kind == AXL_JSON_EV_KEY && first.empty()) {
                first = ev.text_copy();
            }
        }
        axl_printf("scan: copied %s\n", first.c_str());
    }

    // A MALFORMED document ends the loop the same way a complete one does,
    // so failed() is the only way to tell them apart.
    {
        axl::json_scanner bad{"{\"a\":"};
        int seen = 0;
        for (const axl::json_event &ev : bad) { (void)ev; seen++; }
        axl_printf("scan: malformed %d\n", (int)bad.failed());
    }

    axl_printf("scan: done\n");
}

// ---------------------------------------------------------------------------
// own -- the two factories differ only in who holds the bytes
// ---------------------------------------------------------------------------

static void
verb_own(void)
{
    // parse_owning takes a TEMPORARY and keeps it. With parse() this would be
    // a dangling document reading released memory -- which is exactly why the
    // two are named rather than overloaded.
    const auto owned = axl::json_document::parse_owning(
        std::string("{\"k\":\"kept-alive\"}"));
    axl_printf("own: owning %s\n",
               owned ? (*owned)["k"].as_string().value_or("<none>").c_str()
                     : "<parse failed>");

    // A document is move-only, and the moved-from one must not double-free.
    auto a = axl::json_document::parse_owning(std::string("{\"v\":7}"));
    axl_printf("own: before %d\n", (int)(*a)["v"].as<int64_t>().value_or(-1));
    axl::json_document b = std::move(*a);
    axl_printf("own: moved %d %d\n",
               (int)b["v"].as<int64_t>().value_or(-1), (int)(*a).exists());

    // Move ASSIGNMENT must free the destination's document first; the leak
    // gate is what notices if it does not.
    auto c = axl::json_document::parse_owning(std::string("{\"v\":9}"));
    axl::json_document sink =
        std::move(*axl::json_document::parse_owning(std::string("{\"v\":1}")));
    sink = std::move(*c);
    axl_printf("own: assigned %d\n", (int)sink["v"].as<int64_t>().value_or(-1));

    // A parse FAILURE is a value, not a halt.
    const auto bad = axl::json_document::parse("{not json", AXL_JSON_STRICT);
    axl_printf("own: badparse %d %d\n",
               (int)bad.has_value(), (int)(bad.error() == AXL_INVALID));

    axl_printf("own: done\n");
}

int
main(int argc, char **argv)
{
    const char *verb = argc > 1 ? argv[1] : "read";

    if (axl_streql(verb, "read")) {
        verb_read();
    } else if (axl_streql(verb, "chain")) {
        verb_chain();
    } else if (axl_streql(verb, "range")) {
        verb_range();
    } else if (axl_streql(verb, "write")) {
        verb_write();
    } else if (axl_streql(verb, "splice")) {
        verb_splice();
    } else if (axl_streql(verb, "scan")) {
        verb_scan();
    } else if (axl_streql(verb, "own")) {
        verb_own();
    } else {
        axl_printf("unknown verb: %s\n", verb);
        return 1;
    }
    return 0;
}
