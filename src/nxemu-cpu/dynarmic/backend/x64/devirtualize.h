/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstring>
#include <utility>

#include <mcl/assert.hpp>
#include <mcl/bit_cast.hpp>
#include <mcl/stdint.hpp>
#include <mcl/type_traits/function_info.hpp>

#include "dynarmic/backend/x64/callback.h"

namespace Dynarmic {
namespace Backend::X64 {

namespace impl {

template<typename FunctionType, FunctionType mfp>
struct ThunkBuilder;

template<typename C, typename R, typename... Args, R (C::*mfp)(Args...)>
struct ThunkBuilder<R (C::*)(Args...), mfp> {
    static R Thunk(C* this_, Args... args) {
        return (this_->*mfp)(std::forward<Args>(args)...);
    }
};

}  // namespace impl

template<auto mfp>
ArgCallback DevirtualizeGeneric(mcl::class_type<decltype(mfp)>* this_) {
    return ArgCallback{&impl::ThunkBuilder<decltype(mfp), mfp>::Thunk, reinterpret_cast<u64>(this_)};
}

template<auto mfp>
ArgCallback DevirtualizeWindows(mcl::class_type<decltype(mfp)>* this_) {
    static_assert(sizeof(mfp) == 8);
    return ArgCallback{mcl::bit_cast<u64>(mfp), reinterpret_cast<u64>(this_)};
}

template<auto mfp>
ArgCallback DevirtualizeItanium(mcl::class_type<decltype(mfp)>* this_) {
    struct MemberFunctionPointer {
        /// For a non-virtual function, this is a simple function pointer.
        /// For a virtual function, it is (1 + virtual table offset in bytes).
        u64 ptr;
        /// The required adjustment to `this`, prior to the call.
        u64 adj;
    } mfp_struct = mcl::bit_cast<MemberFunctionPointer>(mfp);

    static_assert(sizeof(MemberFunctionPointer) == 16);
    static_assert(sizeof(MemberFunctionPointer) == sizeof(mfp));

    u64 fn_ptr = mfp_struct.ptr;
    u64 this_ptr = reinterpret_cast<u64>(this_) + mfp_struct.adj;
    if (mfp_struct.ptr & 1) {
        u64 vtable = mcl::bit_cast_pointee<u64>(this_ptr);
        fn_ptr = mcl::bit_cast_pointee<u64>(vtable + fn_ptr - 1);
    }
    return ArgCallback{fn_ptr, this_ptr};
}

template<auto mfp>
ArgCallback Devirtualize(mcl::class_type<decltype(mfp)>* this_) {
#if defined(__APPLE__) || defined(linux) || defined(__linux) || defined(__linux__)
    return DevirtualizeItanium<mfp>(this_);
#elif defined(__MINGW64__)
    return DevirtualizeItanium<mfp>(this_);
#elif defined(_WIN32)
    return DevirtualizeWindows<mfp>(this_);
#else
    return DevirtualizeGeneric<mfp>(this_);
#endif
}

/// Like Devirtualize, but the object pointer is read from the JIT state at state_offset when the
/// call runs. Every core passes an object of the same class, so the function is the same.
template<auto mfp>
ArgCallback DevirtualizeFromState(mcl::class_type<decltype(mfp)>* this_, size_t state_offset) {
    const ArgCallback callback = Devirtualize<mfp>(this_);
    // The object pointer must be passed unchanged (no this adjustment)
    ASSERT(callback.Arg() == reinterpret_cast<u64>(this_));
    return callback.WithArgFromState(state_offset);
}

}  // namespace Backend::X64
}  // namespace Dynarmic
