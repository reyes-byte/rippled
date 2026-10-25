#include <benchmarks/libxrpl/ballast/accounting/Probes.h>

#include <openssl/crypto.h>

#include <cstdlib>
#include <new>

namespace ballast::allocation::probes {

void*
allocate(std::size_t bytes, Form form)
{
    constexpr auto align = std::align_val_t{256};
    switch (form)
    {
        case Form::Scalar:
            return ::operator new(bytes);
        case Form::Array:
            return ::operator new[](bytes);
        case Form::AlignedScalar:
            return ::operator new(bytes, align);
        case Form::AlignedArray:
            return ::operator new[](bytes, align);
        case Form::NoThrowScalar:
            return ::operator new(bytes, std::nothrow);
        case Form::NoThrowArray:
            return ::operator new[](bytes, std::nothrow);
        case Form::AlignedNoThrowScalar:
            return ::operator new(bytes, align, std::nothrow);
        case Form::AlignedNoThrowArray:
            return ::operator new[](bytes, align, std::nothrow);
    }
    std::abort();
}

void
release(void* ptr, Release form, std::size_t bytes) noexcept
{
    constexpr auto align = std::align_val_t{256};
    switch (form)
    {
        case Release::Scalar:
            return ::operator delete(ptr);
        case Release::Array:
            return ::operator delete[](ptr);
        case Release::SizedScalar:
            return ::operator delete(ptr, bytes);
        case Release::SizedArray:
            return ::operator delete[](ptr, bytes);
        case Release::AlignedScalar:
            return ::operator delete(ptr, align);
        case Release::AlignedArray:
            return ::operator delete[](ptr, align);
        case Release::SizedAlignedScalar:
            return ::operator delete(ptr, bytes, align);
        case Release::SizedAlignedArray:
            return ::operator delete[](ptr, bytes, align);
        case Release::NoThrowScalar:
            return ::operator delete(ptr, std::nothrow);
        case Release::NoThrowArray:
            return ::operator delete[](ptr, std::nothrow);
        case Release::AlignedNoThrowScalar:
            return ::operator delete(ptr, align, std::nothrow);
        case Release::AlignedNoThrowArray:
            return ::operator delete[](ptr, align, std::nothrow);
    }
    std::abort();
}

void*
arrayExpression(std::size_t bytes)
{
    return new unsigned char[bytes];
}
void
deleteArrayExpression(void* ptr) noexcept
{
    delete[] static_cast<unsigned char*>(ptr);
}
void*
cAllocate(std::size_t bytes)
{
    return std::malloc(bytes);
}
void
cRelease(void* ptr) noexcept
{
    std::free(ptr);
}
void*
opensslAllocate(std::size_t bytes)
{
    return OPENSSL_malloc(bytes);
}
void
opensslRelease(void* ptr) noexcept
{
    OPENSSL_free(ptr);
}

}  // namespace ballast::allocation::probes
