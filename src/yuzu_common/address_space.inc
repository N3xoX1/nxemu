// SPDX-FileCopyrightText: 2021 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_common/address_space.h"
#include "yuzu_common/yuzu_assert.h"

#define MAP_MEMBER(returnType)                                                                     \
    template <typename VaType, VaType UnmappedVa, typename PaType, PaType UnmappedPa,              \
              bool PaContigSplit, size_t AddressSpaceBits, typename ExtraBlockInfo>                \
    requires AddressSpaceValid<VaType, AddressSpaceBits> returnType FlatAddressSpaceMap<           \
        VaType, UnmappedVa, PaType, UnmappedPa, PaContigSplit, AddressSpaceBits, ExtraBlockInfo>
#define MAP_MEMBER_CONST()                                                                         \
    template <typename VaType, VaType UnmappedVa, typename PaType, PaType UnmappedPa,              \
              bool PaContigSplit, size_t AddressSpaceBits, typename ExtraBlockInfo>                \
    requires AddressSpaceValid<VaType, AddressSpaceBits> FlatAddressSpaceMap<                      \
        VaType, UnmappedVa, PaType, UnmappedPa, PaContigSplit, AddressSpaceBits, ExtraBlockInfo>

#define MM_MEMBER(returnType)                                                                      \
    template <typename VaType, VaType UnmappedVa, size_t AddressSpaceBits>                         \
    requires AddressSpaceValid<VaType, AddressSpaceBits> returnType                                \
        FlatMemoryManager<VaType, UnmappedVa, AddressSpaceBits>

#define ALLOC_MEMBER(returnType)                                                                   \
    template <typename VaType, VaType UnmappedVa, size_t AddressSpaceBits>                         \
    requires AddressSpaceValid<VaType, AddressSpaceBits> returnType                                \
        FlatAllocator<VaType, UnmappedVa, AddressSpaceBits>
#define ALLOC_MEMBER_CONST()                                                                       \
    template <typename VaType, VaType UnmappedVa, size_t AddressSpaceBits>                         \
    requires AddressSpaceValid<VaType, AddressSpaceBits>                                           \
        FlatAllocator<VaType, UnmappedVa, AddressSpaceBits>

namespace Common {
MAP_MEMBER_CONST()::FlatAddressSpaceMap(VaType va_limit_,
                                        std::function<void(VaType, VaType)> unmap_callback_)
    : va_limit{va_limit_}, unmap_callback{std::move(unmap_callback_)} {
    if (va_limit > VaMaximum) {
        ASSERT_MSG(false, "Invalid VA limit!");
    }
}

MAP_MEMBER(void)::MapLocked(VaType virt, PaType phys, VaType size, ExtraBlockInfo extra_info) {
    VaType virt_end{virt + size};

    if (virt_end > va_limit) {
        ASSERT_MSG(false,
                   "Trying to map a block past the VA limit: virt_end: 0x{:X}, va_limit: 0x{:X}",
                   virt_end, va_limit);
    }

    auto block_end_successor{std::lower_bound(blocks.begin(), blocks.end(), virt_end)};
    if (block_end_successor == blocks.begin()) {
        ASSERT_MSG(false, "Trying to map a block before the VA start: virt_end: 0x{:X}", virt_end);
    }

    auto block_end_predecessor{std::prev(block_end_successor)};

    if (block_end_successor != blocks.end()) {
        // We have blocks in front of us, if one is directly in front then we don't have to add a
        // tail
        if (block_end_successor->virt != virt_end) {
            PaType tailPhys{[&]() -> PaType {
                if constexpr (!PaContigSplit) {
                    // Always propagate unmapped regions rather than calculating offset
                    return block_end_predecessor->phys;
                } else {
                    if (block_end_predecessor->Unmapped()) {
                        // Always propagate unmapped regions rather than calculating offset
                        return block_end_predecessor->phys;
                    } else {
                        return block_end_predecessor->phys + virt_end - block_end_predecessor->virt;
                    }
                }
            }()};

            if (block_end_predecessor != blocks.begin() && block_end_predecessor->virt >= virt) {
                // If this block's start would be overlapped by the map then reuse it as a tail
                // block
                block_end_predecessor->virt = virt_end;
                block_end_predecessor->phys = tailPhys;
                block_end_predecessor->extra_info = block_end_predecessor->extra_info;

                // No longer predecessor anymore
                block_end_successor = block_end_predecessor--;
            } else {
                // Else insert a new one and we're done
                blocks.insert(block_end_successor,
                              {Block(virt, phys, extra_info),
                               Block(virt_end, tailPhys, block_end_predecessor->extra_info)});
                if (unmap_callback) {
                    unmap_callback(virt, size);
                }

                return;
            }
        }
    } else {
        // block_end_predecessor will always be unmapped as blocks has to be terminated by an
        // unmapped chunk
        if (block_end_predecessor != blocks.begin() && block_end_predecessor->virt >= virt) {
            // Move the unmapped block start backwards
            block_end_predecessor->virt = virt_end;

            // No longer predecessor anymore
            block_end_successor = block_end_predecessor--;
        } else {
            // Else insert a new one and we're done
            blocks.insert(block_end_successor,
                          {Block(virt, phys, extra_info), Block(virt_end, UnmappedPa, {})});
            if (unmap_callback) {
                unmap_callback(virt, size);
            }

            return;
        }
    }

    auto block_start_successor{block_end_successor};

    // Walk the block vector to find the start successor as this is more efficient than another
    // binary search in most scenarios
    while (std::prev(block_start_successor)->virt >= virt) {
        block_start_successor--;
    }

    // Check that the start successor is either the end block or something in between
    if (block_start_successor->virt > virt_end) {
        ASSERT_MSG(false, "Unsorted block in AS map: virt: 0x{:X}", block_start_successor->virt);
    } else if (block_start_successor->virt == virt_end) {
        // We need to create a new block as there are none spare that we would overwrite
        blocks.insert(block_start_successor, Block(virt, phys, extra_info));
    } else {
        // Erase overwritten blocks
        if (auto eraseStart{std::next(block_start_successor)}; eraseStart != block_end_successor) {
            blocks.erase(eraseStart, block_end_successor);
        }

        // Reuse a block that would otherwise be overwritten as a start block
        block_start_successor->virt = virt;
        block_start_successor->phys = phys;
        block_start_successor->extra_info = extra_info;
    }

    if (unmap_callback) {
        unmap_callback(virt, size);
    }
}

MAP_MEMBER(void)::UnmapLocked(VaType virt, VaType size) {
    VaType virt_end{virt + size};

    if (virt_end > va_limit) {
        ASSERT_MSG(false,
                   "Trying to map a block past the VA limit: virt_end: 0x{:X}, va_limit: 0x{:X}",
                   virt_end, va_limit);
    }

    auto block_end_successor{std::lower_bound(blocks.begin(), blocks.end(), virt_end)};
    if (block_end_successor == blocks.begin()) {
        ASSERT_MSG(false, "Trying to unmap a block before the VA start: virt_end: 0x{:X}",
                   virt_end);
    }

    auto block_end_predecessor{std::prev(block_end_successor)};

    auto walk_back_to_predecessor{[&](auto iter) {
        while (iter->virt >= virt) {
            iter--;
        }

        return iter;
    }};

    auto erase_blocks_with_end_unmapped{[&](auto unmappedEnd) {
        auto block_start_predecessor{walk_back_to_predecessor(unmappedEnd)};
        auto block_start_successor{std::next(block_start_predecessor)};

        auto eraseEnd{[&]() {
            if (block_start_predecessor->Unmapped()) {
                // If the start predecessor is unmapped then we can erase everything in our region
                // and be done
                return std::next(unmappedEnd);
            } else {
                // Else reuse the end predecessor as the start of our unmapped region then erase all
                // up to it
                unmappedEnd->virt = virt;
                return unmappedEnd;
            }
        }()};

        // We can't have two unmapped regions after each other
        if (eraseEnd != blocks.end() &&
            (eraseEnd == block_start_successor ||
             (block_start_predecessor->Unmapped() && eraseEnd->Unmapped()))) {
            ASSERT_MSG(false, "Multiple contiguous unmapped regions are unsupported!");
        }

        blocks.erase(block_start_successor, eraseEnd);
    }};

    // We can avoid any splitting logic if these are the case
    if (block_end_predecessor->Unmapped()) {
        if (block_end_predecessor->virt > virt) {
            erase_blocks_with_end_unmapped(block_end_predecessor);
        }

        if (unmap_callback) {
            unmap_callback(virt, size);
        }

        return; // The region is unmapped, bail out early
    } else if (block_end_successor->virt == virt_end && block_end_successor->Unmapped()) {
        erase_blocks_with_end_unmapped(block_end_successor);

        if (unmap_callback) {
            unmap_callback(virt, size);
        }

        return; // The region is unmapped here and doesn't need splitting, bail out early
    } else if (block_end_successor == blocks.end()) {
        // This should never happen as the end should always follow an unmapped block
        ASSERT_MSG(false, "Unexpected Memory Manager state!");
    } else if (block_end_successor->virt != virt_end) {
        // If one block is directly in front then we don't have to add a tail

        // The previous block is mapped so we will need to add a tail with an offset
        PaType tailPhys{[&]() {
            if constexpr (PaContigSplit) {
                return block_end_predecessor->phys + virt_end - block_end_predecessor->virt;
            } else {
                return block_end_predecessor->phys;
            }
        }()};

        if (block_end_predecessor->virt >= virt) {
            // If this block's start would be overlapped by the unmap then reuse it as a tail block
            block_end_predecessor->virt = virt_end;
            block_end_predecessor->phys = tailPhys;

            // No longer predecessor anymore
            block_end_successor = block_end_predecessor--;
        } else {
            blocks.insert(block_end_successor,
                          {Block(virt, UnmappedPa, {}),
                           Block(virt_end, tailPhys, block_end_predecessor->extra_info)});
            if (unmap_callback) {
                unmap_callback(virt, size);
            }

            // The previous block is mapped and ends before
            return;
        }
    }

    // Walk the block vector to find the start predecessor as this is more efficient than another
    // binary search in most scenarios
    auto block_start_predecessor{walk_back_to_predecessor(block_end_successor)};
    auto block_start_successor{std::next(block_start_predecessor)};

    if (block_start_successor->virt > virt_end) {
        ASSERT_MSG(false, "Unsorted block in AS map: virt: 0x{:X}", block_start_successor->virt);
    } else if (block_start_successor->virt == virt_end) {
        // There are no blocks between the start and the end that would let us skip inserting a new
        // one for head

        // The previous block is may be unmapped, if so we don't need to insert any unmaps after it
        if (block_start_predecessor->Mapped()) {
            blocks.insert(block_start_successor, Block(virt, UnmappedPa, {}));
        }
    } else if (block_start_predecessor->Unmapped()) {
        // If the previous block is unmapped it covers the region, so erase every block inside it
        // (Lichii #1896: erasing up to block_end_predecessor left the last block mapped)
        blocks.erase(block_start_successor, block_end_successor);
    } else {
        // Erase overwritten blocks, skipping the first one as we have written the unmapped start
        // block there
        if (auto eraseStart{std::next(block_start_successor)}; eraseStart != block_end_successor) {
            blocks.erase(eraseStart, block_end_successor);
        }

        // Add in the unmapped block header
        block_start_successor->virt = virt;
        block_start_successor->phys = UnmappedPa;
    }

    if (unmap_callback)
        unmap_callback(virt, size);
}

ALLOC_MEMBER_CONST()::FlatAllocator(VaType virt_start_, VaType va_limit_)
    : Base{va_limit_}, virt_start{virt_start_}, current_linear_alloc_end{virt_start_} {}

ALLOC_MEMBER(VaType)::Allocate(VaType size) {
    std::scoped_lock lock(this->block_mutex);

    // First free range of the size at or after `from`; the last block reaches the VA limit.
    // Lichii #1896: the old skip loop could return a mapped range, or nothing with space free.
    const auto find_free = [&](VaType from) -> VaType {
        const auto starts_after = [](VaType virt, const auto& b) { return virt < b.virt; };
        auto block{std::upper_bound(this->blocks.begin(), this->blocks.end(), from, starts_after)};
        if (block == this->blocks.begin()) {
            return UnmappedVa;
        }
        for (block = std::prev(block); block != this->blocks.end(); ++block) {
            const auto next{std::next(block)};
            const VaType end{next == this->blocks.end() ? this->va_limit : next->virt};
            const VaType start{std::max(block->virt, from)};
            if (block->Unmapped() && start < end && end - start >= size) {
                return start;
            }
        }
        return UnmappedVa;
    };

    // Continue after the previous allocation, then look for a gap from the start of the space.
    VaType alloc_start{find_free(current_linear_alloc_end)};
    if (alloc_start == UnmappedVa) {
        alloc_start = find_free(virt_start);
    }
    if (alloc_start == UnmappedVa) {
        return {}; // AS is full
    }

    current_linear_alloc_end = alloc_start + size;
    this->MapLocked(alloc_start, true, size, {});
    return alloc_start;
}

ALLOC_MEMBER(void)::AllocateFixed(VaType virt, VaType size) {
    this->Map(virt, true, size);
}

ALLOC_MEMBER(void)::Free(VaType virt, VaType size) {
    std::scoped_lock lock(this->block_mutex);
    this->UnmapLocked(virt, size);
    // Freeing the newest allocation lets the next one take its place
    if (virt + size == current_linear_alloc_end) {
        current_linear_alloc_end = virt;
    }
}
} // namespace Common
