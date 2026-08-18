/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cxx-seam-selftest.cpp
    Fixture for test-cxx-seam-qemu.sh: the C++ seams over AXL's C API --
    phases C2, C3 and C5 of AXL-Cxx-Design.md section 9.

    Driven by a verb so a halting case can be run without killing the rest:

      cstr      axl::view / axl::adopt                        (C2)
      array     axl::array_span / axl::array_ptr_span         (C3)
      ntree     axl::children / ancestors / preorder / ...    (C5)
      radix     axl::radix_tree<T>                            (C5)
      gfx       axl::gfx_target_scope                         (C5)
      mismatch  a DELIBERATE element-size mismatch; must HALT (C3)

    Every line printed is asserted with grep -Fxq, so the text below IS the
    contract. Nothing here prints a value it did not compute.

    Two things this fixture pins that are easy to leave untested:

    1. A `std::views::filter` pipeline over every range. AXL-Cxx-Design.md
       section 2's second trap is a hand-rolled iterator that satisfied
       std::sort and was then REJECTED by views::filter for lacking a default
       constructor -- C++20 iterator concepts refine std::semiregular where
       iterator_traits did not. Only a views pipeline catches it, and it
       catches it at COMPILE time, so these lines failing to build is the
       assertion.
    2. The live axl_malloc count across axl::adopt. "It returned the right
       string" would pass just as well while leaking the C buffer, and the
       suite's leak gate only fires at teardown -- by which point the verb
       that leaked is no longer identifiable.
**/

#include <stdint.h>

#include <algorithm>
#include <ranges>
#include <string>

#include <axl/axl-array.h>
#include <axl/axl-atexit.h>
#include <axl/axl-array.hpp>
#include <axl/axl-cstr.hpp>
#include <axl/axl-gfx-surface.hpp>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-ntree.h>
#include <axl/axl-ntree.hpp>
#include <axl/axl-radix-tree.hpp>
#include <axl/axl-stream.h>
#include <axl/axl-str.h>
#include <axl/axl-string.hpp>

// ---------------------------------------------------------------------------
// C2 -- the C-string seam
// ---------------------------------------------------------------------------

static void
verb_cstr(void)
{
    // --- axl::view: the NULL case is the entire reason it exists ----------
    std::string_view h = axl::view("hello");
    axl_printf("cstr: view %zu %d\n", h.size(), (int)(h == "hello"));

    std::string_view n = axl::view(nullptr);
    // data() must NOT be null: a view built from a C string is always
    // NUL-terminated at data()[size()], so axl::view(x).data() is a safe
    // `const char *` whatever x was. A default-constructed string_view has
    // data() == nullptr and would break that.
    axl_printf("cstr: viewnull %zu %d %d %d\n",
               n.size(), (int)n.empty(),
               (int)(n.data() != nullptr),
               (int)(n.data() != nullptr && n.data()[0] == '\0'));

    // The data() checks must be applied to the NON-NULL empty input too.
    // Asserting them only on the NULL input let `view("")` return a
    // default-constructed view -- data() == nullptr -- while both lines
    // stayed green, breaking the header's headline guarantee.
    std::string_view e = axl::view("");
    axl_printf("cstr: viewempty %zu %d %d %d\n", e.size(), (int)(e == n),
               (int)(e.data() != nullptr),
               (int)(e.data() != nullptr && e.data()[0] == '\0'));

    // --- axl::adopt: the copy is right AND the C buffer is released -------
    AxlMemStats before;
    axl_mem_get_stats(&before);

    std::string s = axl::adopt(axl_strdup("adopted"));
    axl_printf("cstr: adopt %s %zu\n", s.c_str(), s.size());

    // Long enough to defeat any small-string buffer, so the copy is a real
    // heap allocation on the C++ side while the C side is freed.
    std::string big = axl::adopt(
        axl_strdup("a-string-far-too-long-to-live-inside-any-sso-buffer"));
    axl_printf("cstr: adoptbig %zu %d\n", big.size(),
               (int)(big.starts_with("a-string-far") && big.ends_with("buffer")));

    std::string z = axl::adopt(nullptr);
    axl_printf("cstr: adoptnull %d %zu\n", (int)z.empty(), z.size());

    AxlMemStats after;
    axl_mem_get_stats(&after);
    // THE assertion of this verb. std::string allocates through newlib
    // malloc, not axl_malloc, so the axl live count sees only the strdup/free
    // pairs -- it must be back exactly where it started.
    axl_printf("cstr: freed %d\n", (int)(after.count == before.count));

    // --- the typed form, which is what AXL-Cxx-Design section 9c is for ---
    axl::string t = axl::adopt<axl::string>(axl_strdup("typed-target"));
    axl_printf("cstr: typed %s %d %d\n", t.c_str(), (int)t.size(), (int)t.bad());

    axl::string tz = axl::adopt<axl::string>(nullptr);
    axl_printf("cstr: typednull %d %d\n", (int)tz.empty(), (int)tz.bad());

    // Past axl::string's 23-byte SSO buffer, so the copy is a real
    // axl_malloc. Both the typed cases above are short enough to live
    // inline, which meant bad() was false BY CONSTRUCTION and the heap path
    // was never taken.
    axl::string tbig = axl::adopt<axl::string>(
        axl_strdup("a-typed-string-well-past-the-inline-buffer"));
    axl_printf("cstr: typedbig %d %d %d\n", (int)tbig.size(), (int)tbig.bad(),
               (int)(axl_strcmp(tbig.c_str(),
                     "a-typed-string-well-past-the-inline-buffer") == 0));

    // THE reason adopt<> is templated at all: std::string HALTS when the copy
    // cannot be allocated, axl::string sets a sticky bad() the caller reads.
    // Every other typed assertion here is short enough to skip allocation, so
    // without this the one documented difference between the two destinations
    // has no test. The strdup happens BEFORE arming, or the injection would
    // be eaten by it instead.
    char *raw = axl_strdup("another-string-well-past-the-inline-buffer");
    axl_mem_fail_next_alloc(1);
    axl::string toom = axl::adopt<axl::string>(raw);
    axl_printf("cstr: typedoom %d %d\n", (int)toom.bad(), (int)toom.empty());

    axl_printf("cstr: done\n");
}

// ---------------------------------------------------------------------------
// C3 -- AxlArray as a span
// ---------------------------------------------------------------------------

// sizeof(Item) is deliberately NOT sizeof(void *). With `{int; bool}` it was
// exactly 8 on both arches -- the same as a pointer -- so array_ptr_span's
// stride check could have been written against sizeof(T) instead of
// sizeof(void *) and every assertion below would still have passed. The
// sabotage pass caught that; the padding field is the fix.
struct Item {
    int  id;
    bool visible;
    char name[16];
};
static_assert(sizeof(Item) != sizeof(void *),
              "Item must not be pointer-sized, or the pointer-mode stride "
              "check is untested");

static void
verb_array(void)
{
    AxlArray *a = axl_array_new(sizeof(int));
    for (int i = 5; i > 0; i--) {
        axl_array_append(a, &i);            // [5,4,3,2,1]
    }

    std::span<int> v = axl::array_span<int>(a);
    axl_printf("array: span %zu %d %d\n", v.size(), v.front(), v.back());

    int sum = 0;
    for (int x : v) { sum += x; }
    axl_printf("array: sum %d\n", sum);

    // std::sort over the span writes THROUGH to the array's own buffer --
    // this is a view, not a copy, and reading the result back through the C
    // accessor is what proves it.
    std::sort(v.begin(), v.end());
    axl_printf("array: sorted %d %d %d\n",
               *(int *)axl_array_get(a, 0),
               *(int *)axl_array_get(a, 2),
               *(int *)axl_array_get(a, 4));

    // The views::filter pipeline. If the element type were reached through a
    // proxy iterator this would not compile (see the file header).
    int evens = 0;
    for (int x : v | std::views::filter([](int i) { return i % 2 == 0; })) {
        evens += x;
    }
    axl_printf("array: evens %d\n", evens);

    // A span<T> converts to span<const T> with no separate call.
    std::span<const int> ro = v;
    axl_printf("array: ro %zu %d\n", ro.size(), ro[0]);

    axl_printf("array: null %d %d\n",
               (int)axl::array_span<int>(nullptr).size(),
               (int)axl::array_span<int>(nullptr).empty());
    axl_array_free(a);

    // --- pointer mode -----------------------------------------------------
    static Item items[] = { {10, true, "a"}, {20, false, "b"}, {30, true, "c"} };
    AxlArray *p = axl_array_new(sizeof(void *));
    for (Item &it : items) {
        axl_array_append_ptr(p, &it);
    }

    std::span<Item *> sp = axl::array_ptr_span<Item>(p);
    axl_printf("array: ptrspan %zu %d %d\n", sp.size(), sp[0]->id, sp[2]->id);

    int shown = 0;
    for (Item *it : sp | std::views::filter([](Item *i) { return i->visible; })) {
        shown += it->id;
    }
    axl_printf("array: ptrfilter %d\n", shown);

    // AgtTreeView::row_of_ is exactly this loop, hand-written.
    auto found = std::ranges::find(sp, &items[1]);
    axl_printf("array: find %d\n", (int)(found - sp.begin()));

    axl_printf("array: ptrnull %d\n",
               (int)axl::array_ptr_span<Item>(nullptr).size());

    // The const overloads. Without these a class holding a
    // `const AxlArray *` could not use this header at all.
    const AxlArray *cp = p;
    std::span<Item *const> csp = axl::array_ptr_span<Item>(cp);
    axl_printf("array: constptr %zu %d\n", csp.size(), csp[1]->id);
    axl_array_free(p);

    // A NON-NULL, ZERO-LENGTH array. This is the only case that distinguishes
    // "empty span over a real buffer" from "span over nothing", and it was
    // untested -- both the NULL and stolen cases take the other branch.
    AxlArray *empty = axl_array_new(sizeof(int));
    std::span<int> es = axl::array_span<int>(empty);
    axl_printf("array: emptyarr %zu %d\n", es.size(), (int)es.empty());

    const AxlArray *ce = empty;
    std::span<const int> ces = axl::array_span<int>(ce);
    axl_printf("array: constempty %zu\n", ces.size());
    axl_array_free(empty);

    // A stolen array reports NULL data with length 0 -- same branch as NULL,
    // named separately in the header, so pinned separately here.
    AxlArray *st = axl_array_new(sizeof(int));
    int one = 1;
    axl_array_append(st, &one);
    size_t stn = 0;
    void *stolen = axl_array_steal(st, &stn);
    axl_printf("array: stolen %zu %d\n",
               axl::array_span<int>(st).size(), (int)stn);
    axl_free(stolen);
    axl_array_free(st);

    // The literal nullptr now resolves; it used to be ambiguous once the
    // const overload existed, which is what the nullptr_t overload is for.
    axl_printf("array: nulllit %zu %zu\n",
               axl::array_span<int>(nullptr).size(),
               axl::array_ptr_span<Item>(nullptr).size());

    axl_printf("array: done\n");
}

// ---------------------------------------------------------------------------
// C5 -- AxlNTree as ranges
// ---------------------------------------------------------------------------

// Payload: a single letter, so a traversal renders as a readable word and the
// ORDER is pinned rather than only the count.
struct Label { char c; };

static AxlNTree *
add_labeled(AxlNTree *parent, Label *l)
{
    return axl_ntree_append_data(parent, l);
}

static void
verb_ntree(void)
{
    //        a
    //      +-+-+---+
    //      b  c    f
    //         +-+-+
    //         d   e
    static Label la{'a'}, lb{'b'}, lc{'c'}, ld{'d'}, le{'e'}, lf{'f'};

    AxlNTree *a = axl_ntree_new(&la);
    AxlNTree *b = add_labeled(a, &lb);
    AxlNTree *c = add_labeled(a, &lc);
    AxlNTree *f = add_labeled(a, &lf);
    AxlNTree *d = add_labeled(c, &ld);
    AxlNTree *e = add_labeled(c, &le);

    char buf[32];
    size_t i;

    // --- children: order, and the leaf/NULL cases ------------------------
    i = 0;
    for (AxlNTree *n : axl::children(a)) {
        buf[i++] = axl::data_of<Label>(n)->c;
    }
    buf[i] = '\0';
    axl_printf("ntree: children %s %zu\n", buf, i);

    axl_printf("ntree: leaf %d %d\n",
               (int)std::ranges::distance(axl::children(b)),
               (int)std::ranges::distance(axl::children(static_cast<AxlNTree *>(nullptr))));

    // --- ancestors: excludes the node, so the count IS the depth ---------
    i = 0;
    for (AxlNTree *n : axl::ancestors(d)) {
        buf[i++] = axl::data_of<Label>(n)->c;
    }
    buf[i] = '\0';
    // axl_ntree_depth counts the root as depth 1, so depth-1 edges are walked.
    axl_printf("ntree: ancestors %s %zu %d\n",
               buf, i, (int)(i == (size_t)axl_ntree_depth(d) - 1));

    axl_printf("ntree: rootup %d\n",
               (int)std::ranges::distance(axl::ancestors(a)));

    // --- preorder / postorder: exact orders, not just counts -------------
    i = 0;
    for (AxlNTree *n : axl::preorder(a)) {
        buf[i++] = axl::data_of<Label>(n)->c;
    }
    buf[i] = '\0';
    axl_printf("ntree: preorder %s %d\n", buf,
               (int)(i == (size_t)axl_ntree_n_nodes(a, AXL_NTREE_ALL)));

    i = 0;
    for (AxlNTree *n : axl::postorder(a)) {
        buf[i++] = axl::data_of<Label>(n)->c;
    }
    buf[i] = '\0';
    axl_printf("ntree: postorder %s %zu\n", buf, i);

    // A subtree walk must stay INSIDE the subtree: starting at c must not
    // escape to c's sibling f. This is the bound the iterator checks against
    // m_root, and the one that is easy to get wrong in both directions.
    i = 0;
    for (AxlNTree *n : axl::preorder(c)) {
        buf[i++] = axl::data_of<Label>(n)->c;
    }
    buf[i] = '\0';
    axl_printf("ntree: subtree %s\n", buf);

    // --- preorder_pruned: the node is VISITED, its subtree is SKIPPED ----
    // The distinction that matters for a collapsed tree view: `c` is still
    // drawn, `d` and `e` are not. Composing views::filter over preorder
    // cannot express this -- filter removes a node from the OUTPUT, while
    // the walk has already descended into it.
    char pc[32], pnone[32], pall[32];
    i = 0;
    for (AxlNTree *n : axl::preorder_pruned(a, [c](const AxlNTree *n) {
             return n != c;                       // c is "collapsed"
         })) {
        pc[i++] = axl::data_of<Label>(n)->c;
    }
    pc[i] = '\0';

    // Pruning at the ROOT yields the root alone, not an empty range.
    i = 0;
    for (AxlNTree *n : axl::preorder_pruned(a, [](const AxlNTree *) {
             return false;
         })) {
        pnone[i++] = axl::data_of<Label>(n)->c;
    }
    pnone[i] = '\0';

    // A predicate that never prunes must agree with plain preorder exactly.
    i = 0;
    for (AxlNTree *n : axl::preorder_pruned(a, [](const AxlNTree *) {
             return true;
         })) {
        pall[i++] = axl::data_of<Label>(n)->c;
    }
    pall[i] = '\0';
    axl_printf("ntree: pruned %s %s %s\n", pc, pnone, pall);

    i = 0;
    for (AxlNTree *n : axl::postorder(c)) {
        buf[i++] = axl::data_of<Label>(n)->c;
    }
    buf[i] = '\0';
    axl_printf("ntree: subpost %s\n", buf);

    // A single node is its own whole subtree in both orders.
    axl_printf("ntree: single %d %d\n",
               (int)std::ranges::distance(axl::preorder(e)),
               (int)std::ranges::distance(axl::postorder(e)));

    axl_printf("ntree: nullwalk %d %d\n",
               (int)std::ranges::distance(axl::preorder(static_cast<AxlNTree *>(nullptr))),
               (int)std::ranges::distance(axl::postorder(static_cast<AxlNTree *>(nullptr))));

    // --- the views pipeline, over a tree range ---------------------------
    i = 0;
    for (AxlNTree *n : axl::preorder(a)
             | std::views::filter([](AxlNTree *x) {
                   return axl::data_of<Label>(x)->c >= 'd';
               })) {
        buf[i++] = axl::data_of<Label>(n)->c;
    }
    buf[i] = '\0';
    axl_printf("ntree: filtered %s\n", buf);

    // --- const overloads: a const node yields const nodes ----------------
    const AxlNTree *ca = a;
    i = 0;
    for (const AxlNTree *n : axl::children(ca)) {
        buf[i++] = axl::data_of<Label>(n)->c;
    }
    buf[i] = '\0';
    static_assert(std::is_same_v<decltype(*axl::children(ca).begin()),
                                 const AxlNTree *>,
                  "const overload must yield const AxlNTree *");
    axl_printf("ntree: constchildren %s\n", buf);

    axl_printf("ntree: dataof %d %d\n",
               (int)(axl::data_of<Label>(f)->c == 'f'),
               (int)(axl::data_of<Label>(static_cast<const AxlNTree *>(nullptr)) == nullptr));

    // The three const overloads that had no assertion at all. Members of a
    // class template are instantiated only on use, so their operator++ had
    // never been COMPILED for the const specialization -- a wrong body would
    // have shipped without a diagnostic.
    const AxlNTree *cd = d;
    i = 0;
    for (const AxlNTree *n : axl::ancestors(cd)) { buf[i++] = axl::data_of<Label>(n)->c; }
    buf[i] = '\0';
    axl_printf("ntree: constancestors %s\n", buf);

    const AxlNTree *cc = c;
    i = 0;
    for (const AxlNTree *n : axl::preorder(cc))  { buf[i++] = axl::data_of<Label>(n)->c; }
    buf[i] = '\0';
    axl_printf("ntree: constpre %s\n", buf);

    i = 0;
    for (const AxlNTree *n : axl::postorder(cc)) { buf[i++] = axl::data_of<Label>(n)->c; }
    buf[i] = '\0';
    axl_printf("ntree: constpost %s\n", buf);

    // POST-increment, on both iterator types. Range-for, ranges::distance and
    // views::filter all use the pre-increment form, so `operator++(int)`
    // returning *this instead of the saved copy would have shipped silently.
    auto sib = axl::children(a).begin();
    auto sib_prev = sib++;
    auto sub = axl::preorder(a).begin();
    auto sub_prev = sub++;
    axl_printf("ntree: postinc %c %c %c %c\n",
               axl::data_of<Label>(*sib_prev)->c, axl::data_of<Label>(*sib)->c,
               axl::data_of<Label>(*sub_prev)->c, axl::data_of<Label>(*sub)->c);

    // The documented NULL cases for the remaining factories, now that the
    // literal resolves.
    axl_printf("ntree: nullfactories %d %d %d %d\n",
               (int)std::ranges::distance(axl::children(nullptr)),
               (int)std::ranges::distance(axl::ancestors(nullptr)),
               (int)std::ranges::distance(axl::preorder(nullptr)),
               (int)std::ranges::distance(axl::postorder(nullptr)));

    // A node carrying no payload.
    AxlNTree *bare = axl_ntree_new(nullptr);
    axl_printf("ntree: nodata %d\n", (int)(axl::data_of<Label>(bare) == nullptr));
    axl_ntree_free(bare);

    // BORROWED RANGE. Without the enable_borrowed_range opt-in this does not
    // compile at all: ranges::find_if over a temporary range returns
    // std::ranges::dangling, and `*it` is rejected. That would have made the
    // ntree ranges compose differently from array_span's std::span for no
    // stated reason, so the compile IS the assertion.
    auto found = std::ranges::find_if(axl::children(a), [](AxlNTree *x) {
        return axl::data_of<Label>(x)->c == 'c';
    });
    axl_printf("ntree: borrowed %c\n", axl::data_of<Label>(*found)->c);

    axl_ntree_free(a);   // labels are static; nothing to free per node
    axl_printf("ntree: done\n");
}

// ---------------------------------------------------------------------------
// C5 -- the radix tree wrapper
// ---------------------------------------------------------------------------

struct Route { int id; };

static int route_frees;

static void
free_route(void *p) AXL_CB_NOEXCEPT
{
    route_frees++;
    axl_free(p);
}

// A plain FUNCTION, not a lambda. `for_each(dump_route)` did not compile
// before the shim: F deduced to a function reference, so the header tried to
// static_cast a void * to a function type. A test that only ever passed
// lambdas could not have caught it.
static int route_visits;

static void
dump_route(const char *key, Route *r) AXL_CB_NOEXCEPT
{
    (void)key;
    route_visits += r != nullptr ? r->id : 0;
}

static void
verb_radix(void)
{
    axl::radix_tree<Route> t;
    axl_printf("radix: valid %d %d %zu\n",
               (int)t.valid(), (int)t.empty(), t.size());

    static Route r1{1}, r2{2}, r3{3};
    t.insert("/api/v1/", &r1);
    t.insert("/api/v2/", &r2);
    t.insert("/static/", &r3);
    axl_printf("radix: size %zu %d\n", t.size(), (int)t.empty());

    Route *hit = t.lookup("/api/v2/");
    axl_printf("radix: lookup %d %d\n",
               hit != nullptr ? hit->id : -1,
               (int)(t.lookup("/nope/") == nullptr));

    // Longest-prefix, with the suffix pointing INTO the key we passed.
    const char *key = "/api/v1/users/7";
    const char *tail = nullptr;
    Route *pre = t.lookup_prefix(key, &tail);
    axl_printf("radix: prefix %d %s %d\n",
               pre != nullptr ? pre->id : -1,
               tail != nullptr ? tail : "(none)",
               (int)(tail == key + 8));   // exact offset, not a range

    // The default argument: no suffix wanted.
    axl_printf("radix: prefixnosuffix %d\n",
               t.lookup_prefix("/static/x") != nullptr ? 1 : 0);
    axl_printf("radix: nomatch %d\n",
               (int)(t.lookup_prefix("/zzz") == nullptr));

    // A CAPTURING visitor -- the thing the C foreach cannot take.
    int seen = 0, id_sum = 0;
    t.for_each([&](const char *k, Route *r) {
        seen++;
        id_sum += r->id;
        if (axl_streql(k, "/static/")) { id_sum += 100; }
    });
    axl_printf("radix: foreach %d %d\n", seen, id_sum);

    // Sequenced into locals ON PURPOSE. Argument evaluation order is
    // UNSPECIFIED in C++, and gcc evaluates right-to-left -- so writing these
    // three as arguments to one call ran the second remove() first and
    // reported "0 1 3" for a tree that behaved correctly. Any expression with
    // a side effect gets its own statement here.
    const bool first_remove  = t.remove("/api/v1/");
    const bool second_remove = t.remove("/api/v1/");
    axl_printf("radix: remove %d %d %zu\n",
               (int)first_remove, (int)second_remove, t.size());

    // Move: the source becomes invalid and must not double-free at scope exit.
    axl::radix_tree<Route> moved = std::move(t);
    axl_printf("radix: moved %d %zu %d %zu\n",
               (int)moved.valid(), moved.size(), (int)t.valid(), t.size());
    axl_printf("radix: movedlookup %d\n",
               moved.lookup("/api/v2/") != nullptr ? 1 : 0);

    // Every call on a moved-from tree is a safe no-op, not a fault. Sequenced
    // for the reason above, even though these three happen to be
    // order-independent on an invalid tree.
    const bool moved_lookup = (t.lookup("/api/v2/") == nullptr);
    const bool moved_remove = t.remove("/api/v2/");
    const bool moved_insert = t.insert("/x", &r1);
    axl_printf("radix: emptyops %d %d %d\n",
               (int)moved_lookup, (int)moved_remove, (int)moved_insert);

    // for_each taking a plain function name, and on an INVALID tree.
    route_visits = 0;
    moved.for_each(dump_route);
    const int visits_live = route_visits;
    route_visits = 0;
    t.for_each(dump_route);                        // moved-from: visits nothing
    axl_printf("radix: fnvisitor %d %d\n", visits_live, route_visits);

    // get() / operator bool / release() -- none of these had ever been
    // INSTANTIATED, so a wrong body would not even have been compiled.
    axl_printf("radix: accessors %d %d\n",
               (int)(moved.get() != nullptr), (int)static_cast<bool>(moved));
    axl_printf("radix: boolinvalid %d\n", (int)static_cast<bool>(t));

    // MOVE ASSIGNMENT: never instantiated either, and it is the one that must
    // free the destination's existing tree first. A version that forgot would
    // leak, which the leak gate then catches.
    axl::radix_tree<Route> sink;
    sink.insert("/doomed", &r1);
    sink = std::move(moved);
    axl_printf("radix: moveassign %d %zu %d\n",
               (int)sink.valid(), sink.size(), (int)moved.valid());
    // Self-move must be safe -- the `this != &other` guard is the only thing
    // between it and freeing the tree it is about to adopt. gcc warns on the
    // direct spelling, which is right everywhere except here, where it is the
    // behaviour under test.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
    sink = std::move(sink);
#pragma GCC diagnostic pop
    axl_printf("radix: selfmove %d %zu\n", (int)sink.valid(), sink.size());

    // release() hands the handle back; the object goes invalid and must not
    // free it at scope exit.
    AxlRadixTree *raw = sink.release();
    axl_printf("radix: release %d %d %zu\n",
               (int)(raw != nullptr), (int)sink.valid(),
               axl_radix_tree_size(raw));
    axl_radix_tree_free(raw);                      // ours now

    // lookup_prefix leaves *suffix UNTOUCHED when nothing matches -- a
    // documented promise that `radix: nomatch` could not see, because it
    // passed no suffix pointer.
    axl::radix_tree<Route> pt;
    pt.insert("/api/", &r1);
    const char *sentinel = "UNTOUCHED";
    const char *probe = sentinel;
    axl_printf("radix: suffixkept %d %d\n",
               (int)(pt.lookup_prefix("/zzz", &probe) == nullptr),
               (int)(probe == sentinel));

    // A NULL value is a real entry. Before the C-side fix this key was
    // counted by size() and reachable by nothing -- remove() refused it and
    // for_each() skipped it, so it could never leave the tree.
    axl::radix_tree<Route> nt;
    nt.insert("/null", nullptr);
    nt.insert("/null", nullptr);              // must NOT count twice
    route_visits = 0;
    nt.for_each(dump_route);                   // visits it; adds 0 to the sum
    // Sequenced, for the reason recorded above radix: remove -- gcc evaluates
    // arguments right-to-left, so an inline nt.remove() here ran BEFORE
    // nt.size() and reported the post-removal size. Third time in this file.
    const size_t null_size    = nt.size();
    const bool   null_removed = nt.remove("/null");
    axl_printf("radix: nullvalue %zu %d %d\n", null_size, route_visits,
               (int)null_removed);
    axl_printf("radix: nullgone %zu\n", nt.size());

    // An OWNING tree runs the destructor on every value it still holds.
    route_frees = 0;
    {
        axl::radix_tree<Route> owner{free_route};
        owner.insert("/a", static_cast<Route *>(axl_malloc(sizeof(Route))));
        owner.insert("/b", static_cast<Route *>(axl_malloc(sizeof(Route))));
        owner.remove("/a");                       // frees one now
        axl_printf("radix: ownremove %d %zu\n", route_frees, owner.size());

        // REPLACING a key frees the old value -- documented on insert(), and
        // the leak gate is what would notice if it did not.
        owner.insert("/b", static_cast<Route *>(axl_malloc(sizeof(Route))));
        axl_printf("radix: ownreplace %d %zu\n", route_frees, owner.size());
    }                                             // frees the survivor here
    axl_printf("radix: owned %d\n", route_frees);

    axl_printf("radix: done\n");
}

// ---------------------------------------------------------------------------
// C5 -- the draw-target scope guard
// ---------------------------------------------------------------------------

static void
verb_gfx(void)
{
    AxlGfxBuffer *outer = axl_gfx_buffer_new(8, 8);
    AxlGfxBuffer *inner = axl_gfx_buffer_new(4, 4);
    axl_printf("gfx: buffers %d\n", (int)(outer != nullptr && inner != nullptr));

    // Start from the screen target, which is what NULL means.
    axl_gfx_target_buffer(nullptr);
    axl_printf("gfx: base %d\n", (int)(axl_gfx_get_current_target() == nullptr));

    {
        axl::gfx_target_scope o{outer};
        axl_printf("gfx: outer %d %d\n",
                   (int)(axl_gfx_get_current_target() == outer),
                   (int)(o.saved() == nullptr));
        {
            axl::gfx_target_scope i{inner};
            axl_printf("gfx: inner %d %d\n",
                       (int)(axl_gfx_get_current_target() == inner),
                       (int)(i.saved() == outer));
        }
        // THE assertion: the inner scope restored OUTER, not NULL. A guard
        // that reset to "no target" would put the rest of this scope's
        // drawing on the screen instead of into outer's buffer.
        axl_printf("gfx: restored %d\n",
                   (int)(axl_gfx_get_current_target() == outer));
    }
    axl_printf("gfx: unwound %d\n",
               (int)(axl_gfx_get_current_target() == nullptr));

    // A NULL argument is legitimate and means the screen -- the headless path.
    {
        axl::gfx_target_scope o{outer};
        {
            axl::gfx_target_scope screen{nullptr};
            axl_printf("gfx: toscreen %d %d\n",
                       (int)(axl_gfx_get_current_target() == nullptr),
                       (int)(screen.saved() == outer));
        }
        axl_printf("gfx: backtobuf %d\n",
                   (int)(axl_gfx_get_current_target() == outer));
    }

    axl_gfx_buffer_free(inner);
    axl_gfx_buffer_free(outer);
    axl_printf("gfx: done\n");
}

// ---------------------------------------------------------------------------
// C3 -- the halt. Deliberately wrong, and must not return.
// ---------------------------------------------------------------------------

static void
free_array_at_exit(void *p) AXL_CB_NOEXCEPT
{
    axl_array_free(static_cast<AxlArray *>(p));
}

static void
verb_mismatch(void)
{
    AxlArray *a = axl_array_new(sizeof(int64_t));
    int64_t v = 1;
    axl_array_append(a, &v);

    // The halt below never returns, so the axl_array_free at the end of this
    // function cannot run -- and AXL_AUTOPTR would not save it either, since
    // abort() does not unwind. Without this the fixture would leak its own
    // array every run, and a leak in a test is a test bug rather than a
    // reason to widen the harness's grep. axl_exit drains atexit before the
    // teardown leak report, which is what makes this work.
    axl_atexit(free_array_at_exit, a);

    axl_printf("mismatch: about to read an int64_t array as a 40-byte struct\n");

    struct Wide { char pad[40]; };
    std::span<Wide> bad = axl::array_span<Wide>(a);

    // Never reached. Printing bad.size() keeps the compiler from eliding the
    // call entirely, which would make the halt come from nowhere.
    axl_printf("mismatch: UNREACHABLE %zu\n", bad.size());
}

int
main(int argc, char **argv)
{
    const char *verb = argc > 1 ? argv[1] : "cstr";

    if (axl_streql(verb, "cstr")) {
        verb_cstr();
    } else if (axl_streql(verb, "array")) {
        verb_array();
    } else if (axl_streql(verb, "ntree")) {
        verb_ntree();
    } else if (axl_streql(verb, "radix")) {
        verb_radix();
    } else if (axl_streql(verb, "gfx")) {
        verb_gfx();
    } else if (axl_streql(verb, "mismatch")) {
        verb_mismatch();
    } else {
        axl_printf("unknown verb: %s\n", verb);
        return 1;
    }
    return 0;
}
