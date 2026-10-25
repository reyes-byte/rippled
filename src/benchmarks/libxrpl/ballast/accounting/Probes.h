#pragma once

#include <cstddef>

namespace ballast::allocation::probes {

enum class Form {
    Scalar,
    Array,
    AlignedScalar,
    AlignedArray,
    NoThrowScalar,
    NoThrowArray,
    AlignedNoThrowScalar,
    AlignedNoThrowArray
};
enum class Release {
    Scalar,
    Array,
    SizedScalar,
    SizedArray,
    AlignedScalar,
    AlignedArray,
    SizedAlignedScalar,
    SizedAlignedArray,
    NoThrowScalar,
    NoThrowArray,
    AlignedNoThrowScalar,
    AlignedNoThrowArray
};

// Separate translation unit, without unity/LTO: calls cannot be elided as a
// visible allocation/free pair by the calibration test's compiler.
void*
allocate(std::size_t bytes, Form form = Form::Scalar);
void
release(void* ptr, Release form = Release::Scalar, std::size_t bytes = 0) noexcept;
void*
arrayExpression(std::size_t bytes);
void
deleteArrayExpression(void* ptr) noexcept;
void*
cAllocate(std::size_t bytes);
void
cRelease(void* ptr) noexcept;
void*
opensslAllocate(std::size_t bytes);
void
opensslRelease(void* ptr) noexcept;

}  // namespace ballast::allocation::probes
