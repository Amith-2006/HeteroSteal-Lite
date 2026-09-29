// include/heterosteal/correlation_table.hpp
#pragma once
#include "object_pool.hpp"

namespace hs {

// CorrelationTable provides helper functions documenting and wrapping
// TaskPool's generational encoding/decoding and resolution logic (spec §4.2, §11.1).
// In HeteroSteal, the TaskPool slab doubles as the correlation table:
//   - SQE user_data = (generation << 32) | slot_index
//   - CQE user_data is decoded and checked against current generation to detect stale completions.

struct CorrelationHandle {
    uint32_t slot_index{0};
    uint32_t generation{0};

    constexpr uint64_t to_u64() const noexcept {
        return TaskPool::encode_handle(slot_index, generation);
    }

    static constexpr CorrelationHandle from_u64(uint64_t handle) noexcept {
        CorrelationHandle ch;
        TaskPool::decode_handle(handle, ch.slot_index, ch.generation);
        return ch;
    }
};

inline TaskDescriptor* resolve_completion(TaskPool& pool, uint64_t user_data) {
    return pool.resolve(user_data);
}

inline CompletionGuard resolve_completion_guarded(TaskPool& pool, uint64_t user_data) {
    return pool.resolve_guarded(user_data);
}

} // namespace hs
