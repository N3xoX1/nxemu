#pragma once

// Fake Android exports exercise the production runtime resolver on desktop CI.
#include <cstring>

namespace TestAdpf {
inline bool library_available = true;
inline const char* missing_symbol = "";
void* Symbol(const char* name);
}

namespace Common {
class DynamicLibrary {
public:
    explicit DynamicLibrary(const char*) {}
    bool IsOpen() const { return TestAdpf::library_available; }
    template <typename T>
    bool GetSymbol(const char* name, T* value) const {
        *value = std::strcmp(name, TestAdpf::missing_symbol) == 0
                     ? nullptr : reinterpret_cast<T>(TestAdpf::Symbol(name));
        return *value != nullptr;
    }
};
}
