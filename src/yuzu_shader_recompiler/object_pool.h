// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace Shader {

template <typename T>
    requires std::is_destructible_v<T>
class ObjectPool {
public:
    explicit ObjectPool(size_t chunk_size = 8192) : new_chunk_size{chunk_size} {
        node = &chunks.emplace_back(new_chunk_size);
    }

    template <typename... Args>
        requires std::is_constructible_v<T, Args...>
    [[nodiscard]] T* Create(Args&&... args) {
        Chunk* const chunk{FreeChunk()};
        T* const object{std::construct_at(&chunk->storage[chunk->used_objects].object,
                                        std::forward<Args>(args)...)};
        // Publish the slot only after construction succeeded. Release() must never
        // destroy a slot whose constructor threw.
        ++chunk->used_objects;
        return object;
    }

    void ReleaseContents() {
        if (chunks.empty()) {
            return;
        }
        Chunk& root{chunks.front()};
        if (root.used_objects == root.num_objects) {
            // Root chunk has been filled, squash allocations into it
            const size_t total_objects{root.num_objects + new_chunk_size * (chunks.size() - 1)};
            // Allocate before releasing live objects so an allocation failure leaves
            // the pool usable and node pointing to an existing chunk.
            Chunk replacement{total_objects};
            chunks.clear();
            chunks.emplace_back(std::move(replacement));
        } else {
            root.Release();
            chunks.resize(1);
        }
        chunks.shrink_to_fit();
        node = &chunks.front();
    }

private:
    struct NonTrivialDummy {
        NonTrivialDummy() noexcept {}
    };

    union Storage {
        Storage() noexcept {}
        ~Storage() noexcept {}

        NonTrivialDummy dummy{};
        T object;
    };

    struct Chunk {
        explicit Chunk() = default;
        explicit Chunk(size_t size)
            : num_objects{size}, storage{std::make_unique<Storage[]>(size)} {}

        Chunk& operator=(Chunk&& rhs) noexcept {
            Release();
            used_objects = std::exchange(rhs.used_objects, 0);
            num_objects = std::exchange(rhs.num_objects, 0);
            storage = std::move(rhs.storage);
            return *this;
        }

        Chunk(Chunk&& rhs) noexcept
            : used_objects{std::exchange(rhs.used_objects, 0)},
              num_objects{std::exchange(rhs.num_objects, 0)}, storage{std::move(rhs.storage)} {}

        ~Chunk() {
            Release();
        }

        void Release() {
            // Storage is a union with an intentionally empty destructor. Destroy
            // the constructed T objects to release their owned IR resources.
            for (size_t index = 0; index < used_objects; ++index) {
                std::destroy_at(&storage[index].object);
            }
            used_objects = 0;
        }

        size_t used_objects{};
        size_t num_objects{};
        std::unique_ptr<Storage[]> storage;
    };

    [[nodiscard]] Chunk* FreeChunk() {
        if (node->used_objects != node->num_objects) {
            return node;
        }
        node = &chunks.emplace_back(new_chunk_size);
        return node;
    }

    Chunk* node{};
    std::vector<Chunk> chunks;
    size_t new_chunk_size{};
};

} // namespace Shader
