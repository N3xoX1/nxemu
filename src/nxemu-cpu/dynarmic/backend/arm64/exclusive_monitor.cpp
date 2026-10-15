/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/interface/exclusive_monitor.h"

#include <algorithm>

#include <mcl/assert.hpp>

namespace Dynarmic {

namespace {

// Exclusive reads do not take the lock, so the addresses are accessed atomically.
VAddr LoadAddress(VAddr& address) {
    return std::atomic_ref<VAddr>{address}.load();
}

void StoreAddress(VAddr& address, VAddr value) {
    std::atomic_ref<VAddr>{address}.store(value);
}

}  // namespace

ExclusiveMonitor::ExclusiveMonitor(size_t processor_count)
        : exclusive_addresses(processor_count, INVALID_EXCLUSIVE_ADDRESS), exclusive_values(processor_count) {}

ExclusiveMonitor::~ExclusiveMonitor()
{
}

size_t ExclusiveMonitor::GetProcessorCount() const {
    return exclusive_addresses.size();
}

void ExclusiveMonitor::Lock() {
    lock.Lock();
}

void ExclusiveMonitor::Unlock() {
    lock.Unlock();
}

std::uint32_t ExclusiveMonitor::BeginMark(size_t processor_id, VAddr masked_address) {
    std::uint32_t sequence = operation_sequence.load(std::memory_order_acquire);
    while ((sequence & 1) != 0) {
        sequence = operation_sequence.load(std::memory_order_acquire);
    }
    StoreAddress(exclusive_addresses[processor_id], masked_address);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return sequence;
}

bool ExclusiveMonitor::EndMark(std::uint32_t sequence) {
    // An exclusive operation that started after the address was marked clears the mark itself.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return operation_sequence.load(std::memory_order_relaxed) == sequence;
}

bool ExclusiveMonitor::CheckAndClear(size_t processor_id, VAddr address) {
    const VAddr masked_address = address & RESERVATION_GRANULE_MASK;

    Lock();
    if (LoadAddress(exclusive_addresses[processor_id]) != masked_address) {
        Unlock();
        return false;
    }

    operation_sequence.fetch_add(1);
    for (VAddr& other_address : exclusive_addresses) {
        if (LoadAddress(other_address) == masked_address) {
            StoreAddress(other_address, INVALID_EXCLUSIVE_ADDRESS);
        }
    }
    return true;
}

void ExclusiveMonitor::EndExclusiveOperation() {
    operation_sequence.fetch_add(1, std::memory_order_release);
    Unlock();
}

void ExclusiveMonitor::Clear() {
    Lock();
    for (VAddr& address : exclusive_addresses) {
        StoreAddress(address, INVALID_EXCLUSIVE_ADDRESS);
    }
    Unlock();
}

void ExclusiveMonitor::ClearProcessor(size_t processor_id) {
    Lock();
    StoreAddress(exclusive_addresses[processor_id], INVALID_EXCLUSIVE_ADDRESS);
    Unlock();
}

}  // namespace Dynarmic
