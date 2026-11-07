// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <stdexcept>

#include "yuzu_shader_recompiler/object_pool.h"

namespace
{

struct OwnedObject
{
    static inline int constructed = 0;
    static inline int destroyed = 0;

    explicit OwnedObject(bool fail = false)
    {
        if (fail)
        {
            throw std::runtime_error("test constructor failure");
        }
        value = std::make_unique<int>(42);
        ++constructed;
    }

    ~OwnedObject()
    {
        ++destroyed;
    }

    std::unique_ptr<int> value;
};

bool Run()
{
    {
        Shader::ObjectPool<OwnedObject> pool{2};
        if (!pool.Create()->value || !pool.Create()->value)
        {
            return false;
        }
        // A full root chunk triggers a replacement allocation at release.
        pool.ReleaseContents();
        if (OwnedObject::destroyed != 2)
        {
            return false;
        }

        if (!pool.Create()->value)
        {
            return false;
        }
        try
        {
            [[maybe_unused]] auto * unused = pool.Create(true);
            return false;
        }
        catch (const std::runtime_error &)
        {
        }
        // Failed constructions must not count as live pooled objects.
        pool.ReleaseContents();
        if (OwnedObject::destroyed != 3)
        {
            return false;
        }

        // Exercise several chunks, move/resize and the non-full-root path.
        for (int i = 0; i < 7; ++i)
        {
            if (!pool.Create()->value)
            {
                return false;
            }
        }
        pool.ReleaseContents();
        if (OwnedObject::destroyed != 10)
        {
            return false;
        }
        if (!pool.Create()->value)
        {
            return false;
        }
    }
    return OwnedObject::constructed == OwnedObject::destroyed &&
           OwnedObject::constructed == 11;
}

} // namespace

bool FailedNewChunk()
{
    Shader::ObjectPool<OwnedObject> pool{1};
    const int before = OwnedObject::destroyed;
    auto * original = pool.Create();
    try
    {
        [[maybe_unused]] auto * unused = pool.Create(true);
        return false;
    }
    catch (const std::runtime_error &)
    {
    }
    // A failed first construction in the newly allocated chunk leaves the old
    // object live; the next construction must reuse the unpublished slot.
    if (!original->value || !pool.Create()->value) return false;
    pool.ReleaseContents();
    pool.ReleaseContents();
    return OwnedObject::destroyed == before + 2;
}

int main()
{
    return Run() && FailedNewChunk() &&
                   OwnedObject::constructed == OwnedObject::destroyed
               ? 0
               : 1;
}
