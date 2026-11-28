// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

// metal-cpp is header only. Exactly one translation unit in the program must
// define the *_PRIVATE_IMPLEMENTATION macros before including these; that unit
// is metal_common/metal_impl.cpp. Everywhere else just includes this header.
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <MetalFX/MetalFX.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "yuzu_common/common_types.h"

namespace Metal {

/// Owning handle for a Metal/Foundation object. metal-cpp objects are
/// reference counted the same way their Objective-C counterparts are, so this
/// is just NS::SharedPtr under a name that matches how we use it.
template <typename T>
using Owned = NS::SharedPtr<T>;

/// Takes ownership of an object returned by a `new*`/`alloc` style call
/// (+1 retain count) without retaining it again.
template <typename T>
[[nodiscard]] Owned<T> Adopt(T* object) noexcept {
    return object != nullptr ? NS::TransferPtr(object) : Owned<T>{};
}

/// Retains an object we do not own (a `+0` autoreleased return value). The null
/// check matters: NS::RetainPtr dereferences unguarded, and callers legitimately
/// pass null (nextDrawable() returns null when the window is occluded).
template <typename T>
[[nodiscard]] Owned<T> Retain(T* object) noexcept {
    return object != nullptr ? NS::RetainPtr(object) : Owned<T>{};
}

/// Thrown on unrecoverable Metal errors. RendererBase construction is wrapped
/// in a try/catch by VideoCore::CreateGPU, which reports it and falls back.
class Exception final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Wraps a string in an autoreleased NS::String. The result is only valid
/// until the enclosing autorelease pool drains, which is fine for the
/// label/source arguments this exists for.
[[nodiscard]] inline NS::String* ToNSString(std::string_view string) {
    return NS::String::string(std::string{string}.c_str(), NS::UTF8StringEncoding);
}

/// Converts an NS::String back to a std::string. Returns an empty string for
/// null input, which Metal does hand out for unset labels.
[[nodiscard]] inline std::string FromNSString(const NS::String* string) {
    if (string == nullptr) {
        return {};
    }
    const char* utf8 = string->utf8String();
    return utf8 != nullptr ? std::string{utf8} : std::string{};
}

/// Extracts a human readable message out of an NS::Error, for logging.
[[nodiscard]] inline std::string DescribeError(const NS::Error* error) {
    if (error == nullptr) {
        return "no error";
    }
    // Domain and code as well as the message: MTL4CommandQueueErrorDomain's
    // localized description is the same useless sentence whatever went wrong,
    // and the code is what distinguishes a watchdog timeout from a memory fault.
    std::string described = fmt::format("{} [{} code {}]",
                                        FromNSString(error->localizedDescription()),
                                        FromNSString(error->domain()), error->code());

    // userInfo is where Metal puts the detail that matters, which resource
    // faulted, which encoder, the failure reason, and localizedDescription drops
    // all of it. Values go through NSObject::description instead of being type
    // tested, since metal-cpp builds without RTTI.
    NS::Error* const mutable_error = const_cast<NS::Error*>(error);
    if (NS::Dictionary* const info = mutable_error->userInfo();
        info != nullptr && info->count() > 0) {
        described += " {";
        NS::Enumerator<NS::Object>* const keys = info->keyEnumerator();
        bool first = true;
        while (NS::Object* const key = keys->nextObject()) {
            described += first ? "" : ", ";
            first = false;
            described += FromNSString(key->description());
            if (NS::Object* const value = info->object(key); value != nullptr) {
                described += "=" + FromNSString(value->description());
            }
        }
        described += "}";
    }
    return described;
}

/// Scoped autorelease pool. Metal returns autoreleased objects (notably
/// nextDrawable), so anything per-frame or in a loop needs one or it leaks
/// until the thread unwinds.
class ScopedAutoreleasePool {
public:
    ScopedAutoreleasePool() : pool{NS::AutoreleasePool::alloc()->init()} {}

    ~ScopedAutoreleasePool() {
        if (pool != nullptr) {
            pool->release();
        }
    }

    ScopedAutoreleasePool(const ScopedAutoreleasePool&) = delete;
    ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;

    ScopedAutoreleasePool(ScopedAutoreleasePool&& other) noexcept
        : pool{std::exchange(other.pool, nullptr)} {}

    ScopedAutoreleasePool& operator=(ScopedAutoreleasePool&& other) noexcept {
        std::swap(pool, other.pool);
        return *this;
    }

private:
    NS::AutoreleasePool* pool;
};

} // namespace Metal
