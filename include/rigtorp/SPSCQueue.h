/*
Copyright (c) 2020 Erik Rigtorp <erik@rigtorp.se>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory> // std::allocator
#include <new>    // std::hardware_destructive_interference_size
#include <stdexcept>
#include <type_traits> // std::enable_if, std::is_*_constructible

#ifdef __has_cpp_attribute
#if __has_cpp_attribute(nodiscard)
#define RIGTORP_NODISCARD [[nodiscard]]
#endif
#endif
#ifndef RIGTORP_NODISCARD
#define RIGTORP_NODISCARD
#endif

namespace rigtorp {

template <typename T, typename Allocator = std::allocator<T>> class SPSCQueue {

#if defined(__cpp_if_constexpr) && defined(__cpp_lib_void_t)
  template <typename Alloc2, typename = void>
  struct has_allocate_at_least : std::false_type {};

  template <typename Alloc2>
  struct has_allocate_at_least<
      Alloc2, std::void_t<typename Alloc2::value_type,
                          decltype(std::declval<Alloc2 &>().allocate_at_least(
                              size_t{}))>> : std::true_type {};
#endif

public:
  explicit SPSCQueue(const size_t capacity,
                     const size_t max_allocation_length=1,
                     const Allocator &allocator = Allocator())
      : capacity_(capacity), allocator_(allocator) {
    // The queue needs at least one element
    if (capacity_ < 1) {
      capacity_ = 1;
    }
    capacity_++; // Needs one slack element
    // Prevent overflowing size_t
    if (capacity_ > SIZE_MAX - 2 * kPadding) {
      capacity_ = SIZE_MAX - 2 * kPadding;
    }

    assert(capacity_ > max_allocation_length &&
         "Can only construct a queue with capacity larger than max_allocation_length");
    capacityMargin_ = capacity_ - max_allocation_length;
    maxAllocationLength_ = max_allocation_length;

    //wrapCapacity_.load(capacity_);

#if defined(__cpp_if_constexpr) && defined(__cpp_lib_void_t)
    if constexpr (has_allocate_at_least<Allocator>::value) {
      auto res = allocator_.allocate_at_least(capacity_ + 2 * kPadding);
      slots_ = res.ptr;
      capacity_ = res.count - 2 * kPadding;
    } else {
      slots_ = std::allocator_traits<Allocator>::allocate(
          allocator_, capacity_ + 2 * kPadding);
    }
#else
    slots_ = std::allocator_traits<Allocator>::allocate(
        allocator_, capacity_ + 2 * kPadding);
#endif

    static_assert(alignof(SPSCQueue<T>) == kCacheLineSize, "");
    static_assert(sizeof(SPSCQueue<T>) >= 3 * kCacheLineSize, "");
    assert(reinterpret_cast<char *>(&readIdx_) -
               reinterpret_cast<char *>(&writeIdx_) >=
           static_cast<std::ptrdiff_t>(kCacheLineSize));
  }

  ~SPSCQueue() {
    while (front()) {
      pop();
    }
    std::allocator_traits<Allocator>::deallocate(allocator_, slots_,
                                                 capacity_ + 2 * kPadding);
  }

  // non-copyable and non-movable
  SPSCQueue(const SPSCQueue &) = delete;
  SPSCQueue &operator=(const SPSCQueue &) = delete;

  T* allocate_n(size_t n_items) {
    auto writeIdx = writeIdx_.load(std::memory_order_relaxed);
    //n_items++; // it will need 1 item to save the number of following bytes
    // it's not const as there may be the case when writeIdx is shifted to 0

    if (n_items >= maxAllocationLength_) {
      throw std::runtime_error("SPSCCoord::allocate_n requested too large size: " + std::to_string(n_items) + " > " + std::to_string(maxAllocationLength_));
      //nDrops_++;
      //return nullptr;
    } // TODO: maybe modify to not throw?

    //if (writeIdx >= capacity_) {
    //  throw std::runtime_error("SPSCCoord::allocate_n overflow!");
    //}

    allocateNextWriteIdxCache_ = writeIdx+n_items;
    // the case when it does not fit to the end of the buffer margin
    // do allocate the current pointer
    // but the next one must roll over
    if (allocateNextWriteIdxCache_ > capacityMargin_) {
      //// wait to clear enough space to write from the 0 index
      ////auto nextWriteIdx = writeIdx + 1;
      //while ((0+n_items) >= readIdxCache_) {
      //  readIdxCache_ = readIdx_.load(std::memory_order_acquire);
      //}
      //writeIdx = 0;
      ////writeIdx_.store(0, std::memory_order_relaxed);
      ////wrapCapacity_.load(capacity_ - n_items);
      allocateNextWriteIdxCache_ = 0;
    }

    // what if it's an inverted topology and the read pointer is ahead?
    // the space must be cleared before the user can use it
    //if (readIdxCache_ >= writeIdx && n_items > (readIdxCache_ - writeIdx)) {
    //  // wait until the read clears enough
    //  readIdxCache_ = readIdx_.load(std::memory_order_acquire);
    //  while (readIdxCache_ >= writeIdx && n_items > (readIdxCache_ - writeIdx)) {
    //    readIdxCache_ = readIdx_.load(std::memory_order_acquire);
    //  }
    //}
    // it probably only happens like this
    while (allocateNextWriteIdxCache_ == readIdxCache_) {
      readIdxCache_ = readIdx_.load(std::memory_order_acquire);
    }

    //if (nextWriteIdx == capacity_) {
    //  nextWriteIdx = 0;
    //}
    //while (nextWriteIdx == readIdxCache_) {
    //  readIdxCache_ = readIdx_.load(std::memory_order_acquire);
    //}

    auto placement_ptr = &slots_[writeIdx + kPadding];
    // new (placement_ptr) T(std::forward<Args>(args)...);

    return placement_ptr;
    //std::cout << "SPSCQueue::emplace writeIdx=" << std::dec << writeIdx << " writeIdx+kPadding=" << writeIdx + kPadding << " placement_ptr=" << std::hex << placement_ptr << std::endl;
  }

  void allocate_store() noexcept {
    // assume the coord comes from allocate_n
    // the coord really must be a unique_ptr of the current index
    // i.e. coord.index == writeIdx_
    // and this can be a fetch_add
    //writeIdx_.fetch_add(n_items, std::memory_order_release);
    //while (allocateNextWriteIdxCache_ == readIdxCache_) {
    //  readIdxCache_ = readIdx_.load(std::memory_order_acquire);
    //}
    writeIdx_.store(allocateNextWriteIdxCache_, std::memory_order_release);
  }

  template <typename... Args>
  void emplace(Args &&...args) noexcept(
      std::is_nothrow_constructible<T, Args &&...>::value) {
    static_assert(std::is_constructible<T, Args &&...>::value,
                  "T must be constructible with Args&&...");
    auto const writeIdx = writeIdx_.load(std::memory_order_relaxed);
    auto nextWriteIdx = writeIdx + 1;
    if (nextWriteIdx == capacity_) {
      nextWriteIdx = 0;
    }
    while (nextWriteIdx == readIdxCache_) {
      readIdxCache_ = readIdx_.load(std::memory_order_acquire);
    }
    new (&slots_[writeIdx + kPadding]) T(std::forward<Args>(args)...);
    writeIdx_.store(nextWriteIdx, std::memory_order_release);
  }

  template <typename... Args>
  RIGTORP_NODISCARD bool try_emplace(Args &&...args) noexcept(
      std::is_nothrow_constructible<T, Args &&...>::value) {
    static_assert(std::is_constructible<T, Args &&...>::value,
                  "T must be constructible with Args&&...");
    auto const writeIdx = writeIdx_.load(std::memory_order_relaxed);
    auto nextWriteIdx = writeIdx + 1;
    if (nextWriteIdx == capacity_) {
      nextWriteIdx = 0;
    }
    if (nextWriteIdx == readIdxCache_) {
      readIdxCache_ = readIdx_.load(std::memory_order_acquire);
      if (nextWriteIdx == readIdxCache_) {
        return false;
      }
    }
    new (&slots_[writeIdx + kPadding]) T(std::forward<Args>(args)...);
    writeIdx_.store(nextWriteIdx, std::memory_order_release);
    return true;
  }

  void push(const T &v) noexcept(std::is_nothrow_copy_constructible<T>::value) {
    static_assert(std::is_copy_constructible<T>::value,
                  "T must be copy constructible");
    emplace(v);
  }

  template <typename P, typename = typename std::enable_if<
                            std::is_constructible<T, P &&>::value>::type>
  void push(P &&v) noexcept(std::is_nothrow_constructible<T, P &&>::value) {
    emplace(std::forward<P>(v));
  }

  RIGTORP_NODISCARD bool
  try_push(const T &v) noexcept(std::is_nothrow_copy_constructible<T>::value) {
    static_assert(std::is_copy_constructible<T>::value,
                  "T must be copy constructible");
    return try_emplace(v);
  }

  template <typename P, typename = typename std::enable_if<
                            std::is_constructible<T, P &&>::value>::type>
  RIGTORP_NODISCARD bool
  try_push(P &&v) noexcept(std::is_nothrow_constructible<T, P &&>::value) {
    return try_emplace(std::forward<P>(v));
  }

  RIGTORP_NODISCARD T *front() noexcept {
    auto const readIdx = readIdx_.load(std::memory_order_relaxed);
    if (readIdx == writeIdxCache_) {
      writeIdxCache_ = writeIdx_.load(std::memory_order_acquire);
      if (writeIdxCache_ == readIdx) {
        return nullptr;
      }
    }
    return &slots_[readIdx + kPadding];
  }

  void pop() noexcept {
    static_assert(std::is_nothrow_destructible<T>::value,
                  "T must be nothrow destructible");
    auto const readIdx = readIdx_.load(std::memory_order_relaxed);
    if (writeIdxCache_ == readIdx)
      assert((writeIdxCache_ = writeIdx_.load(std::memory_order_acquire)) != readIdx &&
           "Can only call pop() after front() has returned a non-nullptr");
    slots_[readIdx + kPadding].~T();
    auto nextReadIdx = readIdx + 1;
    if (nextReadIdx == capacity_) {
      nextReadIdx = 0;
    }
    readIdx_.store(nextReadIdx, std::memory_order_release);
  }

  void allocate_pop_n(size_t n_items) noexcept {
    auto readIdx = readIdx_.load(std::memory_order_relaxed);

    if (writeIdxCache_ == readIdx)
      assert((writeIdxCache_ = writeIdx_.load(std::memory_order_acquire)) != readIdx &&
           "Can only call pop() after front() has returned a non-nullptr");
    //slots_[readIdx + kPadding].~T();

    // TODO: the user has to use allocate_n which does the check if this fits
    // in the case nextReadIdx can't fit, the allocate_n has rolled over to the beginning
    // FIXME: no, this happens before, in front()
    auto nextReadIdx = readIdx + n_items;
    if (nextReadIdx > capacityMargin_) { // "greater then" -- the same as in allocate_n
      //readIdx = 0;
      nextReadIdx = 0;
    }

    readIdx_.store(nextReadIdx, std::memory_order_release);
  }

  RIGTORP_NODISCARD size_t size() const noexcept {
    std::ptrdiff_t diff = writeIdx_.load(std::memory_order_acquire) -
                          readIdx_.load(std::memory_order_acquire);
    if (diff < 0) {
      diff += capacity_;
    }
    return static_cast<size_t>(diff);
  }

  RIGTORP_NODISCARD bool empty() const noexcept {
    return writeIdx_.load(std::memory_order_acquire) ==
           readIdx_.load(std::memory_order_acquire);
  }

  RIGTORP_NODISCARD size_t capacity() const noexcept { return capacity_ - 1; }

private:
#ifdef __cpp_lib_hardware_interference_size
  static constexpr size_t kCacheLineSize =
      std::hardware_destructive_interference_size;
#else
  static constexpr size_t kCacheLineSize = 64;
#endif

  // Padding to avoid false sharing between slots_ and adjacent allocations
  static constexpr size_t kPadding = (kCacheLineSize - 1) / sizeof(T) + 1;

private:
  size_t capacity_;
  size_t capacityMargin_;
  size_t maxAllocationLength_;
  T *slots_;
#if defined(__has_cpp_attribute) && __has_cpp_attribute(no_unique_address)
  Allocator allocator_ [[no_unique_address]];
#else
  Allocator allocator_;
#endif

  // Align to cache line size in order to avoid false sharing
  // readIdxCache_ and writeIdxCache_ is used to reduce the amount of cache
  // coherency traffic
  alignas(kCacheLineSize) std::atomic<size_t> writeIdx_ = {0};
  alignas(kCacheLineSize) size_t readIdxCache_ = 0;
  alignas(kCacheLineSize) std::atomic<size_t> readIdx_ = {0};
  alignas(kCacheLineSize) size_t writeIdxCache_ = 0;

  //alignas(kCacheLineSize) size_t wrapCapacity_ = {0}; // dynamic capacity size for the cases of allocate_n roll over

  alignas(kCacheLineSize) size_t allocateNextWriteIdxCache_ = 0; // just to make the allocate logic work
};
} // namespace rigtorp
