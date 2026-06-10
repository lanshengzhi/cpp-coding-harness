#pragma once

#include <expected>
#include <utility>

// Private implementation macros for coroutine error propagation.
// These macros use co_return and are therefore only valid inside coroutines
// that return std::expected. Do NOT use in public headers.

#define CCH_DETAIL_TRY_CAT(a, b) a ## b
#define CCH_DETAIL_TRY_UNIQUE(name, counter) CCH_DETAIL_TRY_CAT(name, counter)

#define CCH_TRY(var, expr) \
    CCH_DETAIL_TRY_IMPL(var, expr, __COUNTER__)
#define CCH_DETAIL_TRY_IMPL(var, expr, counter) \
    auto CCH_DETAIL_TRY_UNIQUE(_cch_try_expected_, counter) = (expr); \
    if (!CCH_DETAIL_TRY_UNIQUE(_cch_try_expected_, counter)) { \
        co_return std::unexpected(CCH_DETAIL_TRY_UNIQUE(_cch_try_expected_, counter).error()); \
    } \
    auto var = std::move(*CCH_DETAIL_TRY_UNIQUE(_cch_try_expected_, counter))

#define CCH_TRY_VOID(expr) \
    CCH_DETAIL_TRY_VOID_IMPL(expr, __COUNTER__)
#define CCH_DETAIL_TRY_VOID_IMPL(expr, counter) \
    do { \
        auto CCH_DETAIL_TRY_UNIQUE(_cch_try_void_expected_, counter) = (expr); \
        if (!CCH_DETAIL_TRY_UNIQUE(_cch_try_void_expected_, counter)) { \
            co_return std::unexpected(CCH_DETAIL_TRY_UNIQUE(_cch_try_void_expected_, counter).error()); \
        } \
    } while (0)
