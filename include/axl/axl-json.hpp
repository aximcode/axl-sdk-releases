/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-json.hpp
 *
 * JSON for C++: navigation that chains, containers that close themselves, and
 * `add` that picks the right emitter from the type you passed.
 *
 * @code
 * #include <axl/axl-json.hpp>
 *
 * // --- read ------------------------------------------------------------
 * auto doc = axl::json_document::parse(bytes);          // result<json_document>
 * if (!doc) { return doc.error(); }
 *
 * // ONE check for a three-level descent: a missing "net" propagates.
 * if (auto port = (*doc)["net"]["listen"]["port"].as<int64_t>()) {
 *     bind(*port);
 * }
 *
 * for (axl::json_value v : (*doc)["items"].array()) {
 *     handle(v["id"].as<int64_t>().value_or(-1));
 * }
 *
 * for (auto &&[key, value] : (*doc)["headers"].object()) {
 *     send(key.c_str(), value.as_string().value_or("").c_str());
 * }
 *
 * // --- write -----------------------------------------------------------
 * AXL_AUTOPTR(AxlString) out = axl_string_new("");
 * axl::json_writer w{out};
 * {
 *     auto o = w.object();                 // closes at the brace
 *     w.add("name", "axl");                // picks axl_json_kv_str
 *     w.add("port", 8080);                 // ...kv_int
 *     w.add("scale", 1.5);                 // ...kv_double
 *     {
 *         auto a = w.array("items");       // key + `[`, closes at the brace
 *         w.add(1);
 *         w.add(2);
 *     }
 *     w.splice("cached", (*doc)["items"]); // a parsed subtree, verbatim
 * }
 * w.finish();
 * @endcode
 *
 * @par Four faces, and this header covers all four
 *
 * `docs/AXL-JSON-Design.md` describes the C API as two engines behind four
 * faces — streaming and whole-document, in each direction. Each gets a C++
 * face here, and the bridge between them comes free:
 *
 * | | streaming | whole-document |
 * |---|---|---|
 * | **in** | #axl::json_scanner | #axl::json_document |
 * | **out** | #axl::json_writer | #axl::json_writer::splice() |
 *
 * `splice()` is `axl_json_write_token()`, and it needs no C change because a
 * sub-reader is REBASED — token 0 of `doc["items"]` is the array itself. So
 * "write this parsed subtree into that document" is one call.
 *
 * @par Errors are values, in BOTH compile modes
 *
 * Nothing here throws, and that is a stronger requirement than it used to be.
 * Exceptions genuinely work under UEFI — `axl-c++ -fexceptions` gives real
 * `try`/`catch`, pinned by `test-cxx-exceptions-qemu.sh` — but they are a
 * per-translation-unit opt-in and `-fno-exceptions` is the default. A header
 * that threw would be unusable in the default mode, so #axl::result is what
 * works in both. It also matches the C library, where errors are QUERIED
 * (JSON decision 16), so a C++ caller and a C caller distinguish the same
 * outcomes.
 *
 * @par Navigation CHAINS, so one check covers a whole descent
 *
 * #axl::json_value carries the reason it is empty. `operator[]` on a value
 * that is already errored returns that same error untouched, so
 * `doc["a"]["b"]["c"]` performs no lookups after the first failure and the
 * first `as_X()` reports what went wrong — #AXL_NOT_FOUND for an absent key,
 * #AXL_INVALID for "you indexed something that is not an object".
 *
 * This is simdjson's model. The alternative shapes were considered and each
 * loses something: `nlohmann::json::operator[]` DEFAULT-CONSTRUCTS a missing
 * key, mutating on read; RapidJSON asserts; Boost.JSON throws. Returning
 * `result<json_value>` at every step is honest but forces three unwraps for a
 * three-level lookup, which reads badly beside the C API it sits on.
 *
 * @par Lifetimes: the document owns, everything else borrows
 *
 * The C design's rule is "single owner, no reference counting" — a document
 * owns its tokens, and value handles are non-owning views valid for its
 * lifetime. That carries over exactly: #axl::json_document is move-only and
 * frees on destruction, and every #axl::json_value, range and iterator borrows
 * it. Outliving the document is undefined, the same way it is in C.
 *
 * The BYTES are a second question, and the two factories differ only in it.
 * #axl::json_document::parse() is zero-copy and borrows the caller's buffer,
 * matching `axl_json_parse()`; #axl::json_document::parse_owning() takes a
 * `std::string` by value and keeps it, for the very common C++ case where the
 * bytes were a temporary. Getting this wrong is silent, so the two are named
 * rather than overloaded.
 */

#ifndef AXL_JSON_HPP
#define AXL_JSON_HPP

#ifndef __cplusplus
#error "axl-json.hpp is C++ only; C consumers want axl-json.h"
#endif

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <axl/axl-cxx.hpp>
#include <axl/axl-json.h>
#include <axl/axl-string.h>

namespace axl {

class json_array_range;
class json_object_range;

/**
 * A borrowed view of one JSON value, carrying the reason it is empty.
 *
 * Copyable and cheap — it is a token cursor, not a document. Valid only while
 * the #axl::json_document it came from is.
 */
class json_value {
public:
    /// An errored value that names no failure yet; `exists()` is false.
    json_value() noexcept = default;

    /// Wrap a C sub-reader. The reader is COPIED (it is a value struct that
    /// borrows), so this does not alias the caller's variable.
    explicit json_value(const AxlJsonReader &r) noexcept
        : m_r(r), m_err(AXL_OK)
    {
        /* m_err MUST be set here. It defaults to AXL_NOT_FOUND so that a
           default-constructed value names a failure rather than reading as a
           valid empty document -- and leaving it to that initializer made
           every value built from a real reader born errored, which turned the
           whole API into one that finds nothing. */
    }

    /**
     * Descend to @a key.
     *
     * @return the value at @a key; an errored value carrying #AXL_NOT_FOUND if
     *     the key is absent, or #AXL_INVALID if this value is not an object.
     *     An already-errored value is returned UNCHANGED, which is what makes
     *     `v["a"]["b"]` safe to write without a check between the steps.
     */
    [[nodiscard]] json_value
    operator[](
        const char *key    ///< member name
    ) const noexcept
    {
        if (m_err != AXL_OK) {
            return *this;   /* propagate the FIRST failure, not the last */
        }
        AxlJsonReader sub;
        if (axl_json_get_value(&m_r, key, &sub)) {
            return json_value(sub);
        }
        /* Distinguish "no such key" from "this is not an object" — the C
           reader keeps them apart and collapsing them here would make a typo
           and a schema mismatch look identical. */
        return errored(axl_json_value_type(&m_r) == AXL_JSON_TYPE_OBJECT
                           ? AXL_NOT_FOUND : AXL_INVALID);
    }

    /**
     * Descend to @a key.
     *
     * The `std::string` overload, so a key held as one needs no `.c_str()`.
     * Identical in every other respect to the `const char *` form above,
     * including the chaining rule.
     */
    [[nodiscard]] json_value
    operator[](
        const std::string &key    ///< member name
    ) const noexcept
    {
        return (*this)[key.c_str()];
    }

    /// The JSON type, or #AXL_JSON_TYPE_NONE for an errored or empty value.
    [[nodiscard]] AxlJsonType type() const noexcept
    {
        return m_err == AXL_OK ? axl_json_value_type(&m_r)
                               : AXL_JSON_TYPE_NONE;
    }

    /// Why this value is empty; #AXL_OK when it is not.
    [[nodiscard]] AxlStatus error() const noexcept { return m_err; }

    /// Whether a value is actually here. A JSON `null` EXISTS — that is the
    /// distinction axl_json_get_value() preserves and this preserves with it.
    [[nodiscard]] bool exists() const noexcept
    {
        return m_err == AXL_OK && type() != AXL_JSON_TYPE_NONE;
    }

    /// @copydoc exists()
    explicit operator bool() const noexcept { return exists(); }

    [[nodiscard]] bool is_object() const noexcept { return type() == AXL_JSON_TYPE_OBJECT; }
    [[nodiscard]] bool is_array()  const noexcept { return type() == AXL_JSON_TYPE_ARRAY;  }
    [[nodiscard]] bool is_string() const noexcept { return type() == AXL_JSON_TYPE_STRING; }
    [[nodiscard]] bool is_number() const noexcept { return type() == AXL_JSON_TYPE_NUMBER; }
    [[nodiscard]] bool is_bool()   const noexcept { return type() == AXL_JSON_TYPE_BOOL;   }
    [[nodiscard]] bool is_null()   const noexcept { return type() == AXL_JSON_TYPE_NULL;   }

    /**
     * The value as a `std::string`, sized exactly.
     *
     * Two calls under the hood — axl_json_value_string_len() then
     * axl_json_value_string() — because the C accessor truncates silently and
     * cannot report the size it wanted. The length query decodes to measure,
     * so a string is decoded twice; that is the price of never truncating, and
     * it is why the C API keeps the truncating form as its default.
     *
     * @return the decoded value, or an error: this value's own if the chain
     *     failed, #AXL_INVALID if it is not a string.
     */
    [[nodiscard]] result<std::string>
    as_string() const
    {
        if (m_err != AXL_OK) { return err(m_err); }
        size_t n = 0;
        if (!axl_json_value_string_len(&m_r, &n)) { return err(AXL_INVALID); }
        std::string s;
        s.resize(n);
        /* n + 1: the accessor writes a NUL, and s.data()[n] is the string's
           own terminator slot -- writing '\\0' there is the one value the
           standard permits. */
        if (!axl_json_value_string(&m_r, s.data(), n + 1)) {
            return err(AXL_INVALID);
        }
        return s;
    }

    /// The value as a signed integer. #AXL_INVALID if it is not a number in
    /// range — the C reader REFUSES rather than wrapping, and so does this.
    [[nodiscard]] result<int64_t> as_int() const noexcept
    {
        return scalar<int64_t>(axl_json_value_int);
    }

    /// The value as an unsigned integer.
    [[nodiscard]] result<uint64_t> as_uint() const noexcept
    {
        return scalar<uint64_t>(axl_json_value_uint);
    }

    /// The value as a `double`. #AXL_INVALID if it cannot be represented
    /// exactly — refuse rather than round, matching the C accessor.
    [[nodiscard]] result<double> as_double() const noexcept
    {
        return scalar<double>(axl_json_value_double);
    }

    /// The value as a boolean.
    [[nodiscard]] result<bool> as_bool() const noexcept
    {
        return scalar<bool>(axl_json_value_bool);
    }

    /**
     * The value as @a T, chosen by type.
     *
     * `as<int64_t>()`, `as<double>()`, `as<bool>()`, `as<std::string>()`, and
     * any integral type that fits. A type with no mapping is a compile error
     * naming the ones that do, rather than a silent wrong overload.
     */
    template <class T>
    [[nodiscard]] result<T>
    as() const
    {
        if constexpr (std::is_same_v<T, std::string>) {
            return as_string();
        } else if constexpr (std::is_same_v<T, bool>) {
            return as_bool();
        } else if constexpr (std::is_floating_point_v<T>) {
            return as_double().transform([](double d) { return (T)d; });
        } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            return as_int().transform([](int64_t v) { return (T)v; });
        } else if constexpr (std::is_integral_v<T>) {
            return as_uint().transform([](uint64_t v) { return (T)v; });
        } else {
            static_assert(sizeof(T) == 0,
                          "axl::json_value::as<T> supports std::string, bool, "
                          "an integral type, or a floating-point type");
        }
    }

    /// Iterate this value as an array. Empty for anything that is not one.
    [[nodiscard]] json_array_range array() const noexcept;

    /// Iterate this value as an object, yielding `(std::string, json_value)`.
    /// Empty for anything that is not one.
    [[nodiscard]] json_object_range object() const noexcept;

    /**
     * The underlying C reader, for the calls this class does not wrap.
     *
     * Borrowed and owned by the document. Do not axl_json_free() it — on a
     * sub-reader that is a documented no-op, but the habit is wrong.
     */
    [[nodiscard]] const AxlJsonReader *reader() const noexcept { return &m_r; }

private:
    static json_value errored(AxlStatus s) noexcept
    {
        json_value v;
        v.m_err = s;
        return v;
    }

    /* Shared body for the four scalar accessors: each C twin has the same
       shape (bool return, value out-param, untouched on false), so writing it
       once is what keeps their error mapping identical. */
    template <class T, class Fn>
    result<T> scalar(Fn fn) const noexcept
    {
        if (m_err != AXL_OK) { return err(m_err); }
        T v{};
        if (!fn(&m_r, &v)) { return err(AXL_INVALID); }
        return v;
    }

    AxlJsonReader m_r{};
    /* AXL_NOT_FOUND, not AXL_OK: a default-constructed value names nothing and
       must not read as a valid empty document. */
    AxlStatus     m_err = AXL_NOT_FOUND;
};

// ---------------------------------------------------------------------------
// Ranges
// ---------------------------------------------------------------------------

/// Input iterator over a JSON array's elements. See #axl::json_array_range.
class json_array_iterator {
public:
    using iterator_concept  = std::input_iterator_tag;
    using iterator_category = std::input_iterator_tag;
    using value_type        = json_value;
    using difference_type   = std::ptrdiff_t;

    json_array_iterator() noexcept = default;

    explicit json_array_iterator(const AxlJsonReader &r) noexcept
    {
        if (axl_json_value_array_begin(&r, &m_it)) {
            m_live = true;
            ++*this;
        }
    }

    const json_value &operator*() const noexcept { return m_cur; }
    const json_value *operator->() const noexcept { return &m_cur; }

    json_array_iterator &
    operator++() noexcept
    {
        AxlJsonReader elem;
        if (m_live && axl_json_array_next(&m_it, &elem)) {
            m_cur = json_value(elem);
        } else {
            m_live = false;
            m_cur  = json_value{};
        }
        return *this;
    }

    /* Post-increment returns void. An input iterator may, and a copy would
       promise a value the advanced iterator no longer holds. */
    void operator++(int) noexcept { ++*this; }

    bool operator==(std::default_sentinel_t) const noexcept { return !m_live; }

private:
    AxlJsonArrayIter m_it{};
    json_value       m_cur{};
    bool             m_live = false;
};

/// A JSON array as a range. Single-pass: the underlying C iterator advances.
class json_array_range : public std::ranges::view_interface<json_array_range> {
public:
    json_array_range() noexcept = default;
    explicit json_array_range(const AxlJsonReader &r) noexcept : m_r(r), m_ok(true) {}

    [[nodiscard]] json_array_iterator begin() const noexcept
    {
        return m_ok ? json_array_iterator(m_r) : json_array_iterator{};
    }
    [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }

private:
    AxlJsonReader m_r{};
    bool          m_ok = false;
};

/// One key/value pair from #axl::json_object_range.
struct json_member {
    /// The DECODED key, owned. Object keys carry escapes, so a borrowed view
    /// of the document would hand back `\\u0041` where the key is `A`.
    std::string key;
    json_value  value;
};

/// Input iterator over a JSON object's members. See #axl::json_object_range.
class json_object_iterator {
public:
    using iterator_concept  = std::input_iterator_tag;
    using iterator_category = std::input_iterator_tag;
    using value_type        = json_member;
    using difference_type   = std::ptrdiff_t;

    json_object_iterator() noexcept = default;

    explicit json_object_iterator(const AxlJsonReader &r)
    {
        if (axl_json_value_object_begin(&r, &m_it)) {
            m_live = true;
            ++*this;
        }
    }

    const json_member &operator*() const noexcept { return m_cur; }
    const json_member *operator->() const noexcept { return &m_cur; }

    json_object_iterator &
    operator++()
    {
        size_t klen = 0;
        /* PEEK first, so the key buffer is sized before the pair is taken.
           axl_json_object_next() truncates a key that does not fit and reports
           it only afterwards, with the pair already consumed -- so sizing
           first is the only way a std::string key can be whole. */
        if (!m_live || !axl_json_object_peek_key_len(&m_it, &klen)) {
            m_live = false;
            m_cur  = json_member{};
            return *this;
        }
        json_member out;
        out.key.resize(klen);
        AxlJsonReader value;
        if (!axl_json_object_next(&m_it, out.key.data(), klen + 1, &value)) {
            m_live = false;
            m_cur  = json_member{};
            return *this;
        }
        out.value = json_value(value);
        m_cur     = std::move(out);
        return *this;
    }

    void operator++(int) { ++*this; }

    bool operator==(std::default_sentinel_t) const noexcept { return !m_live; }

private:
    AxlJsonObjectIter m_it{};
    json_member       m_cur{};
    bool              m_live = false;
};

/**
 * A JSON object as a range of #axl::json_member.
 *
 * Single-pass. Each step allocates a `std::string` for the key, which is what
 * makes the key whole and outlive the iteration step; a scan that only
 * COMPARES keys against known names is cheaper through `operator[]` or the C
 * iterator with a fixed buffer.
 */
class json_object_range : public std::ranges::view_interface<json_object_range> {
public:
    json_object_range() noexcept = default;
    explicit json_object_range(const AxlJsonReader &r) noexcept : m_r(r), m_ok(true) {}

    [[nodiscard]] json_object_iterator begin() const
    {
        return m_ok ? json_object_iterator(m_r) : json_object_iterator{};
    }
    [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }

private:
    AxlJsonReader m_r{};
    bool          m_ok = false;
};

inline json_array_range
json_value::array() const noexcept
{
    return m_err == AXL_OK ? json_array_range(m_r) : json_array_range{};
}

inline json_object_range
json_value::object() const noexcept
{
    return m_err == AXL_OK ? json_object_range(m_r) : json_object_range{};
}

// ---------------------------------------------------------------------------
// The document
// ---------------------------------------------------------------------------

/**
 * A parsed JSON document, owning its token array.
 *
 * Move-only, matching the C design's single-owner rule. Every
 * #axl::json_value it hands out borrows it.
 */
class json_document : public json_value {
public:
    json_document() noexcept = default;

    ~json_document() { reset(); }

    json_document(const json_document &) = delete;
    json_document &operator=(const json_document &) = delete;

    json_document(json_document &&other) noexcept
        : json_value(other), m_owned(std::move(other.m_owned)),
          m_owns(other.m_owns)
    {
        /* m_owns is carried EXPLICITLY. Omitting it let the default member
           initializer (false) win, so the moved-TO document never freed --
           and since parse() returns by value, that leaked every document the
           API ever produced. */
        other.disown();
    }

    json_document &
    operator=(json_document &&other) noexcept
    {
        if (this != &other) {
            reset();
            static_cast<json_value &>(*this) = other;
            m_owned = std::move(other.m_owned);
            m_owns  = other.m_owns;
            other.disown();
        }
        return *this;
    }

    /**
     * Parse @a bytes WITHOUT copying them.
     *
     * Zero-copy, like `axl_json_parse()` — the document points into @a bytes
     * and every string it hands back is decoded from them on demand.
     *
     * @warning @a bytes must outlive the document AND every value taken from
     *     it. `parse(read_file())` on a temporary is a dangling document that
     *     reads plausible garbage. Use #parse_owning() when the bytes are not
     *     already owned by something longer-lived.
     *
     * @return the document, or the parse failure as an #AxlStatus.
     */
    [[nodiscard]] static result<json_document>
    parse(
        std::string_view bytes,                      ///< borrowed document
        AxlJsonFlags     flags = AXL_JSON_RELAXED    ///< dialect
    )
    {
        json_document d;
        AxlJsonReader r;
        if (!axl_json_parse(bytes.data(), bytes.size(), flags, &r)) {
            return err(AXL_INVALID);
        }
        d.adopt(r);
        return d;
    }

    /**
     * Parse @a bytes and KEEP them.
     *
     * The safe default for C++, where the bytes are usually a temporary. @a
     * bytes is moved into the document, so there is one copy at most and none
     * if the caller moves in.
     *
     * @return the document, or the parse failure as an #AxlStatus.
     */
    [[nodiscard]] static result<json_document>
    parse_owning(
        std::string  bytes,                          ///< document, moved in
        AxlJsonFlags flags = AXL_JSON_RELAXED        ///< dialect
    )
    {
        json_document d;
        /* The bytes live behind a unique_ptr, NOT in a std::string member.
           The reader points INTO them, and a short document lives in the
           string's small-buffer -- inside the object -- so moving the document
           would relocate the bytes and leave the reader pointing at the old
           address. A heap-stable holder makes the address survive every move,
           which is what a move-only owning document needs. */
        d.m_owned = std::make_unique<std::string>(std::move(bytes));
        AxlJsonReader r;
        if (!axl_json_parse(d.m_owned->data(), d.m_owned->size(), flags, &r)) {
            return err(AXL_INVALID);
        }
        d.adopt(r);
        return d;
    }

private:
    void adopt(const AxlJsonReader &r) noexcept
    {
        static_cast<json_value &>(*this) = json_value(r);
        m_owns = true;
    }

    void disown() noexcept
    {
        other_reset();
    }

    void other_reset() noexcept
    {
        m_owns = false;
        static_cast<json_value &>(*this) = json_value{};
    }

    void reset() noexcept
    {
        if (m_owns) {
            /* const_cast: axl_json_free takes a mutable reader and this object
               owns the one it is freeing. json_value keeps the reader by value
               and exposes it read-only, which is right for every OTHER holder
               of one. */
            axl_json_free(const_cast<AxlJsonReader *>(reader()));
        }
        other_reset();
        m_owned.reset();
    }

    std::unique_ptr<std::string> m_owned;
    bool                         m_owns = false;
};


// ---------------------------------------------------------------------------
// The writer
// ---------------------------------------------------------------------------

class json_writer;

/**
 * An open `{` or `[`, closed by its destructor.
 *
 * The C writer's containers are balanced by discipline —
 * `axl_json_obj_begin()` and a matching `axl_json_obj_end()`, with an
 * unbalanced pair reported only at axl_json_writer_finish(). This makes the
 * pairing structural: the close happens on every path out of the scope,
 * including an early `return`.
 *
 * `[[nodiscard]]`, because `w.object();` with the variable name forgotten
 * opens and closes a container within the one full-expression and emits `{}`
 * — the same silent mistake `std::lock_guard` is famous for.
 *
 * Neither copyable nor movable: the close must happen exactly once, where the
 * scope ends.
 */
class [[nodiscard]] json_scope {
public:
    ~json_scope();

    json_scope(const json_scope &) = delete;
    json_scope &operator=(const json_scope &) = delete;
    json_scope(json_scope &&) = delete;
    json_scope &operator=(json_scope &&) = delete;

private:
    friend class json_writer;
    json_scope(json_writer *w, bool is_object) noexcept
        : m_w(w), m_object(is_object) {}

    json_writer *m_w;
    bool         m_object;
};

/**
 * Builds a JSON document into an `AxlString`.
 *
 * Wraps `AxlJsonWriter`, whose error flag is STICKY — the first failure turns
 * every later call into a no-op, so a caller may emit a whole document and
 * check once at #finish(). That is the C contract and this does not soften it.
 */
class json_writer {
public:
    /**
     * Append to @a out.
     *
     * @a out is borrowed and must outlive the writer. It is APPENDED to, not
     * cleared — `axl_string_clear()` first to reuse one.
     */
    explicit json_writer(
        AxlString    *out,                        ///< destination, borrowed
        AxlJsonFlags  flags = AXL_JSON_STRICT     ///< dialect + formatting
    ) noexcept
    {
        axl_json_writer_init(&m_w, out, flags);
    }

    json_writer(const json_writer &) = delete;
    json_writer &operator=(const json_writer &) = delete;

    /// Open an object. Closes when the returned scope dies.
    [[nodiscard]] json_scope object() noexcept
    {
        axl_json_obj_begin(&m_w);
        return json_scope(this, true);
    }

    /// Open an object as @a key's value.
    [[nodiscard]] json_scope object(std::string_view key) noexcept
    {
        axl_json_keyn(&m_w, key.data(), key.size());
        axl_json_obj_begin(&m_w);
        return json_scope(this, true);
    }

    /// Open an array. Closes when the returned scope dies.
    [[nodiscard]] json_scope array() noexcept
    {
        axl_json_arr_begin(&m_w);
        return json_scope(this, false);
    }

    /// Open an array as @a key's value.
    [[nodiscard]] json_scope array(std::string_view key) noexcept
    {
        axl_json_keyn(&m_w, key.data(), key.size());
        axl_json_arr_begin(&m_w);
        return json_scope(this, false);
    }

    /// Emit a bare key. Only needed for a value this class cannot type —
    /// #add() emits the key itself.
    void key(std::string_view k) noexcept
    {
        axl_json_keyn(&m_w, k.data(), k.size());
    }

    /**
     * Emit @a v as a value, choosing the emitter from its type.
     *
     * `std::string` / `std::string_view` / `const char *` -> string,
     * `bool` -> bool, a signed integral -> int, an unsigned one -> uint,
     * a floating type -> double, `nullptr` -> null, and an
     * #axl::json_value -> the parsed subtree, spliced.
     *
     * A type with no mapping is a compile error naming the ones that exist.
     * Note `bool` is checked BEFORE the integral cases: it is integral, and
     * without the order `add(true)` would emit `1`.
     */
    template <class T>
    void
    add(
        T &&v    ///< the value
    ) noexcept
    {
        emit(std::forward<T>(v));
    }

    /// Emit @a key and @a v as a member. Same type mapping as #add(T &&).
    template <class T>
    void
    add(
        std::string_view key,   ///< member name
        T              &&v      ///< the value
    ) noexcept
    {
        axl_json_keyn(&m_w, key.data(), key.size());
        emit(std::forward<T>(v));
    }

    /**
     * Copy a parsed value into this document, verbatim.
     *
     * `axl_json_write_token()` — the whole-document-into-streaming bridge. A
     * sub-reader is rebased, so `w.splice(doc["items"])` writes exactly that
     * subtree with its escapes in their source spelling.
     */
    void
    splice(
        const json_value &v    ///< parsed value to copy in
    ) noexcept
    {
        if (!v.exists()) { return; }
        axl_json_write_token(&m_w, v.reader(), 0);
    }

    /// Emit @a key and splice @a v as its value.
    void splice(std::string_view key, const json_value &v) noexcept
    {
        if (!v.exists()) { return; }
        axl_json_keyn(&m_w, key.data(), key.size());
        axl_json_write_token(&m_w, v.reader(), 0);
    }

    /// Emit a JSON5 comment. A no-op unless the dialect allows comments.
    void comment(std::string_view text) noexcept
    {
        axl_json_comment(&m_w, std::string(text).c_str());
    }

    /**
     * Finalize, and report whether the whole document was written.
     *
     * Validates that every container was closed and that the sink took every
     * byte.
     *
     * @return the byte count the sink accepted, or #AXL_INVALID if the sticky
     *     error was ever set — an unclosed container, a structural misuse, or
     *     a sink that refused.
     */
    [[nodiscard]] result<size_t>
    finish() noexcept
    {
        size_t n = axl_json_writer_finish(&m_w);
        if (axl_json_writer_error(&m_w)) { return err(AXL_INVALID); }
        return n;
    }

    /// Whether the sticky error flag is set.
    [[nodiscard]] bool failed() const noexcept
    {
        return axl_json_writer_error(&m_w);
    }

    /// The underlying C writer, for the calls this class does not wrap.
    [[nodiscard]] AxlJsonWriter *get() noexcept { return &m_w; }

private:
    friend class json_scope;

    void emit(bool v) noexcept              { axl_json_bool(&m_w, v); }
    void emit(std::nullptr_t) noexcept      { axl_json_null(&m_w); }
    void emit(const char *v) noexcept
    {
        if (v == nullptr) { axl_json_null(&m_w); } else { axl_json_str(&m_w, v); }
    }
    void emit(std::string_view v) noexcept  { axl_json_strn(&m_w, v.data(), v.size()); }
    void emit(const std::string &v) noexcept { axl_json_strn(&m_w, v.data(), v.size()); }
    void emit(const json_value &v) noexcept { splice(v); }

    template <class T>
    void
    emit(T v) noexcept
    {
        if constexpr (std::is_floating_point_v<T>) {
            axl_json_double(&m_w, (double)v);
        } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            axl_json_int(&m_w, (int64_t)v);
        } else if constexpr (std::is_integral_v<T>) {
            axl_json_uint(&m_w, (uint64_t)v);
        } else {
            static_assert(sizeof(T) == 0,
                          "axl::json_writer::add supports a string type, bool, "
                          "an integral or floating-point type, nullptr, or an "
                          "axl::json_value to splice");
        }
    }

    AxlJsonWriter m_w{};
};

inline json_scope::~json_scope()
{
    if (m_object) {
        axl_json_obj_end(&m_w->m_w);
    } else {
        axl_json_arr_end(&m_w->m_w);
    }
}


// ---------------------------------------------------------------------------
// The scanner — the streaming READ face
// ---------------------------------------------------------------------------

/**
 * One pull from #axl::json_scanner, as a range element.
 *
 * @warning `text` BORROWS the scanner's bytes and dies at the next advance —
 *     that is `AxlJsonEvent`'s contract unchanged, and a range-for advances
 *     for you at the bottom of every iteration. So this is safe to read
 *     inside the loop body and never safe to store. #text_copy() is the way
 *     to keep one. It is also NOT NUL-terminated, which is why it is a
 *     `string_view` and not a `const char *`.
 */
struct json_event {
    AxlJsonEventKind kind = AXL_JSON_EV_EOF;   ///< what this event is
    std::string_view text;                     ///< raw source bytes, borrowed

    /// The value type this event denotes, or #AXL_JSON_TYPE_NONE for a
    /// structural one. `axl_json_event_type()`.
    [[nodiscard]] AxlJsonType type() const noexcept
    {
        AxlJsonEvent ev{};
        ev.kind = kind;
        ev.text = text.data();
        ev.len  = text.size();
        return axl_json_event_type(&ev);
    }

    /// Is this a key or string equal to @a s? Compares DECODED, so an escaped
    /// key matches its plain spelling — `axl_json_event_equals()`, which
    /// exists so a caller need not allocate to ask.
    [[nodiscard]] bool equals(const char *s) const noexcept
    {
        AxlJsonEvent ev{};
        ev.kind = kind;
        ev.text = text.data();
        ev.len  = text.size();
        return axl_json_event_equals(&ev, s);
    }

    /// The DECODED text, owned — the way to keep a key past the next advance.
    [[nodiscard]] std::string
    text_copy() const
    {
        AxlJsonEvent ev{};
        ev.kind = kind;
        ev.text = text.data();
        ev.len  = text.size();
        /* Sized from the source span: decoding a string only shrinks it,
           except that REPAIR can turn one ill-formed byte into the three of
           U+FFFD -- hence 3x rather than 1x. axl_json_event_string() reports
           truncation as -1 rather than returning a short answer, which is
           what makes the size check below meaningful. */
        std::string out;
        out.resize(text.size() * 3 + 1);
        int n = axl_json_event_string(&ev, out.data(), out.size());
        if (n < 0) { return std::string{}; }
        out.resize((size_t)n);
        return out;
    }
};

/// Input iterator over scanner events. See #axl::json_scanner.
class json_scanner_iterator {
public:
    using iterator_concept  = std::input_iterator_tag;
    using iterator_category = std::input_iterator_tag;
    using value_type        = json_event;
    using difference_type   = std::ptrdiff_t;

    json_scanner_iterator() noexcept = default;
    explicit json_scanner_iterator(AxlJsonScanner *s) noexcept : m_s(s)
    {
        m_live = (m_s != nullptr);
        ++*this;
    }

    const json_event &operator*() const noexcept { return m_cur; }
    const json_event *operator->() const noexcept { return &m_cur; }

    json_scanner_iterator &
    operator++() noexcept
    {
        AxlJsonEvent ev{};
        if (m_live && axl_json_scanner_next(m_s, &ev)
            && ev.kind != AXL_JSON_EV_EOF) {
            m_cur.kind = ev.kind;
            m_cur.text = std::string_view(ev.text != nullptr ? ev.text : "",
                                          ev.text != nullptr ? ev.len : 0);
        } else {
            /* EOF ends the range rather than being yielded. It carries no
               text, and a loop that had to skip it would be the shape every
               caller writes anyway. axl_json_scanner_next() returning false
               is a scan FAILURE, which json_scanner::failed() reports. */
            m_live = false;
            m_cur  = json_event{};
        }
        return *this;
    }

    void operator++(int) noexcept { ++*this; }

    bool operator==(std::default_sentinel_t) const noexcept { return !m_live; }

private:
    AxlJsonScanner *m_s = nullptr;
    json_event      m_cur{};
    bool            m_live = false;
};

/**
 * Pull events from a JSON document without building one.
 *
 * The streaming read face, for a document too large to tokenize or one whose
 * shape is known and only one field is wanted. Owns the C scanner; move-only.
 *
 * @code
 * axl::json_scanner sc{bytes};
 * for (const axl::json_event &ev : sc) {
 *     if (ev.kind == AXL_JSON_EV_KEY && ev.equals("port")) { ... }
 * }
 * if (sc.failed()) { ... }        // a malformed document, not merely an end
 * @endcode
 */
class json_scanner : public std::ranges::view_interface<json_scanner> {
public:
    /**
     * Scan @a bytes.
     *
     * @warning @a bytes is BORROWED and must outlive the scanner — the C
     *     source is zero-copy over a contiguous buffer, exactly as
     *     #axl::json_document::parse() is.
     */
    explicit json_scanner(
        std::string_view bytes,                     ///< borrowed document
        AxlJsonFlags     flags = AXL_JSON_RELAXED   ///< dialect
    ) noexcept
    {
        axl_json_source_init_mem(&m_src, bytes.data(), bytes.size());
        m_ok = axl_json_scanner_init(&m_s, &m_src, flags);
    }

    ~json_scanner() { if (m_ok) { axl_json_scanner_free(&m_s); } }

    json_scanner(const json_scanner &) = delete;
    json_scanner &operator=(const json_scanner &) = delete;

    [[nodiscard]] json_scanner_iterator begin() noexcept
    {
        return m_ok ? json_scanner_iterator(&m_s) : json_scanner_iterator{};
    }
    [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }

    /// Whether the scan stopped on a MALFORMED document rather than its end.
    /// A range-for cannot tell the two apart — both end the loop — so this is
    /// the question to ask afterwards.
    [[nodiscard]] bool failed() const noexcept
    {
        return !m_ok || axl_json_scanner_error(&m_s)->code != AXL_JSON_OK;
    }

    /// The underlying C scanner, for the calls this class does not wrap.
    [[nodiscard]] AxlJsonScanner *get() noexcept { return &m_s; }

private:
    AxlJsonSource  m_src{};
    AxlJsonScanner m_s{};
    bool           m_ok = false;
};

} // namespace axl

#endif /* AXL_JSON_HPP */
