// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Decoded results on disk: the lists every receiver keeps until the user clears them (messages, aircraft, vessels, cells, pictures...)
// are saved as a compact binary form of plain structs and the standard containers. A file starts with a signature of the saved types,
// so a build whose structs differ (a field added, removed or retyped) discards an old file instead of misreading it.
//
// pack(a, b, ...) -> blob; unpack(blob, a, b, ...) -> false (and nothing changed) when the blob is damaged or of other types.
// Plain structs (aggregates, C arrays inside them too) are walked field by field with no list to keep in step: see results_fields.h.
#pragma once
#include "results_fields.h"
#include "dect2/files/files_proto.h"
#include <array>
#include <complex>
#include <cstdint>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dect2::results {

constexpr uint32_t kFormat = 1;   // the encoding itself: raise it when put()/get() change (and in core/src/files/files_proto.cpp and rust/onair_files/src/container.rs, which write and check the header)

namespace detail {

// ---- the fields of a plain struct: T{{x}, {x}, ...} compiles for up to as many braced initializers as T has fields
// (a braced initializer always fills exactly one field, a C array or a nested struct included)
struct Any { template <class T> operator T() const; };   // never called: only for counting
template <class T, size_t... I> constexpr bool initN(std::index_sequence<I...>) { return requires { T{{((void)I, Any{})}...}; }; }
template <class T, size_t N = 0> constexpr size_t fieldCount() {
    if constexpr (N > kMaxFields) return N;
    else if constexpr (initN<T>(std::make_index_sequence<N + 1>{})) return fieldCount<T, N + 1>();
    else return N;
}
template <class T, class F> void forEachField(T& t, F&& f) {
    using U = std::remove_cv_t<T>;
    static_assert(std::is_aggregate_v<U>, "results: only plain structs (aggregates) are saved field by field");
    forEachFieldN<fieldCount<U>()>(t, f);
}

template <class T, template <class...> class Tpl> struct isSpec : std::false_type {};
template <template <class...> class Tpl, class... A> struct isSpec<Tpl<A...>, Tpl> : std::true_type {};
template <class T> struct isStdArray : std::false_type {};
template <class E, size_t N> struct isStdArray<std::array<E, N>> : std::true_type {};
template <class T> struct isVecBool : std::false_type {};
template <class A> struct isVecBool<std::vector<bool, A>> : std::true_type {};
template <class T> constexpr bool kSeq = isSpec<T, std::vector>::value || isSpec<T, std::deque>::value || isSpec<T, std::list>::value;
template <class T> constexpr bool kSet = isSpec<T, std::set>::value || isSpec<T, std::unordered_set>::value;
template <class T> constexpr bool kMap = isSpec<T, std::map>::value || isSpec<T, std::unordered_map>::value;
template <class T> constexpr bool kPtr = isSpec<T, std::shared_ptr>::value || isSpec<T, std::unique_ptr>::value;
template <class> constexpr bool kNever = false;

struct Writer {
    std::string s;
    void raw(const void* p, size_t n) { s.append(static_cast<const char*>(p), n); }
    void u32(uint32_t v) { raw(&v, 4); }
};

struct Reader {
    const char* p = nullptr;
    const char* e = nullptr;
    bool ok = true;
    bool raw(void* d, size_t n) {
        if (!ok || (size_t)(e - p) < n) { ok = false; return false; }
        if (n) std::memcpy(d, p, n);
        p += n;
        return true;
    }
    bool count(uint32_t& n) {   // a list length: never more items than bytes left (an empty struct aside), so damage cannot ask for gigabytes
        if (!raw(&n, 4)) return false;
        if (n > (size_t)(e - p) + 4096) { ok = false; return false; }
        return true;
    }
};

template <class T> void put(Writer& w, const T& v);
template <class T> void get(Reader& r, T& v);

template <class T> void put(Writer& w, const T& v) {
    if constexpr (std::is_same_v<T, bool>) { const uint8_t b = v ? 1 : 0; w.raw(&b, 1); }
    else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) w.raw(&v, sizeof v);
    else if constexpr (std::is_array_v<T>) { for (const auto& x : v) put(w, x); }
    else if constexpr (isSpec<T, std::basic_string>::value) { w.u32((uint32_t)v.size()); w.raw(v.data(), v.size() * sizeof(typename T::value_type)); }
    else if constexpr (isSpec<T, std::complex>::value) { put(w, v.real()); put(w, v.imag()); }
    else if constexpr (isSpec<T, std::pair>::value) { put(w, v.first); put(w, v.second); }
    else if constexpr (isStdArray<T>::value) { for (const auto& x : v) put(w, x); }
    else if constexpr (kSeq<T> || kSet<T>) { w.u32((uint32_t)v.size()); for (const auto& x : v) put(w, (const typename T::value_type&)x); }
    else if constexpr (kMap<T>) { w.u32((uint32_t)v.size()); for (const auto& [k, x] : v) { put(w, k); put(w, x); } }
    else if constexpr (isSpec<T, std::optional>::value) { put(w, v.has_value()); if (v) put(w, *v); }
    else if constexpr (kPtr<T>) { put(w, v != nullptr); if (v) put(w, *v); }
    else if constexpr (std::is_aggregate_v<T>) forEachField(v, [&](const auto& f) { put(w, f); });
    else static_assert(kNever<T>, "results: a type that cannot be saved (add it to results_io.h)");
}

template <class T> void get(Reader& r, T& v) {
    if (!r.ok) return;
    if constexpr (std::is_same_v<T, bool>) { uint8_t b = 0; r.raw(&b, 1); v = b != 0; }
    else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) r.raw(&v, sizeof v);
    else if constexpr (std::is_array_v<T>) { for (auto& x : v) get(r, x); }
    else if constexpr (isSpec<T, std::basic_string>::value) {
        uint32_t n = 0;
        if (!r.count(n)) return;
        v.resize(n);
        r.raw(v.data(), n * sizeof(typename T::value_type));
    }
    else if constexpr (isSpec<T, std::complex>::value) { typename T::value_type re{}, im{}; get(r, re); get(r, im); v = T(re, im); }
    else if constexpr (isSpec<T, std::pair>::value) { get(r, v.first); get(r, v.second); }
    else if constexpr (isStdArray<T>::value) { for (auto& x : v) get(r, x); }
    else if constexpr (isVecBool<T>::value) {
        uint32_t n = 0;
        if (!r.count(n)) return;
        v.assign(n, false);
        for (uint32_t i = 0; i < n && r.ok; i++) { bool b = false; get(r, b); v[i] = b; }
    }
    else if constexpr (kSeq<T>) {
        uint32_t n = 0;
        if (!r.count(n)) return;
        v.clear();
        for (uint32_t i = 0; i < n && r.ok; i++) { v.emplace_back(); get(r, v.back()); }
    }
    else if constexpr (kSet<T>) {
        uint32_t n = 0;
        if (!r.count(n)) return;
        v.clear();
        for (uint32_t i = 0; i < n && r.ok; i++) { typename T::value_type x{}; get(r, x); v.insert(std::move(x)); }
    }
    else if constexpr (kMap<T>) {
        uint32_t n = 0;
        if (!r.count(n)) return;
        v.clear();
        for (uint32_t i = 0; i < n && r.ok; i++) {
            typename T::key_type k{};
            get(r, k);
            typename T::mapped_type x{};
            get(r, x);
            v.emplace(std::move(k), std::move(x));
        }
    }
    else if constexpr (isSpec<T, std::optional>::value) {
        bool has = false;
        get(r, has);
        if (has) { typename T::value_type x{}; get(r, x); v = std::move(x); } else v.reset();
    }
    else if constexpr (kPtr<T>) {
        bool has = false;
        get(r, has);
        using E = std::remove_const_t<typename T::element_type>;
        if (has) {
            auto x = std::make_unique<E>();
            get(r, *x);
            v = T(x.release());
        } else v = nullptr;
    }
    else if constexpr (std::is_aggregate_v<T>) forEachField(v, [&](auto& f) { get(r, f); });
    else static_assert(kNever<T>, "results: a type that cannot be loaded (add it to results_io.h)");
}

// The shape of a type as text: one letter per kind, sizes, and the fields of structs in order
template <class T> void sig(std::string& s, int depth = 0) {
    if (depth > 24) { s += '~'; return; }
    if constexpr (std::is_same_v<T, bool>) s += 'b';
    else if constexpr (std::is_enum_v<T>) { s += 'e'; s += std::to_string(sizeof(T)); }
    else if constexpr (std::is_arithmetic_v<T>) { s += std::is_floating_point_v<T> ? 'f' : std::is_signed_v<T> ? 'i' : 'u'; s += std::to_string(sizeof(T)); }
    else if constexpr (std::is_array_v<T>) { s += 'A'; s += std::to_string(std::extent_v<T>); sig<std::remove_cv_t<std::remove_extent_t<T>>>(s, depth + 1); }
    else if constexpr (isSpec<T, std::basic_string>::value) { s += 's'; s += std::to_string(sizeof(typename T::value_type)); }
    else if constexpr (isSpec<T, std::complex>::value) { s += 'c'; sig<typename T::value_type>(s, depth + 1); }
    else if constexpr (isSpec<T, std::pair>::value) { s += 'p'; sig<std::remove_cv_t<typename T::first_type>>(s, depth + 1); sig<typename T::second_type>(s, depth + 1); }
    else if constexpr (isStdArray<T>::value) { s += 'a'; s += std::to_string(std::tuple_size_v<T>); sig<typename T::value_type>(s, depth + 1); }
    else if constexpr (kSeq<T>) { s += 'v'; sig<typename T::value_type>(s, depth + 1); }
    else if constexpr (kSet<T>) { s += 'S'; sig<typename T::value_type>(s, depth + 1); }
    else if constexpr (kMap<T>) { s += 'm'; sig<typename T::key_type>(s, depth + 1); sig<typename T::mapped_type>(s, depth + 1); }
    else if constexpr (isSpec<T, std::optional>::value) { s += 'o'; sig<typename T::value_type>(s, depth + 1); }
    else if constexpr (kPtr<T>) { s += 'P'; sig<std::remove_const_t<typename T::element_type>>(s, depth + 1); }
    else if constexpr (std::is_aggregate_v<T>) {
        s += '{';
        T t{};
        forEachField(t, [&](auto& f) { sig<std::remove_cvref_t<decltype(f)>>(s, depth + 1); });
        s += '}';
    }
    else static_assert(kNever<T>, "results: a type that cannot be saved (add it to results_io.h)");
}

inline uint64_t fnv1a(const std::string& s) { return filesp::fnv1a(s); }   // FNV-1a 64 bit of a blob (what each saved file holds)

template <class... T> uint64_t signatureHash() {
    std::string s;
    (sig<std::remove_cvref_t<T>>(s), ...);
    return filesp::signatureHash(s);   // FNV-1a of the signature text xor kFormat: core/src/files/files_proto.cpp (C++) or the Rust crate onair_files
}

template <class Tup, size_t... I, class... T> void moveOut(Tup& tmp, std::index_sequence<I...>, T&... v) { ((v = std::move(std::get<I>(tmp))), ...); }

} // namespace detail

// The text the signature is made from (tests print it when two builds disagree)
template <class... T> std::string signature() { std::string s; (detail::sig<std::remove_cvref_t<T>>(s), ...); return s; }

template <class... T> std::string pack(const T&... v) {
    detail::Writer w;
    w.s = filesp::containerHeader(detail::signatureHash<T...>());   // OARS, kFormat, the signature hash
    (detail::put(w, v), ...);
    return std::move(w.s);
}

// Every value is read into a copy first: a damaged or foreign blob leaves the targets as they were
template <class... T> bool unpack(const std::string& blob, T&... v) {
    detail::Reader r;
    const size_t off = filesp::containerCheck(blob, detail::signatureHash<T...>());   // magic, format and signature hash
    if (off == SIZE_MAX) return false;
    r.p = blob.data() + off; r.e = blob.data() + blob.size();
    std::tuple<std::remove_cvref_t<T>...> tmp;
    std::apply([&](auto&... x) { (detail::get(r, x), ...); }, tmp);
    if (!r.ok || r.p != r.e) return false;
    detail::moveOut(tmp, std::index_sequence_for<T...>{}, v...);
    return true;
}

} // namespace dect2::results
