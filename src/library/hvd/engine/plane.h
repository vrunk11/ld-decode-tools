// SPDX-License-Identifier: GPL-3.0-or-later
//
// plane.h — minimal row-major image containers for the HVD engine.
//
// The numerical engine is deliberately free of any decode-orc SDK type so it
// can be unit-tested in isolation (no host, no Qt, no plugin ABI). Everything
// flows through `Plane` (real float image) and `ComplexPlane` (the chroma
// phasor field). These mirror the NumPy 2-D float arrays used by the reference
// Python implementation in `reference/hvd/decoder.py`.
//
// Conventions (identical to the reference):
//   * storage is row-major, index (row=y, col=x) -> data[y * width + x];
//   * `height` is the number of image lines, `width` the samples per line;
//   * all arithmetic is float32 (the reference decodes in float32 for the 3D
//     path; the 2-D path is numerically identical within tolerance).

#ifndef LDDECODE_HVD_ENGINE_PLANE_H_
#define LDDECODE_HVD_ENGINE_PLANE_H_

#include <cassert>
#include <complex>
#include <cstddef>
#include <vector>

// A loop whose iterations do not depend on each other, for the compiler to vectorise (OpenMP simd: -fopenmp-simd, which needs no OpenMP runtime).
// MSVC has the OpenMP of 2.0, which has no simd: the loop is left to it.
#if defined(__GNUC__) || defined(__clang__)
#define HVD_PRAGMA(x) _Pragma(#x)
#define HVD_SIMD HVD_PRAGMA(omp simd)
#define HVD_SIMD_REDUCE_ADD(v) HVD_PRAGMA(omp simd reduction(+ : v))
#else
#define HVD_SIMD
#define HVD_SIMD_REDUCE_ADD(v)
#endif


namespace hvd {

namespace detail {

// The planes are big (a field is a megabyte or two of floats, and the decoder makes some fifty of them for each), and the heap gives blocks of that size
// back to the system when they are freed and asks for them again, zeroed, at the next allocation: the page faults that follow, in every thread, were
// what stopped the decoder from going faster with more threads. Each thread keeps the blocks it has freed, to give them out again.
class BlockCache {
 public:
  BlockCache() = default;
  BlockCache(const BlockCache&) = delete;
  BlockCache& operator=(const BlockCache&) = delete;
  ~BlockCache() {
    for (auto& kept : blocks_) ::operator delete(kept.pointer);
  }

  void* Take(size_t bytes) {
    for (size_t i = 0; i < blocks_.size(); ++i) {
      if (blocks_[i].bytes == bytes) {
        void* pointer = blocks_[i].pointer;
        cached_ -= bytes;
        blocks_[i] = blocks_.back();
        blocks_.pop_back();
        return pointer;
      }
    }
    return nullptr;
  }

  bool Keep(void* pointer, size_t bytes) {
    if (cached_ + bytes > kMaxCached) return false;
    blocks_.push_back({pointer, bytes});
    cached_ += bytes;
    return true;
  }

  // Blocks under this size are the heap's to manage as it does; what a thread keeps is bounded (about what one field takes at its peak)
  static constexpr size_t kMinPooled = 64 * 1024;
  static constexpr size_t kMaxCached = 192u * 1024u * 1024u;

 private:
  struct Block {
    void* pointer;
    size_t bytes;
  };
  std::vector<Block> blocks_;
  size_t cached_ = 0;
};

inline BlockCache& ThreadCache() {
  thread_local BlockCache cache;
  return cache;
}

}  // namespace detail

// An allocator for the storage of the planes: the blocks of a thread are kept for the next plane of the same size
template <typename T>
struct PlaneAllocator {
  using value_type = T;

  PlaneAllocator() = default;
  template <typename U>
  PlaneAllocator(const PlaneAllocator<U>&) {}

  T* allocate(size_t n) {
    const size_t bytes = n * sizeof(T);
    if (bytes >= detail::BlockCache::kMinPooled) {
      if (void* kept = detail::ThreadCache().Take(bytes)) return static_cast<T*>(kept);
    }
    return static_cast<T*>(::operator new(bytes));
  }

  void deallocate(T* pointer, size_t n) {
    const size_t bytes = n * sizeof(T);
    if (bytes >= detail::BlockCache::kMinPooled && detail::ThreadCache().Keep(pointer, bytes)) return;
    ::operator delete(pointer);
  }

  template <typename U>
  bool operator==(const PlaneAllocator<U>&) const { return true; }
  template <typename U>
  bool operator!=(const PlaneAllocator<U>&) const { return false; }
};

}  // namespace hvd
namespace hvd {

using Complex = std::complex<float>;

// A dense, row-major 2-D array of `T`. Copyable and movable; sized once at
// construction and never resized (the engine allocates working planes up front
// per frame, matching the reference's array lifetimes).
template <typename T>
class BasicPlane {
 public:
  BasicPlane() = default;
  BasicPlane(int height, int width, T fill = T{})
      : height_(height), width_(width), data_(static_cast<size_t>(height) *
                                                   static_cast<size_t>(width),
                                               fill) {
    assert(height >= 0 && width >= 0);
  }

  int height() const { return height_; }
  int width() const { return width_; }
  size_t size() const { return data_.size(); }
  bool empty() const { return data_.empty(); }

  T* data() { return data_.data(); }
  const T* data() const { return data_.data(); }

  // Element access. `at(y, x)` is bounds-checked in debug builds only.
  T& at(int y, int x) {
    assert(y >= 0 && y < height_ && x >= 0 && x < width_);
    return data_[static_cast<size_t>(y) * width_ + x];
  }
  const T& at(int y, int x) const {
    assert(y >= 0 && y < height_ && x >= 0 && x < width_);
    return data_[static_cast<size_t>(y) * width_ + x];
  }

  // Flat access for the vectorised inner loops (gradients, CG, colour).
  T& operator[](size_t i) { return data_[i]; }
  const T& operator[](size_t i) const { return data_[i]; }

 private:
  int height_ = 0;
  int width_ = 0;
  std::vector<T, PlaneAllocator<T>> data_;
};

using Plane = BasicPlane<float>;
using ComplexPlane = BasicPlane<Complex>;

}  // namespace hvd

#endif  // LDDECODE_HVD_ENGINE_PLANE_H_
