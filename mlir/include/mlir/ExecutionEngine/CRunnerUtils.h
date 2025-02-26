//===- CRunnerUtils.h - Utils for debugging MLIR execution ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares basic classes and functions to manipulate structured MLIR
// types at runtime. Entities in this file are must be retargetable, including
// on targets without a C++ runtime.
//
//===----------------------------------------------------------------------===//

#ifndef EXECUTIONENGINE_CRUNNERUTILS_H_
#define EXECUTIONENGINE_CRUNNERUTILS_H_

#ifdef _WIN32
#ifndef MLIR_CRUNNERUTILS_EXPORT
#ifdef mlir_c_runner_utils_EXPORTS
/* We are building this library */
#define MLIR_CRUNNERUTILS_EXPORT __declspec(dllexport)
#else
/* We are using this library */
#define MLIR_CRUNNERUTILS_EXPORT __declspec(dllimport)
#endif // mlir_c_runner_utils_EXPORTS
#endif // MLIR_CRUNNERUTILS_EXPORT
#else
#define MLIR_CRUNNERUTILS_EXPORT
#endif // _WIN32

#include <cstdint>

template <int N> void dropFront(int64_t arr[N], int64_t *res) {
  for (unsigned i = 1; i < N; ++i)
    *(res + i - 1) = arr[i];
}

//===----------------------------------------------------------------------===//
// Codegen-compatible structures for Vector type.
//===----------------------------------------------------------------------===//
namespace detail {
template <unsigned N>
constexpr bool isPowerOf2() {
  return (!(N & (N - 1)));
}
  
template <unsigned N>
constexpr unsigned nextPowerOf2();
template <>
constexpr unsigned nextPowerOf2<0>() {
  return 1;
}
template <>
constexpr unsigned nextPowerOf2<1>() {
  return 1;
}
template <unsigned N> constexpr unsigned nextPowerOf2() {
  return isPowerOf2<N>() ? N : 2 * nextPowerOf2<(N + 1) / 2>();
}

template <typename T, int Dim, bool IsPowerOf2>
struct Vector1D;

template <typename T, int Dim>
struct Vector1D<T, Dim, /*IsPowerOf2=*/true> {
  Vector1D() {
    static_assert(detail::nextPowerOf2<sizeof(T[Dim])>() == sizeof(T[Dim]),
                  "size error");
  }
  constexpr T &operator[](unsigned i) { return vector[i]; }
  constexpr const T &operator[](unsigned i) const { return vector[i]; }

private:
  T vector[Dim];
};

// 1-D vector, padded to the next power of 2 allocation.
// Specialization occurs to avoid zero size arrays (which fail in -Werror).
template <typename T, int Dim>
struct Vector1D<T, Dim, /*IsPowerOf2=*/false> {
  Vector1D() {
    static_assert(detail::nextPowerOf2<sizeof(T[Dim])>() > sizeof(T[Dim]),
                  "size error");
    static_assert(detail::nextPowerOf2<sizeof(T[Dim])>() < 2 * sizeof(T[Dim]),
                  "size error");
  }
  constexpr T &operator[](unsigned i) { return vector[i]; }
  constexpr const T &operator[](unsigned i) const { return vector[i]; }

private:
  T vector[Dim];
  char padding[detail::nextPowerOf2<sizeof(T[Dim])>() - sizeof(T[Dim])];
};
} // end namespace detail

// N-D vectors recurse down to 1-D.
template <typename T, int Dim, int... Dims>
struct Vector {
  constexpr Vector<T, Dims...> &operator[](unsigned i) { return vector[i]; }
  constexpr const Vector<T, Dims...> &operator[](unsigned i) const {
    return vector[i];
  }

private:
  Vector<T, Dims...> vector[Dim];
};

// 1-D vectors in LLVM are automatically padded to the next power of 2.
// We insert explicit padding in to account for this.
template <typename T, int Dim>
struct Vector<T, Dim>
  : public detail::Vector1D<T, Dim, detail::isPowerOf2<sizeof(T[Dim])>()> {};

template <int D1, typename T>
using Vector1D = Vector<T, D1>;
template <int D1, int D2, typename T>
using Vector2D = Vector<T, D1, D2>;
template <int D1, int D2, int D3, typename T>
using Vector3D = Vector<T, D1, D2, D3>;
template <int D1, int D2, int D3, int D4, typename T>
using Vector4D = Vector<T, D1, D2, D3, D4>;

//===----------------------------------------------------------------------===//
// Codegen-compatible structures for StridedMemRef type.
//===----------------------------------------------------------------------===//
/// StridedMemRef descriptor type with static rank.
template <typename T, int N> struct StridedMemRefType {
  T *basePtr;
  T *data;
  int64_t offset;
  int64_t sizes[N];
  int64_t strides[N];
  // This operator[] is extremely slow and only for sugaring purposes.
  StridedMemRefType<T, N - 1> operator[](int64_t idx) {
    StridedMemRefType<T, N - 1> res;
    res.basePtr = basePtr;
    res.data = data;
    res.offset = offset + idx * strides[0];
    dropFront<N>(sizes, res.sizes);
    dropFront<N>(strides, res.strides);
    return res;
  }
};

/// StridedMemRef descriptor type specialized for rank 1.
template <typename T> struct StridedMemRefType<T, 1> {
  T *basePtr;
  T *data;
  int64_t offset;
  int64_t sizes[1];
  int64_t strides[1];
  T &operator[](int64_t idx) { return *(data + offset + idx * strides[0]); }
};

/// StridedMemRef descriptor type specialized for rank 0.
template <typename T> struct StridedMemRefType<T, 0> {
  T *basePtr;
  T *data;
  int64_t offset;
};

//===----------------------------------------------------------------------===//
// Codegen-compatible structure for UnrankedMemRef type.
//===----------------------------------------------------------------------===//
// Unranked MemRef
template <typename T> struct UnrankedMemRefType {
  int64_t rank;
  void *descriptor;
};

//===----------------------------------------------------------------------===//
// Small runtime support "lib" for vector.print lowering during codegen.
//===----------------------------------------------------------------------===//
extern "C" MLIR_CRUNNERUTILS_EXPORT void print_f32(float f);
extern "C" MLIR_CRUNNERUTILS_EXPORT void print_f64(double d);
extern "C" MLIR_CRUNNERUTILS_EXPORT void print_open();
extern "C" MLIR_CRUNNERUTILS_EXPORT void print_close();
extern "C" MLIR_CRUNNERUTILS_EXPORT void print_comma();
extern "C" MLIR_CRUNNERUTILS_EXPORT void print_newline();

#endif // EXECUTIONENGINE_CRUNNERUTILS_H_
