// src/deque.cpp
#include "heterosteal/deque.hpp"

namespace hs {

namespace {

size_t next_power_of_two(size_t v) {
    if (v < 2) return 2;
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

} // namespace

WorkStealingDeque::Array::Array(int64_t cap)
    : capacity(cap),
      mask(cap - 1),
      buffer(new std::atomic<TaskDescriptor*>[cap]) {
    for (int64_t i = 0; i < cap; ++i) {
        buffer[i].store(nullptr, std::memory_order_relaxed);
    }
}

WorkStealingDeque::Array::~Array() {
    delete[] buffer;
}

WorkStealingDeque::WorkStealingDeque(size_t initial_capacity_pow2) {
    size_t cap = next_power_of_two(initial_capacity_pow2);
    Array* arr = new Array(static_cast<int64_t>(cap));
    array_.store(arr, std::memory_order_relaxed);
    top_.store(0, std::memory_order_relaxed);
    bottom_.store(0, std::memory_order_relaxed);
}

WorkStealingDeque::~WorkStealingDeque() {
    Array* current = array_.load(std::memory_order_relaxed);
    delete current;

    for (Array* old_arr : retired_arrays_) {
        delete old_arr;
    }
    retired_arrays_.clear();
}

size_t WorkStealingDeque::capacity() const {
    Array* a = array_.load(std::memory_order_acquire);
    return a ? static_cast<size_t>(a->capacity) : 0;
}

int64_t WorkStealingDeque::size_approx() const {
    int64_t b = bottom_.load(std::memory_order_relaxed);
    int64_t t = top_.load(std::memory_order_relaxed);
    int64_t sz = b - t;
    return sz > 0 ? sz : 0;
}

WorkStealingDeque::Array* WorkStealingDeque::resize(Array* old_array, int64_t b, int64_t t) {
    int64_t new_cap = old_array->capacity * 2;
    Array* new_array = new Array(new_cap);

    for (int64_t i = t; i < b; ++i) {
        TaskDescriptor* item = old_array->buffer[i & old_array->mask].load(std::memory_order_relaxed);
        new_array->buffer[i & new_array->mask].store(item, std::memory_order_relaxed);
    }

    retired_arrays_.push_back(old_array);
    array_.store(new_array, std::memory_order_release);
    return new_array;
}

void WorkStealingDeque::push_bottom(TaskDescriptor* task) {
    int64_t b = bottom_.load(std::memory_order_relaxed);
    int64_t t = top_.load(std::memory_order_acquire);
    Array* a = array_.load(std::memory_order_relaxed);

    if (b - t >= a->capacity) {
        a = resize(a, b, t);
    }

    a->buffer[b & a->mask].store(task, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    bottom_.store(b + 1, std::memory_order_relaxed);
}

std::optional<TaskDescriptor*> WorkStealingDeque::pop_bottom() {
    int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    Array* a = array_.load(std::memory_order_relaxed);
    bottom_.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t t = top_.load(std::memory_order_relaxed);

    if (t <= b) {
        /* Non-empty deque */
        if (t == b) {
            /* Single-last element race against thieves */
            if (!top_.compare_exchange_strong(t, t + 1,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed)) {
                /* Lost race to a thief */
                bottom_.store(b + 1, std::memory_order_relaxed);
                return std::nullopt;
            }
            TaskDescriptor* item = a->buffer[b & a->mask].load(std::memory_order_relaxed);
            bottom_.store(b + 1, std::memory_order_relaxed);
            return item;
        } else {
            /* More than one element left; owner does not compete with thieves */
            TaskDescriptor* item = a->buffer[b & a->mask].load(std::memory_order_relaxed);
            return item;
        }
    } else {
        /* Empty deque */
        bottom_.store(b + 1, std::memory_order_relaxed);
        return std::nullopt;
    }
}

std::optional<TaskDescriptor*> WorkStealingDeque::steal_top() {
    int64_t t = top_.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t b = bottom_.load(std::memory_order_acquire);

    if (t < b) {
        /* Non-empty deque */
        Array* a = array_.load(std::memory_order_acquire);
        TaskDescriptor* item = a->buffer[t & a->mask].load(std::memory_order_relaxed);

        if (!top_.compare_exchange_strong(t, t + 1,
                                          std::memory_order_seq_cst,
                                          std::memory_order_relaxed)) {
            /* Lost race to another thief or owner pop */
            return std::nullopt;
        }
        return item;
    }

    return std::nullopt;
}

} // namespace hs
