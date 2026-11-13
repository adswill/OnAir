// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Fixed evaluation order for the seeded tests.
//
// C++ leaves the order in which the arguments of a call and the operands of most operators are evaluated unspecified: Clang goes left to right, GCC on
// x86 mostly right to left. When two arguments each draw from the test's random generator, the values (and so every digest of tests/common/golden.h)
// differ between compilers. The braced initialiser list is the one place where the order is fixed (left to right), so these macros pass the arguments
// through a std::tuple{...}. Use them whenever more than one argument or operand of an expression draws a random number.
#pragma once
#include <functional>   // std::ref: pass a reference argument through ORD_CALL as std::ref(x)
#include <string>
#include <tuple>
#include <utility>

namespace ordered {
template <class F, class... A> decltype(auto) call(F&& f, std::tuple<A...>&& a) { return std::apply(std::forward<F>(f), std::move(a)); }
template <class... A> std::string cat(std::tuple<A...>&& a) {
    std::string r;
    std::apply([&](const auto&... p) { ((r += p), ...); }, a);
    return r;
}
}  // namespace ordered

// ORD_CALL(f, a, b, c) is f(a, b, c) with a, b, c evaluated in this order (f may be a function, a method such as v.insert, or a qualified name).
#define ORD_CALL(fn, ...) ordered::call([&](auto&&... ordArg) -> decltype(auto) { return fn(std::forward<decltype(ordArg)>(ordArg)...); }, std::tuple{__VA_ARGS__})
// ORD_CAT(a, b, c) is the string a + b + c (each a std::string, const char* or char) with the parts evaluated in this order.
#define ORD_CAT(...) ordered::cat(std::tuple{__VA_ARGS__})
