#pragma once

// fmt uses compile-time conditions that MSVC reports as C4127.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4127) // conditional expression is constant
#endif
#include <fmt/format.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
