/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cxx-streams-selftest.cpp
    Fixture for test-cxx-streams-qemu.sh: axl::cout / axl::cin / axl::cerr
    and axl::string, built with no mode flag and run under QEMU.

    Driven by a verb so startup.nsh can point different stdin at it:

      out      no stdin  -- axl::cout / axl::cerr / axl::endl formatting
      str      no stdin  -- the axl::string surface
      edge     no stdin  -- aliasing, overflow, OOM, and the operators
                            nothing else instantiates
      in       < in.txt  -- >> extraction, chaining, line crossing, hex
      getline  < lines   -- getline, including a final unterminated line
      fail     < bad.txt -- the failure model and cursor pushback
      pipe     | (UCS-2) -- the shell's default pipe must still parse

    Every line printed is asserted with grep -Fxq, so the text below IS the
    contract. Nothing here prints a value it did not compute.
**/

#include <stdint.h>

#include <axl/axl-istream.hpp>
#include <axl/axl-ostream.hpp>
#include <axl/axl-string.hpp>

static void
verb_out(void)
{
    axl::cout << "out: " << "text " << 42 << ' ' << -7 << ' ' << 3.5
              << ' ' << true << ' ' << false << axl::endl;

    axl::cout << "out: widths " << (unsigned int)4000000000u << ' '
              << (long long)-9000000000LL << ' '
              << (unsigned long long)18000000000ULL << axl::endl;

    /* A NULL const char * prints "(null)" rather than faulting -- axl_printf
       does the same, and a crash here would be a poor way to learn. */
    const char *nothing = nullptr;
    axl::cout << "out: null " << nothing << axl::endl;

    axl::string s("from-a-string");
    axl::cout << "out: " << s << axl::endl;

    /* endl must emit "\n", NOT "\r\n": the console stream translates LF to
       CRLF itself, so a CRLF here reaches the wire as \r\r\n. The harness
       strips a single \r per line, so a doubled one leaves a stray line. */
    axl::cout << "out: endl-once" << axl::endl;

    axl::cerr << "err: to stderr" << axl::endl;
}

static void
verb_str(void)
{
    axl::string s;
    axl::cout << "str: empty " << (int)s.size() << ' ' << s.empty()
              << " '" << s.c_str() << "'" << axl::endl;

    s = "hello";
    s += " world";
    s.push_back('!');
    axl::cout << "str: built " << s << ' ' << (int)s.size() << axl::endl;

    axl::cout << "str: find " << (int)s.find("world") << ' '
              << (int)s.find('z') << ' ' << s.starts_with("hello") << ' '
              << s.ends_with("!") << ' ' << s.contains("lo w") << axl::endl;

    axl::string sub = s.substr(6, 5);
    axl::cout << "str: substr " << sub << axl::endl;

    axl::string t = s;                 /* copy */
    t.erase(5, 6);                     /* "hello!" */
    axl::cout << "str: erase " << t << ' ' << (int)t.size() << axl::endl;

    t.insert(5, ",");
    axl::cout << "str: insert " << t << axl::endl;

    t.replace(0, 5, "HELLO");
    axl::cout << "str: replace " << t << axl::endl;

    axl::string m = static_cast<axl::string &&>(t);   /* move */
    axl::cout << "str: move " << m << ' ' << (int)t.size() << axl::endl;

    axl::string a("aaa");
    axl::string b("bbb");
    a.swap(b);
    axl::cout << "str: swap " << a << ' ' << b << axl::endl;

    axl::cout << "str: compare " << (a == axl::string("bbb")) << ' '
              << (a == "bbb") << ' ' << (a < b) << axl::endl;

    axl::string r(3, 'z');
    r.resize(5, 'q');
    axl::cout << "str: resize " << r << ' ' << (int)r.size() << axl::endl;
    r.resize(2);
    axl::cout << "str: shrink " << r << ' ' << (int)r.size() << axl::endl;

    /* Iteration: the container inherits every <algorithm> and <ranges> view
       just by supplying begin()/end(), which is the whole reason for them. */
    axl::string it("abc");
    int sum = 0;
    for (char c : it) {
        sum += c;
    }
    axl::cout << "str: iterate " << sum << ' ' << it.front() << it.back()
              << ' ' << it[1] << ' ' << it.at(2) << axl::endl;

    /* Capacity is observable and reserve() must not lose content. */
    axl::string cap("keep");
    cap.reserve(500);
    axl::cout << "str: reserve " << cap << ' ' << (cap.capacity() >= 500)
              << ' ' << (int)cap.size() << axl::endl;
    cap.shrink_to_fit();
    axl::cout << "str: shrink_to_fit " << cap << ' '
              << (int)cap.capacity() << ' ' << cap.is_small() << axl::endl;

    /* Self-append: the source IS the buffer the append is about to realloc. */
    axl::string self("0123456789");
    for (int i = 0; i < 4; i++) {
        self += self;
    }
    axl::cout << "str: self " << (int)self.size() << ' '
              << self.starts_with("0123456789") << ' '
              << self.ends_with("0123456789") << ' ' << self.bad() << axl::endl;

    /* A moved-from string is empty and usable, not poisoned. */
    axl::string src("moved");
    axl::string dst = static_cast<axl::string &&>(src);
    src = "reused";
    axl::cout << "str: moved-from " << dst << ' ' << src << axl::endl;

    /* steal() hands the buffer over; the string must stay usable after. */
    axl::string st("stolen");
    char       *raw = st.steal();
    st = "refilled";
    axl::cout << "str: steal " << raw << ' ' << st << axl::endl;
    axl_free(raw);
}

static void
verb_edge(void)
{
    /* A source pointing into the string's OWN buffer. assign() used to
       clear() first -- writing a NUL over byte 0 -- and then copy out of that
       same buffer, so "hello" became "\0ello" with bad() reading false. */
    axl::string a1("hello");
    a1 = a1.c_str();
    axl::string a2("hello");
    a2.assign(a2.view());
    axl::cout << "edge: selfassign " << a1 << ' ' << (int)a1.size() << ' '
              << a2 << ' ' << a1.bad() << axl::endl;

    /* replace() captured v.size() before the erase shifted the bytes out
       from under it, so the insert copied post-erase residue. */
    axl::string r("abcdef");
    r.replace(0, 2, r);
    axl::cout << "edge: selfreplace " << r << ' ' << (int)r.size() << axl::endl;

    /* size() + n wraps in C++ BEFORE grow()'s overflow guard can see it, and
       reaches axl_string_resize as a smaller length -- a silent truncation. */
    axl::string ov("abcdef");
    ov.append((size_t)-1 - 1, 'x');
    axl::cout << "edge: overflow " << ov << ' ' << (int)ov.size() << ' '
              << ov.bad() << axl::endl;

    /* operator+ on two strings was an AMBIGUITY error -- both string_view
       overloads were viable and neither won. Nothing called it, so it
       shipped uncompiled. */
    axl::string p("foo");
    axl::string q("bar");
    axl::cout << "edge: concat " << (p + q) << ' ' << (p + "!") << ' '
              << (p + '?') << ' ' << ('<' + p) << axl::endl;

    /* std::string guarantees data() == c_str() and begin() == cbegin(). With
       no buffer the const path returned the shared "" literal while the
       mutable path returned the per-object NUL. */
    axl::string e;
    axl::cout << "edge: ident " << (e.data() == e.c_str()) << ' '
              << (e.begin() == e.cbegin()) << ' '
              << (int)e.size() << axl::endl;

    /* Comparisons that can actually come out BOTH ways -- an operator<=>
       stuck at `equivalent` satisfied the earlier all-true assertion. */
    axl::cout << "edge: compare " << (p == q) << ' ' << (p < q) << ' '
              << (q < p) << ' ' << (p == "foo") << ' '
              << (p != q) << axl::endl;

    /* Both assignment operators, including the self-guards. */
    axl::string c1("one");
    axl::string c2("two");
    c1 = c2;
    axl::string m1("moved");
    axl::string m2("target");
    m2 = static_cast<axl::string &&>(m1);
    axl::cout << "edge: assign " << c1 << ' ' << m2 << ' '
              << (int)m1.size() << axl::endl;

    /* OOM is the headline divergence from std::string and had no positive
       coverage at all: the contract is that the string is left UNCHANGED and
       bad() is set, rather than the image halting. */
    /* Longer than sso_capacity ON PURPOSE: a short string never allocates
       now, so a short probe would report bad()==false and quietly stop
       testing OOM at all. */
    axl::string oom1;
    axl_mem_fail_next_alloc(1);
    oom1.append("longer-than-the-inline-buffer-so-this-must-allocate");
    axl::cout << "edge: oom-lazy '" << oom1 << "' " << oom1.bad() << axl::endl;

    axl::string oom2("keep");
    axl_mem_fail_next_alloc(1);
    oom2.append(400, 'x');
    axl::cout << "edge: oom-grow " << oom2 << ' ' << (int)oom2.size() << ' '
              << oom2.bad() << axl::endl;

    /* bad() is sticky until the value is replaced. */
    axl::cout << "edge: sticky " << oom2.bad();
    oom2.assign("fresh");
    axl::cout << ' ' << oom2.bad() << ' ' << oom2 << axl::endl;

    /* Hex OUTPUT. `axl::cout << axl::hex` used to bind operator<<(bool) via
       the function-to-pointer conversion and print "true". */
    axl::cout << "edge: hexout " << axl::hex << 255 << ' ' << 48879u
              << axl::dec << ' ' << 255 << axl::endl;

    axl::cout << "edge: nullptr " << nullptr << axl::endl;

    /* Members nothing else instantiates -- a template member that is never
       called is never fully compiled. */
    axl::string f("abcabc");
    axl::cout << "edge: search " << (int)f.rfind("abc") << ' '
              << (int)f.find_first_of("cb") << ' '
              << (int)f.find_last_of("ab") << ' '
              << (int)f.find_first_not_of("ab") << ' '
              << f.compare("abcabc") << axl::endl;

    axl::string rv("abc");
    axl::string rev;
    for (auto it = rv.rbegin(); it != rv.rend(); ++it) {
        rev.push_back(*it);
    }
    axl::cout << "edge: reverse " << rev << ' ' << (f.max_size() > 0) << ' '
              << rv.is_small() << axl::endl;

    /* pop_back on empty, substr at the boundary, operator[] at size(). */
    axl::string b1;
    b1.pop_back();
    axl::string b2("abc");
    axl::cout << "edge: bounds '" << b1 << "' '" << b2.substr(3) << "' "
              << (b2[3] == '\0') << ' ' << b2.substr(99).size() << axl::endl;

    /* SSO. The headline is allocation AVOIDANCE, so the assertion has to be
       that no allocation happens -- not merely that the value is right.
       axl_mem_fail_next_alloc(1) makes the very next allocation fail, so a
       short string that still constructs correctly provably took none. */
    axl_mem_fail_next_alloc(1);
    axl::string small("\\EFI\\BOOT\\BOOTX64.EFI");     /* 21 chars */
    axl_mem_fail_next_alloc(0);   /* DISARM -- see below */
    axl::cout << "edge: sso " << small << ' ' << (int)small.size() << ' '
              << small.is_small() << ' ' << small.bad() << ' '
              << (int)axl::string::sso_capacity << axl::endl;

    /* Copying a short string must not allocate either -- that is the 9.7x
       row in the measurement this refactor came from. */
    /* Every arm MUST be disarmed. An unconsumed axl_mem_fail_next_alloc stays
       live and is eaten by whatever allocates next -- which is how the
       ssogrow case below came to assert that the SSO->heap transition does
       NOT happen, and could not have failed if growth broke. */
    axl_mem_fail_next_alloc(1);
    axl::string smallcopy = small;
    axl_mem_fail_next_alloc(0);
    axl::cout << "edge: ssocopy " << smallcopy << ' ' << smallcopy.is_small()
              << ' ' << smallcopy.bad() << axl::endl;

    /* Copy-ASSIGNMENT must be allocation-free for a short source too. It was
       not: assign() appended before erasing unconditionally, so the peak
       length was old+new and a 21-byte assignment over a 21-byte string
       allocated. Only copy-CONSTRUCTION was covered, so this was invisible. */
    axl::string assign_dst("previous-value-here");
    axl_mem_fail_next_alloc(1);
    assign_dst = small;
    axl_mem_fail_next_alloc(0);
    axl::cout << "edge: ssoassign " << assign_dst << ' '
              << assign_dst.is_small() << ' ' << assign_dst.bad() << axl::endl;

    /* One byte past capacity moves to the heap, and shrink_to_fit brings it
       back -- so a string that was briefly long stops costing a block. */
    axl::string grow(axl::string::sso_capacity, 'x');
    bool was_small = grow.is_small();
    grow.push_back('y');
    bool now_heap = !grow.is_small();
    grow.resize(4);
    grow.shrink_to_fit();
    axl::cout << "edge: ssogrow " << was_small << ' ' << now_heap << ' '
              << grow.is_small() << ' ' << (int)grow.capacity() << ' '
              << grow << axl::endl;

    /* HEAP move, heap swap, heap steal, heap identity. Every move/swap/steal
       in this fixture used a SHORT string, so adopt()'s heap branch --
       `m_ptr = other.m_ptr; m_cap = other.m_cap;`, the one place the union
       write matters on a move -- was never executed. Dropping that m_cap
       assignment left the whole suite green. */
    axl::string big1(200, 'a');
    axl::string big2 = static_cast<axl::string &&>(big1);
    bool big_moved = (big2.size() == 200 && !big2.is_small() &&
                      big2.capacity() >= 200 && big1.size() == 0);
    axl::string big3(150, 'b');
    big2.swap(big3);
    bool big_swapped = (big2.size() == 150 && big3.size() == 200 &&
                        big2[0] == 'b' && big3[0] == 'a');
    char *bigraw = big3.steal();
    bool big_stolen = (bigraw != nullptr && bigraw[0] == 'a' &&
                       axl_strlen(bigraw) == 200 && big3.size() == 0 &&
                       big3.is_small());
    axl_free(bigraw);
    axl::cout << "edge: heapmove " << big_moved << ' ' << big_swapped << ' '
              << big_stolen << ' ' << (big2.data() == big2.c_str()) << axl::endl;

    /* shrink_to_fit's heap-STAYS-heap branch (realloc + m_cap = m_size).
       The existing case shrinks 500 -> 4, which takes the inline path. */
    axl::string sh(400, 'z');
    sh.reserve(4000);
    size_t sh_before = sh.capacity();
    sh.resize(300);
    sh.shrink_to_fit();
    axl::cout << "edge: shrinkheap " << (sh_before >= 4000) << ' '
              << (sh.capacity() == 300) << ' ' << (!sh.is_small()) << ' '
              << (int)sh.size() << ' ' << (sh[299] == 'z') << axl::endl;

    /* steal() from an inline string has no block to give away, so it must
       allocate one rather than hand back a pointer into the object. */
    axl::string st2("inline-bytes");
    char       *st2raw = st2.steal();
    axl::cout << "edge: ssosteal " << (st2raw != nullptr ? st2raw : "?") << ' '
              << (int)st2.size() << ' ' << st2.is_small() << axl::endl;
    axl_free(st2raw);

    axl::cout << "edge: float " << 1.5f << ' ' << (const void *)nullptr
              << axl::endl;
}

static void
verb_in(void)
{
    int32_t     a    = 0;
    axl::string word;
    double      d    = 0;
    bool        flag = false;
    uint32_t    h    = 0;

    axl::cin >> a >> word >> d;
    axl::cout << "in: first " << a << ' ' << word << ' ' << d << ' '
              << (bool)axl::cin << axl::endl;

    /* `true` is on the NEXT line: newline is ordinary whitespace, so a
       chained >> spans lines exactly as std::cin does. */
    axl::cin >> flag;
    axl::cout << "in: crossed " << flag << ' ' << (bool)axl::cin << axl::endl;

    axl::result<int32_t> r = axl::cin.read<int32_t>();
    axl::cout << "in: read " << r.has_value() << ' ' << (r ? *r : -1)
              << axl::endl;

    axl::cin >> axl::hex >> h;
    axl::cout << "in: hex " << (int)h << ' ' << (bool)axl::cin << axl::endl;
    axl::cin >> axl::dec;

    /* A bounded buffer: the capacity comes from the array type, so the
       overflow std::istream's operator>>(char *) allows cannot happen. */
    char ch = 0;
    axl::cin >> ch;
    axl::cout << "in: char " << ch << ' ' << (bool)axl::cin << axl::endl;

    char buf[4];
    axl::cin >> buf;
    axl::cout << "in: buf '" << buf << "' fail=" << axl::cin.fail()
              << " status=" << (int)axl::cin.status() << axl::endl;
    axl::cin.clear();

    axl::cout << "in: done" << axl::endl;
}

static void
verb_getline(void)
{
    axl::string line;

    axl::getline(axl::cin, line);
    axl::cout << "gl: 1 '" << line << "' " << (bool)axl::cin << axl::endl;

    axl::getline(axl::cin, line);
    axl::cout << "gl: 2 '" << line << "' " << (bool)axl::cin << axl::endl;

    /* An empty line yields an empty string and must NOT fail -- the
       delimiter was found, so something was read even though it was
       zero bytes long. */
    axl::getline(axl::cin, line);
    axl::cout << "gl: 3 '" << line << "' " << (bool)axl::cin << axl::endl;

    /* The final line has no trailing newline; it still succeeds. */
    axl::getline(axl::cin, line);
    axl::cout << "gl: 4 '" << line << "' " << (bool)axl::cin << axl::endl;

    /* Now the input really is exhausted. */
    axl::getline(axl::cin, line);
    axl::cout << "gl: 5 fail=" << axl::cin.fail() << " eof=" << axl::cin.eof()
              << " status=" << (int)axl::cin.status() << axl::endl;

    axl::cout << "gl: done" << axl::endl;
}

static void
verb_fail(void)
{
    int32_t v = 999;

    /* "notanumber" does not parse. The target must be UNTOUCHED and the
       cursor left AT the offending character, which is where std:: leaves
       it -- proven below by reading the same token back as text. */
    axl::cin >> v;
    axl::cout << "fail: parse " << axl::cin.fail() << ' ' << v << ' '
              << (int)axl::cin.status() << axl::endl;

    axl::cin.clear();
    axl::string back;
    axl::cin >> back;
    axl::cout << "fail: pushback " << back << axl::endl;

    /* A sticky failure short-circuits every later extraction, so one check
       after a chain is enough. */
    int32_t x = 111;
    int32_t y = 222;
    axl::cin >> x;              /* "alsobad" -> fails */
    axl::cin >> y;              /* skipped: already failed */
    axl::cout << "fail: sticky " << x << ' ' << y << ' '
              << axl::cin.fail() << axl::endl;

    /* read<T>() observes the SAME state, and reports why as a value. */
    axl::result<int32_t> r = axl::cin.read<int32_t>();
    axl::cout << "fail: read " << r.has_value() << ' '
              << (int)(r ? AXL_OK : r.error()) << axl::endl;

    axl::cin.clear();
    axl::cin >> x;              /* recovers: "alsobad" as a number still fails */
    axl::cout << "fail: recheck " << axl::cin.fail() << axl::endl;

    axl::cin.clear();
    axl::string rest;
    axl::cin >> rest;
    axl::cout << "fail: rest " << rest << axl::endl;

    /* Reading past the end reports NOT_FOUND, not INVALID. */
    axl::cin.clear();
    int32_t past = 0;
    axl::cin >> past;
    axl::cout << "fail: eof " << axl::cin.fail() << ' ' << axl::cin.eof()
              << ' ' << (int)axl::cin.status() << axl::endl;

    axl::cout << "fail: done" << axl::endl;
}

static void
verb_pipe(void)
{
    int32_t v = 0;
    axl::cin >> v;
    axl::cout << "pipe: " << v << ' ' << (bool)axl::cin << axl::endl;
}

int
main(int argc, char **argv)
{
    const char *verb = argc > 1 ? argv[1] : "out";

    if (axl_streql(verb, "out")) {
        verb_out();
    } else if (axl_streql(verb, "str")) {
        verb_str();
    } else if (axl_streql(verb, "edge")) {
        verb_edge();
    } else if (axl_streql(verb, "in")) {
        verb_in();
    } else if (axl_streql(verb, "getline")) {
        verb_getline();
    } else if (axl_streql(verb, "fail")) {
        verb_fail();
    } else if (axl_streql(verb, "pipe")) {
        verb_pipe();
    } else {
        axl::cerr << "unknown verb: " << verb << axl::endl;
        return 1;
    }
    return 0;
}
