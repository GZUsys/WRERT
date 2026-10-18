#ifndef NVMKV_WRERT_RWLOCK_H
#define NVMKV_WRERT_RWLOCK_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <immintrin.h>

#ifndef WRERT_LOCK_POOL_BITS
#define WRERT_LOCK_POOL_BITS 16
#endif

#ifndef WRERT_LOCK_CACHELINE
#define WRERT_LOCK_CACHELINE 64
#endif

namespace wrert {

class WRERTSpinRWLock {
private:
    std::atomic<uint32_t> state_{0};
    static constexpr uint32_t WRITER_FLAG = 0x80000000u;
    static constexpr uint32_t READER_MASK = 0x7FFFFFFFu;
    static constexpr unsigned MAX_BACKOFF = 64;

public:
    WRERTSpinRWLock() = default;
    WRERTSpinRWLock(const WRERTSpinRWLock&) = delete;
    WRERTSpinRWLock& operator=(const WRERTSpinRWLock&) = delete;

    void init() noexcept {
        state_.store(0, std::memory_order_relaxed);
    }
    void read_lock() noexcept {
        unsigned backoff = 1;
        while (true) {
            uint32_t cur = state_.load(std::memory_order_relaxed);
            if (__builtin_expect((cur & WRITER_FLAG) == 0, 1)) {
                if (__builtin_expect(state_.compare_exchange_weak(cur, cur + 1, std::memory_order_acquire, std::memory_order_relaxed), 1)) {
                    return;
                }
                continue;
            }
            // for (unsigned i = 0; i < backoff; ++i) {
            // _mm_pause();
            // }
            // if (__builtin_expect(backoff < MAX_BACKOFF, 1)) {
            //     backoff <<= 1;
            // }
            _mm_pause();
        }
    }
    void read_unlock() noexcept {
        state_.fetch_sub(1, std::memory_order_release);
    }
    void write_lock() noexcept {
        unsigned backoff = 1;
        while (true) {
            uint32_t cur = state_.load(std::memory_order_relaxed);
            if (__builtin_expect((cur & WRITER_FLAG) != 0, 0)) {
                // for (unsigned i = 0; i < backoff; ++i) _mm_pause();
                // if (__builtin_expect(backoff < MAX_BACKOFF, 1))
                //     backoff <<= 1;
                _mm_pause();
                continue;
            }

            uint32_t prev = state_.fetch_or(WRITER_FLAG, std::memory_order_acq_rel);
            if (__builtin_expect((prev & WRITER_FLAG) == 0, 1)) {
                break;
            }

            // for (unsigned i = 0; i < backoff; ++i) _mm_pause();
            // if (__builtin_expect(backoff < MAX_BACKOFF, 1))
            //     backoff <<= 1;
            _mm_pause();
        }
        // backoff = 1;
        while (true) {
            uint32_t cur = state_.load(std::memory_order_acquire);
            if (__builtin_expect((cur & READER_MASK) == 0, 1)) {
                return;
            }
            // for (unsigned i = 0; i < backoff; ++i) _mm_pause();
            // if (__builtin_expect(backoff < MAX_BACKOFF, 1))
            //     backoff <<= 1;
            _mm_pause();
        }
    }
    void write_unlock() noexcept {
        state_.fetch_and(~WRITER_FLAG, std::memory_order_release);
    }
};

struct alignas(WRERT_LOCK_CACHELINE) PaddedWRERTSpinRWLock {
    WRERTSpinRWLock lock;
    PaddedWRERTSpinRWLock() noexcept = default;
    PaddedWRERTSpinRWLock(const PaddedWRERTSpinRWLock&) = delete;
    PaddedWRERTSpinRWLock& operator=(const PaddedWRERTSpinRWLock&) = delete;
};

class WRERTLockPool {
private:
    static constexpr size_t POOL_SIZE = 1ULL << WRERT_LOCK_POOL_BITS;

    std::unique_ptr<PaddedWRERTSpinRWLock[]> directory_pool_;
    std::unique_ptr<PaddedWRERTSpinRWLock[]> segment_pool_;
    std::unique_ptr<PaddedWRERTSpinRWLock[]> bucket_pool_;

    static inline size_t hash_ptr(const void* p) noexcept {
        uint64_t x = reinterpret_cast<uintptr_t>(p);
        return (x * 11400714819323198485ULL) >> (64 - WRERT_LOCK_POOL_BITS);
    }

public:
    WRERTLockPool() : directory_pool_(new PaddedWRERTSpinRWLock[POOL_SIZE]),
                      segment_pool_(new PaddedWRERTSpinRWLock[POOL_SIZE]),
                      bucket_pool_(new PaddedWRERTSpinRWLock[POOL_SIZE]) {}

    WRERTLockPool(const WRERTLockPool&) = delete;
    WRERTLockPool& operator=(const WRERTLockPool&) = delete;
    WRERTSpinRWLock& directory(const void* ptr) noexcept {
        return directory_pool_[hash_ptr(ptr)].lock;
    }
    WRERTSpinRWLock& segment(const void* ptr) noexcept {
        return segment_pool_[hash_ptr(ptr)].lock;
    }
    WRERTSpinRWLock& bucket(const void* ptr) noexcept {
        return bucket_pool_[hash_ptr(ptr)].lock;
    }
};

inline WRERTLockPool& locks() noexcept {
    static WRERTLockPool instance;
    return instance;
}

} // namespace wrert

#endif // NVMKV_WRERT_RWLOCK_H
