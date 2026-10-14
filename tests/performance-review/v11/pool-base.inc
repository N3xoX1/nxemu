size_t Pool::Baseline() {
    // Refresh semaphore to query updated results
    master_semaphore->Refresh();
    const u64 gpu_tick = master_semaphore->KnownGpuTick();
    const auto search = [this, gpu_tick](size_t begin, size_t end) -> std::optional<size_t> {
        for (size_t iterator = begin; iterator < end; ++iterator) {
            if (gpu_tick >= ticks[iterator]) {
                ticks[iterator] = master_semaphore->CurrentTick();
                return iterator;
            }
        }
        return std::nullopt;
    };
    // Try to find a free resource from the hinted position to the end.
    std::optional<size_t> found = search(hint_iterator, ticks.size());
    if (!found) {
        // Search from beginning to the hinted position.
        found = search(0, hint_iterator);
        if (!found) {
            // Both searches failed, the pool is full; handle it.
            const size_t free_resource = ManageOverflow();

            ticks[free_resource] = master_semaphore->CurrentTick();
            found = free_resource;
        }
    }
    // Free iterator is hinted to the resource after the one that's been committed.
    hint_iterator = (*found + 1) % ticks.size();
    return *found;
}