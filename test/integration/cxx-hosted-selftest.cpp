/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cxx-hosted-selftest.cpp
    The standard containers, running under UEFI.

    Drives `std::vector`, `std::string`, `std::map` and
    `std::unordered_map` through the operations that actually need
    out-of-line code from `libstdc++.a`, then prints EXACT lines the
    harness matches with `grep -Fx`. Substring matching would let a
    regression through, which is the whole reason the strings are
    fixed rather than descriptive.

    This TU is compiled by `axl-c++ --hosted`. Without that flag
    libstdc++ refuses the containers outright at
    `bits/requires_hosted.h`, and test-cxx-hosted-qemu.sh asserts that
    refusal too — the flag has to be what lifts the gate, not a
    coincidence of include paths.

    The `unordered_map` half is not decoration. Its load-factor math
    is the one place libstdc++ does floating point, so on a host whose
    gcc baseline is above `x86-64` it is the member that arrives
    carrying AVX — and UEFI boots with `CR4.OSXSAVE` clear. See
    `src/runtime/axl-cxx-rehash.cpp`.
**/

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include <axl.h>
#include <axl/axl-arena-allocator.hpp>


/* ---------------------------------------------------------------------------
 * Red-black verification for the std::_Rb_tree_* helpers in
 * src/runtime/axl-cxx-rbtree.cpp, which replaced libstdc++'s tree.o so that a
 * hosted image needs no libstdc++.a at all.
 *
 * A differential test alone is not enough here. std::map answers every query
 * correctly even when the tree has degenerated into a linked list -- the
 * result is right and the complexity is gone, silently. So this reaches the
 * actual node graph through _Rb_tree_iterator::_M_node (a public member) and
 * checks the STRUCTURE: colours, parent links, and a uniform black height.
 * ------------------------------------------------------------------------ */

typedef std::_Rb_tree_node_base RbBase;

static int
rb_black_height(const RbBase *n, bool *ok)
{
    if (n == nullptr) {
        return 1;                       /* NULL leaves are black */
    }
    if (n->_M_left != nullptr && n->_M_left->_M_parent != n) {
        *ok = false;
        return -1;
    }
    if (n->_M_right != nullptr && n->_M_right->_M_parent != n) {
        *ok = false;
        return -1;
    }
    if (n->_M_color == std::_S_red &&
        ((n->_M_left  != nullptr && n->_M_left->_M_color  == std::_S_red) ||
         (n->_M_right != nullptr && n->_M_right->_M_color == std::_S_red))) {
        *ok = false;
        return -1;
    }
    int l = rb_black_height(n->_M_left, ok);
    int r = rb_black_height(n->_M_right, ok);
    if (!*ok || l != r) {
        *ok = false;
        return -1;
    }
    return l + (n->_M_color == std::_S_black ? 1 : 0);
}

/* Verify the tree behind @a m, and hand back its black height. */
template <class Map>
static bool
rb_verify(const Map &m, int *out_bh)
{
    *out_bh = 0;
    if (m.empty()) {
        return true;
    }

    /* Walk up from any node to the header: the header is the only node whose
       grandparent is itself. Its _M_parent is the root. */
    const RbBase *n = m.begin()._M_node;
    while (!(n->_M_color == std::_S_red && n->_M_parent->_M_parent == n)) {
        n = n->_M_parent;
    }
    const RbBase *header = n;
    const RbBase *root   = header->_M_parent;

    if (root->_M_color != std::_S_black) {
        return false;
    }
    /* The cached ends must still be the real extremes -- begin() IS
       _M_header._M_left, so a stale cache is a wrong iterator, not a crash. */
    const RbBase *lm = root; while (lm->_M_left  != nullptr) { lm = lm->_M_left;  }
    const RbBase *rm = root; while (rm->_M_right != nullptr) { rm = rm->_M_right; }
    if (header->_M_left != lm || header->_M_right != rm) {
        return false;
    }

    bool ok = true;
    int  bh = rb_black_height(root, &ok);
    if (!ok) {
        return false;
    }
    *out_bh = bh;
    return true;
}

static void
rb_differential(void)
{
    const int    N   = 1200;
    std::map<int, int> m;
    /* Reference: a sorted vector of the keys currently present. */
    std::vector<int> ref;

    uint32_t lcg   = 2463534242u;
    bool     ok    = true;
    bool     shape = true;
    int      bh    = 0;

    for (int step = 0; step < N && ok && shape; step++) {
        lcg ^= lcg << 13; lcg ^= lcg >> 17; lcg ^= lcg << 5;   /* xorshift32 */
        int key = (int)(lcg % 500u);

        bool erase = (lcg & 0x10000u) != 0 && !ref.empty();
        if (erase) {
            size_t at = lcg % ref.size();
            int    k  = ref[at];
            m.erase(k);
            ref.erase(ref.begin() + (long)at);
        } else {
            if (m.insert({key, key * 3}).second) {
                std::vector<int>::iterator ins =
                    std::lower_bound(ref.begin(), ref.end(), key);
                ref.insert(ins, key);
            }
        }

        if (m.size() != ref.size()) { ok = false; break; }
        if (!rb_verify(m, &bh))     { shape = false; break; }
    }

    /* Full forward iteration must reproduce the sorted reference exactly. */
    bool order = (m.size() == ref.size());
    if (order) {
        size_t i = 0;
        for (std::map<int, int>::const_iterator it = m.begin(); it != m.end(); ++it) {
            if (it->first != ref[i] || it->second != ref[i] * 3) { order = false; break; }
            i++;
        }
    }

    /* Reverse iteration exercises _Rb_tree_decrement, which a forward-only
       test never touches at all -- including the decrement-from-end() case
       that depends on the header being red. */
    bool rorder = true;
    {
        size_t i = ref.size();
        for (std::map<int, int>::const_reverse_iterator it = m.rbegin();
             it != m.rend(); ++it) {
            if (i == 0) { rorder = false; break; }
            i--;
            if (it->first != ref[i]) { rorder = false; break; }
        }
        if (i != 0) { rorder = false; }
    }

    /* find() must agree on hits AND misses -- a tree that lost a subtree
       still iterates consistently with a reference built the same way. */
    bool lookup = true;
    for (int k = 0; k < 500 && lookup; k++) {
        bool want = std::binary_search(ref.begin(), ref.end(), k);
        bool got  = m.find(k) != m.end();
        if (want != got) { lookup = false; }
    }

    /* Balance: black height must stay inside the red-black bound. A tree
       degenerating toward a list breaks this long before it breaks an
       invariant. */
    bool bounded = (bh > 0 && bh <= 24);

    axl_printf("cxx: rb steps %s size %s shape %s order %s rev %s find %s bh %s\r\n",
               ok ? "ok" : "BAD", m.size() == ref.size() ? "ok" : "BAD",
               shape ? "ok" : "BAD", order ? "ok" : "BAD",
               rorder ? "ok" : "BAD", lookup ? "ok" : "BAD",
               bounded ? "ok" : "BAD");

    /* multimap and set go through the same helpers with duplicate keys and a
       different node payload; erase-by-iterator hits the two-child splice
       path that erase-by-key mostly avoids. */
    std::multimap<int, int> mm;
    for (int i = 0; i < 300; i++) { mm.insert({i % 40, i}); }
    int mmbh = 0;
    bool mmok = rb_verify(mm, &mmbh) && mm.size() == 300;
    for (std::multimap<int, int>::iterator it = mm.begin(); it != mm.end(); ) {
        it = (it->second % 3 == 0) ? mm.erase(it) : ++it;
    }
    mmok = mmok && rb_verify(mm, &mmbh);

    std::set<std::string> st;
    for (int i = 0; i < 200; i++) {
        char buf[32];
        axl_snprintf(buf, sizeof(buf), "key-%03d", (i * 37) % 200);
        st.insert(std::string(buf));
    }
    int stbh = 0;
    bool stok = rb_verify(st, &stbh) && st.size() == 200;
    st.erase("key-000");
    st.erase("key-199");
    stok = stok && rb_verify(st, &stbh) && st.size() == 198;

    axl_printf("cxx: rb multi %s set %s\r\n",
               mmok ? "ok" : "BAD", stok ? "ok" : "BAD");
}


/* ---------------------------------------------------------------------------
 * std::list and shared_ptr, which needed libstdc++'s list.o and
 * shared_ptr.o -- the two members whose landing pads dragged in the
 * _Unwind_* cascade that AXL-Cxx-Stdlib-Surface.md called "tier 2".
 *
 * Measured: our own -fno-exceptions objects reference ZERO _Unwind_* symbols.
 * The cascade was a consequence of sourcing five functions from a member
 * compiled WITH exceptions, not a prerequisite of std::list.
 *
 * A list is a RING through the sentinel header, so "sorted" and "intact" are
 * different claims: a broken _M_transfer can leave a list that iterates
 * forward correctly and is corrupt backwards. Both directions are walked.
 * ------------------------------------------------------------------------ */

static bool
ring_intact(const std::list<int> &l)
{
    /* RECIPROCITY, not just length. An earlier version of this walked with
       iterators and compared counts, and its comment claimed to check links
       -- it did not. A same-length permutation of the _M_prev chain passed
       it: rend() is reverse_iterator(begin()), so the reverse walk stops the
       moment it reaches begin() and can NEVER observe begin()->_M_prev, the
       one link no public walk touches.
       So go through the nodes, which _List_iterator exposes, and assert
       n->_M_next->_M_prev == n around the whole ring INCLUDING the
       sentinel. */
    const std::__detail::_List_node_base *head = l.end()._M_node;
    const std::__detail::_List_node_base *cur  = head;
    size_t                                n    = 0;

    do {
        if (cur->_M_next == nullptr || cur->_M_prev == nullptr) { return false; }
        if (cur->_M_next->_M_prev != cur)                       { return false; }
        if (cur->_M_prev->_M_next != cur)                       { return false; }
        cur = cur->_M_next;
        if (++n > l.size() + 2) { return false; }   /* runaway ring */
    } while (cur != head);

    /* n counted the sentinel too. */
    return n == l.size() + 1;
}

static void
list_and_shared(void)
{
    const int N = 600;
    std::list<int>   l;
    std::vector<int> ref;
    uint32_t lcg   = 20260809u;
    bool     ok    = true;
    bool     shape = true;

    for (int step = 0; step < N && ok && shape; step++) {
        lcg ^= lcg << 13; lcg ^= lcg >> 17; lcg ^= lcg << 5;
        int key = (int)(lcg % 400u);

        if ((lcg & 0x10000u) != 0 && !ref.empty()) {
            size_t at = lcg % ref.size();
            std::list<int>::iterator it = l.begin();
            for (size_t i = 0; i < at; i++) { ++it; }
            l.erase(it);
            ref.erase(ref.begin() + (long)at);
        } else {
            l.push_back(key);
            ref.push_back(key);
        }
        if (l.size() != ref.size()) { ok = false; break; }
        if (!ring_intact(l))        { shape = false; break; }
    }

    /* Insertion order preserved -- a list is not sorted by construction. */
    bool order = (l.size() == ref.size());
    if (order) {
        size_t i = 0;
        for (std::list<int>::const_iterator it = l.begin(); it != l.end(); ++it) {
            if (*it != ref[i++]) { order = false; break; }
        }
    }

    /* sort() drives _M_transfer hard: it is a merge sort that splices runs. */
    l.sort();
    std::sort(ref.begin(), ref.end());
    bool sorted = (l.size() == ref.size()) && ring_intact(l);
    if (sorted) {
        size_t i = 0;
        for (std::list<int>::const_iterator it = l.begin(); it != l.end(); ++it) {
            if (*it != ref[i++]) { sorted = false; break; }
        }
    }

    /* reverse() is _M_reverse, which nothing above reaches. */
    l.reverse();
    bool reversed = ring_intact(l) && l.size() == ref.size();
    if (reversed) {
        size_t i = ref.size();
        for (std::list<int>::const_iterator it = l.begin(); it != l.end(); ++it) {
            if (*it != ref[--i]) { reversed = false; break; }
        }
    }

    /* splice() is _M_transfer between DIFFERENT lists; swap() is the
       static member, and swapping a non-empty with an empty list is the
       branch a same-size swap never takes. */
    std::list<int> a{1, 2, 3};
    std::list<int> b{7, 8};
    a.splice(a.begin(), b);
    int  ae[] = {7, 8, 1, 2, 3};
    bool spliced = (a.size() == 5 && b.size() == 0 &&
                    ring_intact(a) && ring_intact(b));
    if (spliced) {
        size_t i = 0;
        for (std::list<int>::const_iterator it = a.begin(); it != a.end(); ++it) {
            if (*it != ae[i++]) { spliced = false; break; }   /* middle too */
        }
    }
    /* SELF-splice: position == last, which _M_transfer must treat as a
       no-op. Legal because end() is not inside [begin(), end()). Nothing
       else here reaches it -- dropping the guard passed every other
       assertion in this function. */
    std::list<int> self_l{4, 5, 6};
    self_l.splice(self_l.end(), self_l, self_l.begin(), self_l.end());
    bool selfspliced = (self_l.size() == 3 && ring_intact(self_l) &&
                        self_l.front() == 4 && self_l.back() == 6);
    /* And the other self-splice shape: a single element onto its own
       position, which libstdc++ short-circuits one level up but which
       still reaches _M_transfer for the general range form. */
    std::list<int> one_l{9};
    one_l.splice(one_l.end(), one_l, one_l.begin(), one_l.end());
    selfspliced = selfspliced && one_l.size() == 1 && ring_intact(one_l) &&
                  one_l.front() == 9;

    /* BOTH-POPULATED swap -- the largest branch in the file, and nothing
       reached it. Every swap here and inside sort() has one side empty, so
       five separate sabotages of the eight-write branch all passed.
       push_FRONT afterwards is load-bearing, not decoration: it is the only
       public operation that READS begin()->_M_prev, the link no forward or
       reverse walk can see. With push_back instead, dropping x's sentinel
       fixup still leaves both lists answering every query correctly. */
    std::list<int> p1{1, 2, 3};
    std::list<int> p2{9, 8};
    p1.swap(p2);
    p1.push_front(0); p2.push_front(0);
    p1.push_back(5);  p2.push_back(5);
    int  p1e[] = {0, 9, 8, 5};
    int  p2e[] = {0, 1, 2, 3, 5};
    bool bothswap = (p1.size() == 4 && p2.size() == 5 &&
                     ring_intact(p1) && ring_intact(p2));
    if (bothswap) {
        size_t i = 0;
        for (std::list<int>::const_iterator it = p1.begin(); it != p1.end(); ++it) {
            if (*it != p1e[i++]) { bothswap = false; break; }
        }
        i = 0;
        for (std::list<int>::const_iterator it = p2.begin(); it != p2.end(); ++it) {
            if (*it != p2e[i++]) { bothswap = false; break; }
        }
    }

    /* merge() drives the PUBLIC interleaving _M_transfer shape, which
       sort()'s scratch-list merge does not. And reverse() on an EMPTY list
       is the terminating case the docstring calls out but nothing ran. */
    std::list<int> m1{1, 4, 9};
    std::list<int> m2{2, 3, 10};
    m1.merge(m2);
    int  me[] = {1, 2, 3, 4, 9, 10};
    bool merged = (m1.size() == 6 && m2.size() == 0 &&
                   ring_intact(m1) && ring_intact(m2));
    if (merged) {
        size_t i = 0;
        for (std::list<int>::const_iterator it = m1.begin(); it != m1.end(); ++it) {
            if (*it != me[i++]) { merged = false; break; }
        }
    }
    std::list<int> er;
    er.reverse();
    merged = merged && er.empty() && ring_intact(er);

    std::list<int> empty_l;
    a.swap(empty_l);
    bool swapped = (a.size() == 0 && empty_l.size() == 5 &&
                    ring_intact(a) && ring_intact(empty_l) &&
                    empty_l.front() == 7);

    axl_printf("cxx: list steps %s size %s ring %s order %s sort %s rev %s "
               "splice %s selfsplice %s swap %s both %s merge %s\r\n",
               ok ? "ok" : "BAD", l.size() == ref.size() ? "ok" : "BAD",
               shape ? "ok" : "BAD", order ? "ok" : "BAD",
               sorted ? "ok" : "BAD", reversed ? "ok" : "BAD",
               spliced ? "ok" : "BAD", selfspliced ? "ok" : "BAD",
               swapped ? "ok" : "BAD", bothswap ? "ok" : "BAD",
               merged ? "ok" : "BAD");

    /* shared_ptr. make_shared's single-allocation control block is what
       reaches _Sp_make_shared_tag::_S_eq under -fno-rtti. */
    std::shared_ptr<int> sp = std::make_shared<int>(41);
    *sp += 1;
    std::shared_ptr<int> sp2 = sp;
    std::weak_ptr<int>   wp  = sp;
    bool shared = (*sp == 42 && sp.use_count() == 2 && !wp.expired());
    sp2.reset();
    shared = shared && sp.use_count() == 1;
    {
        std::shared_ptr<int> locked = wp.lock();
        shared = shared && locked && *locked == 42;
    }
    /* A separately-allocated control block takes the OTHER branch. */
    std::shared_ptr<int> plain(new int(9));
    shared = shared && *plain == 9 && plain.use_count() == 1;
    sp.reset();
    shared = shared && wp.expired();

    axl_printf("cxx: shared %s\r\n", shared ? "ok" : "BAD");
}

int
main(void)
{
    /* vector + a standard algorithm over it. */
    std::vector<int> v{5, 3, 9, 1};
    std::sort(v.begin(), v.end());
    axl_print("cxx: vec");
    for (int x : v) {
        axl_printf(" %d", x);
    }
    axl_print("\r\n");

    /* string: `+=` and `append` are the operations that need
     * out-of-line code (and reach __throw_length_error on overflow). */
    std::string s = "axl";
    s += "-";
    s.append("sdk");
    axl_printf("cxx: str %s len=%u\r\n", s.c_str(), (unsigned) s.size());

    /* map: ordered iteration, proving the red-black tree core linked. */
    std::map<std::string, int> m{{"pear", 3}, {"apple", 1}, {"fig", 2}};
    axl_printf("cxx: map %u", (unsigned) m.size());
    for (const auto &kv : m) {
        axl_printf(" %s=%d", kv.first.c_str(), kv.second);
    }
    axl_print("\r\n");

    /* unordered_map: 200 inserts force several rehashes through
     * _M_need_rehash. Assert on looked-up VALUES, never on iteration
     * order — the order is unspecified and pinning it would make this
     * test fail for a reason that is not a defect. */
    std::unordered_map<int, int> u;
    for (int i = 0; i < 200; i++) {
        u[i] = i * i;
    }
    axl_printf("cxx: umap %u sq13=%d sq199=%d\r\n",
               (unsigned) u.size(), u[13], u[199]);

    /* Values alone do NOT test _M_need_rehash: a policy that answers
     * "never grow" leaves every key findable down one enormous chain,
     * so size and lookups stay right while the container quietly goes
     * quadratic. What it actually promises is
     * `load_factor() <= max_load_factor()`, so assert THAT -- and the
     * bucket growth that backs it. */
    axl_printf("cxx: umap grow bkt>=200 %s lf<=mlf %s\r\n",
               u.bucket_count() >= 200 ? "ok" : "BAD",
               u.load_factor() <= u.max_load_factor() ? "ok" : "BAD");

    /* reserve() reaches _M_next_bkt, the OTHER out-of-line member of
     * _Prime_rehash_policy. A build that supplies one and not the
     * other links, then disagrees with itself about _M_next_resize. */
    u.reserve(500);
    axl_printf("cxx: umap reserve bkt>=500 %s\r\n",
               u.bucket_count() >= 500 ? "ok" : "BAD");

    /* Lower the load factor and grow again: a different threshold
     * through the same policy, and every prior key must survive. */
    u.max_load_factor(0.5f);
    for (int i = 200; i < 400; i++) {
        u[i] = i * i;
    }
    bool intact = u.size() == 400;
    for (int i = 0; intact && i < 400; i++) {
        intact = u[i] == i * i;
    }
    axl_printf("cxx: umap mlf %u %s\r\n", (unsigned) u.size(),
               intact ? "intact" : "LOST");
    /* 400 elements at a 0.5 load factor is 800 buckets, minimum. A
     * policy that ignores max_load_factor passes every check above
     * this one. */
    axl_printf("cxx: umap mlf bkt>=800 %s lf<=mlf %s\r\n",
               u.bucket_count() >= 800 ? "ok" : "BAD",
               u.load_factor() <= u.max_load_factor() ? "ok" : "BAD");

    /* Erase then re-insert: the policy must not wedge after a shrink. */
    for (int i = 0; i < 300; i++) {
        u.erase(i);
    }
    u.rehash(64);
    for (int i = 0; i < 300; i++) {
        u[i] = i + 1;
    }
    axl_printf("cxx: umap churn %u u[7]=%d\r\n",
               (unsigned) u.size(), u[7]);

    /* ---------------------------------------------------------------
     * axl::arena_allocator — the same containers, off an arena.
     * ------------------------------------------------------------- */
    AxlArena *arena = axl_arena_new(256 * 1024);
    if (arena == NULL) {
        axl_print("cxx: arena create FAILED\r\n");
        return 1;
    }
    const size_t arena_start = axl_arena_remaining(arena);

    using IntVec = std::vector<int, axl::arena_allocator<int>>;
    {
        IntVec av{axl::arena_allocator<int>(arena)};
        av.reserve(1000);
        const size_t after_reserve = axl_arena_remaining(arena);
        for (int i = 0; i < 1000; i++) {
            av.push_back(i * 2);
        }
        axl_printf("cxx: arena vec %u av[999]=%d\r\n",
                   (unsigned) av.size(), av[999]);
        /* "drew" proves the bytes came from the ARENA and not the
         * heap -- without it this whole section passes identically
         * with a default allocator. "regrew" proves reserve() really
         * did prevent the growth reallocations that an arena never
         * gives back. */
        axl_printf("cxx: arena drew %s regrew %s\r\n",
                   (arena_start - after_reserve) >= 1000 * sizeof(int)
                       ? "ok" : "BAD",
                   axl_arena_remaining(arena) == after_reserve ? "no" : "YES");
    }

    /* axl_arena_alloc promises 8-byte alignment; this type needs 32.
     * Under-aligning is silent on one arch and a fault on another.
     *
     * Swept over all four 8-byte offsets mod 32, because ONE pass
     * proves nothing: with the allocator's padding disabled entirely
     * this check still passed, purely because the arena's bump
     * pointer happened to be 32-aligned at that moment. */
    {
        struct alignas(32) Wide {
            double v[4];
        };
        bool aligned = true;
        for (unsigned off = 0; off < 4; off++) {
            /* Walk the bump pointer so the NEXT allocation starts at
             * offset `off * 8` mod 32. Bounded, so a NULL from an
             * exhausted arena cannot spin. */
            for (int spin = 0; spin < 8; spin++) {
                void *probe = axl_arena_alloc(arena, 8);
                if (probe == NULL || ((uintptr_t) probe + 8) % 32 == off * 8) {
                    break;
                }
            }
            std::vector<Wide, axl::arena_allocator<Wide>>
                wv{axl::arena_allocator<Wide>(arena)};
            wv.resize(4);
            aligned = aligned && ((uintptr_t) wv.data() % 32) == 0;
        }
        axl_printf("cxx: arena align32 %s\r\n", aligned ? "ok" : "BAD");
    }

    /* A node-based container, which allocates through the REBINDING
     * constructor rather than the one the caller wrote. */
    {
        using ArenaMap = std::map<int, int, std::less<int>,
                                  axl::arena_allocator<std::pair<const int, int>>>;
        ArenaMap am{axl::arena_allocator<std::pair<const int, int>>(arena)};
        for (int i = 0; i < 50; i++) {
            am[i] = i * 3;
        }
        axl_printf("cxx: arena map %u am[17]=%d\r\n",
                   (unsigned) am.size(), am[17]);
    }

    /* Swapping containers whose allocators compare UNEQUAL is
     * undefined behaviour unless the allocator propagates on swap,
     * and two different arenas is the ordinary case here.
     *
     * The ELEMENTS after the swap do not test that. libstdc++ swaps
     * the buffer pointers either way, so `a[0] == 22` holds even with
     * propagate_on_container_swap set to false_type -- verified by
     * sabotage, which is the only reason this comment is not simply
     * wrong. What discriminates is where each container now thinks it
     * allocates FROM: without propagation `a` keeps arena1 while
     * holding a buffer that lives in arena2, and that inconsistency
     * is precisely what makes it UB. */
    AxlArena *arena2 = axl_arena_new(16 * 1024);
    if (arena2 == NULL) {
        axl_print("cxx: arena2 create FAILED\r\n");
        return 1;
    }
    {
        IntVec a{axl::arena_allocator<int>(arena)};
        IntVec b{axl::arena_allocator<int>(arena2)};
        a.push_back(11);
        b.push_back(22);
        a.swap(b);
        const bool followed = a.get_allocator().arena() == arena2
                              && b.get_allocator().arena() == arena;
        axl_printf("cxx: arena swap %d %d alloc %s\r\n",
                   a[0], b[0], followed ? "followed" : "STRANDED");
    }
    axl_arena_free(arena2);

    /* The point of the whole allocator: exhaustion is a condition the
     * caller can SEE before committing, instead of a halt inside a
     * container. */
    {
        const size_t huge = IntVec::allocator_type::bytes_for(1000000);
        axl_printf("cxx: arena guard toobig %s overflow %s\r\n",
                   axl_arena_remaining(arena) < huge ? "ok" : "BAD",
                   IntVec::allocator_type::bytes_for(SIZE_MAX) == SIZE_MAX
                       ? "ok" : "BAD");
    }

    /* The guard has to be SOUND, not approximately right: once
     * remaining() >= bytes_for(n), allocate(n) must succeed. It did
     * not, once -- bytes_for omitted the up-to-7 bytes the arena
     * spends rounding its own bump pointer to 8, so the check could
     * pass and the allocation still halt.
     *
     * Swept over all eight bump offsets, and at each one asked for the
     * largest n the guard itself claims fits. If bytes_for
     * under-reports, allocate() HALTS and this image never prints
     * "cxx: done" -- which the harness reads as a failure. */
    {
        using CharAlloc = axl::arena_allocator<char>;
        bool sound = true;
        for (unsigned burn = 0; burn < 8 && sound; burn++) {
            AxlArena *small = axl_arena_new(256);
            if (small == NULL) {
                sound = false;
                break;
            }
            if (burn > 0) {
                (void) axl_arena_alloc(small, burn);
            }
            size_t n = 1;
            while (axl_arena_remaining(small) >= CharAlloc::bytes_for(n + 1)) {
                n++;
            }
            CharAlloc a(small);
            sound = a.allocate(n) != nullptr;   /* halts if the guard lied */
            axl_arena_free(small);
        }
        axl_printf("cxx: arena guard sound %s\r\n", sound ? "ok" : "BAD");
    }

    /* allocate(0) is legal and must not halt, but axl_arena_alloc
     * treats a 0-byte request as an error and returns NULL. */
    {
        axl::arena_allocator<char> z(arena);
        char *zp = z.allocate(0);
        axl_printf("cxx: arena zero %s\r\n", zp != nullptr ? "ok" : "BAD");
        z.deallocate(zp, 0);
    }

    axl_arena_reset(arena);
    axl_printf("cxx: arena reset %s\r\n",
               axl_arena_remaining(arena) == arena_start ? "ok" : "BAD");
    axl_arena_free(arena);

    /* Over-aligned new. The compiler calls a DIFFERENT operator once
     * alignof(T) exceeds 16, and nothing defined it until this test
     * asked for one -- the link error names a mangled symbol and
     * never mentions alignment.
     *
     * Looped, because the aligned path stashes the original pointer
     * below the returned block for delete to recover: recovering the
     * wrong one frees a bogus pointer, and a single alloc/free pair
     * can survive that without visibly breaking. */
    {
        struct alignas(32)  A32  { char   c[1];   };
        struct alignas(64)  A64  { double d[8];   };
        struct alignas(128) A128 { char   c[200]; };
        bool ok = true;
        for (int i = 0; i < 64 && ok; i++) {
            A32  *p32  = new A32();
            A64  *p64  = new A64[3];
            A128 *p128 = new A128();
            ok = ((uintptr_t) p32 % 32) == 0
                 && ((uintptr_t) p64 % 64) == 0
                 && ((uintptr_t) p128 % 128) == 0;
            delete p128;
            delete[] p64;
            delete p32;
        }
        axl_printf("cxx: new overaligned %s\r\n", ok ? "ok" : "BAD");
    }

    /* The throwing operator new halts on failure (see
     * cxx-hosted-badalloc.cpp). `new (std::nothrow)` is the standard's
     * way to ask for a NULL instead, and it must still give one --
     * which needs the std::nothrow OBJECT, not just the overloads
     * that take it. */
    axl_mem_fail_next_alloc(1);
    int *nt = new (std::nothrow) int[4];
    axl_printf("cxx: nothrow %s\r\n", nt == nullptr ? "null" : "ALLOCATED");
    delete[] nt;

    /* The libm shim. x86-64 reaches `ceil` through the container
     * headers -- `_M_bkt_for_elements` rounds a load-factor quotient,
     * and rounding a double needs SSE4.1, above the -march=x86-64
     * baseline -- so on that arch this exercises
     * src/runtime/axl-cxx-libm.cpp. AArch64 folds it to `frintp` and
     * exercises the compiler instead; the values must agree either
     * way, which is the point.
     *
     * Asserted here rather than through bucket counts on purpose: the
     * policy rounds every request up to a prime, which absorbs a
     * one-unit error in ceil and hides it completely. Sabotaging
     * `ceil` to truncate left the whole rest of this fixture green.
     *
     * volatile so the arguments cannot be constant-folded away, which
     * would test the compiler's opinion instead of the running image. */
    volatile double c_int = 2.0, c_up = 2.1, c_neg = -2.1, c_frac = 0.25;
    axl_printf("cxx: ceil %d %d %d %d\r\n",
               (int) ceil(c_int), (int) ceil(c_up),
               (int) ceil(c_neg), (int) ceil(c_frac));

    rb_differential();
    list_and_shared();

    axl_print("cxx: done\r\n");
    return 0;
}
