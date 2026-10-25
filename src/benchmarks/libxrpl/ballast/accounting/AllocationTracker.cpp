#include <benchmarks/libxrpl/ballast/accounting/AllocationTracker.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace ballast::allocation {
namespace detail {

struct State
{
    std::atomic<std::size_t> references{1};  // Scope + each outstanding allocation.
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    Snapshot counters;
};

struct Guard
{
    State* state;
    explicit Guard(State* value) noexcept : state(value)
    {
        while (state->lock.test_and_set(std::memory_order_acquire))
        {
        }
    }
    ~Guard()
    {
        state->lock.clear(std::memory_order_release);
    }
};

struct alignas(std::max_align_t) Header
{
    void* raw;
    std::size_t requested;
    State* origin;
};

thread_local State* current = nullptr;
std::atomic<std::size_t> states{0};

void
releaseState(State* state) noexcept
{
    if (state->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        state->~State();
        std::free(state);
        states.fetch_sub(1, std::memory_order_relaxed);
    }
}

void*
tryAllocate(std::size_t requested, std::size_t alignment) noexcept
{
    alignment = std::max(alignment, alignof(std::max_align_t));
    auto const max = std::numeric_limits<std::size_t>::max();
    if ((alignment & (alignment - 1)) != 0 || alignment > max - sizeof(Header))
        return nullptr;
    auto const overhead = sizeof(Header) + alignment - 1;
    auto const payload = std::max(requested, std::size_t{1});
    if (payload > max - overhead)
        return nullptr;
    void* raw = std::malloc(payload + overhead);
    if (!raw)
        return nullptr;
    auto const start = reinterpret_cast<std::uintptr_t>(raw) + sizeof(Header);
    auto const aligned = (start + alignment - 1) & ~(std::uintptr_t(alignment) - 1);
    auto* header =
        ::new (reinterpret_cast<void*>(aligned - sizeof(Header))) Header{raw, requested, current};
    if (auto* origin = header->origin)
    {
        origin->references.fetch_add(1, std::memory_order_relaxed);
        Guard guard(origin);
        auto& c = origin->counters;
        ++c.allocations;
        c.requestedBytes += requested;
        ++c.liveBlocks;
        c.liveBytes += requested;
        c.peakLiveBytes = std::max(c.peakLiveBytes, c.liveBytes);
    }
    return reinterpret_cast<void*>(aligned);
}

void*
allocate(std::size_t size, std::size_t alignment)
{
    for (;;)
    {
        if (auto* ptr = tryAllocate(size, alignment))
            return ptr;
        auto const handler = std::get_new_handler();
        if (!handler)
            throw std::bad_alloc();
        handler();
    }
}

void*
allocateNoThrow(std::size_t size, std::size_t alignment) noexcept
{
    try
    {
        return allocate(size, alignment);
    }
    catch (...)
    {
        return nullptr;
    }
}

void
deallocate(void* ptr) noexcept
{
    if (!ptr)
        return;
    auto* header =
        reinterpret_cast<Header*>(reinterpret_cast<std::uintptr_t>(ptr) - sizeof(Header));
    auto* origin = header->origin;
    if (current && current != origin)
    {
        Guard guard(current);
        ++current->counters.nonOwnedFrees;
        current->counters.nonOwnedFreedBytes += header->requested;
    }
    if (origin)
    {
        Guard guard(origin);
        auto& c = origin->counters;
        ++c.frees;
        c.freedBytes += header->requested;
        --c.liveBlocks;
        c.liveBytes -= header->requested;
    }
    void* raw = header->raw;
    header->~Header();
    std::free(raw);
    if (origin)
        releaseState(origin);
}

}  // namespace detail

Scope::Scope() : state_(nullptr), previous_(detail::current)
{
    void* storage = std::malloc(sizeof(detail::State));
    if (!storage)
        throw std::bad_alloc();
    state_ = ::new (storage) detail::State;
    detail::states.fetch_add(1, std::memory_order_relaxed);
    detail::current = state_;
}

Scope::~Scope()
{
    static_cast<void>(finish());
    detail::releaseState(state_);
}

Snapshot
Scope::finish() noexcept
{
    if (recording_)
    {
        if (detail::current != state_)
            std::abort();  // Wrong thread or non-LIFO nesting is a caller error.
        detail::current = previous_;
        recording_ = false;
    }
    return snapshot();
}

Snapshot
Scope::snapshot() const noexcept
{
    detail::Guard guard(state_);
    return state_->counters;
}

std::size_t
liveTrackingStates() noexcept
{
    return detail::states.load(std::memory_order_relaxed);
}

std::size_t
allocationHeaderBytes() noexcept
{
    return sizeof(detail::Header);
}

}  // namespace ballast::allocation

// Each allocation enters the engine once. All matching delete forms consult
// our header rather than trusting the optional size/alignment arguments.
void*
operator new(std::size_t n)
{
    return ballast::allocation::detail::allocate(n, alignof(std::max_align_t));
}
void*
operator new[](std::size_t n)
{
    return ballast::allocation::detail::allocate(n, alignof(std::max_align_t));
}
void*
operator new(std::size_t n, std::align_val_t a)
{
    return ballast::allocation::detail::allocate(n, static_cast<std::size_t>(a));
}
void*
operator new[](std::size_t n, std::align_val_t a)
{
    return ballast::allocation::detail::allocate(n, static_cast<std::size_t>(a));
}
void*
operator new(std::size_t n, std::nothrow_t const&) noexcept
{
    return ballast::allocation::detail::allocateNoThrow(n, alignof(std::max_align_t));
}
void*
operator new[](std::size_t n, std::nothrow_t const&) noexcept
{
    return ballast::allocation::detail::allocateNoThrow(n, alignof(std::max_align_t));
}
void*
operator new(std::size_t n, std::align_val_t a, std::nothrow_t const&) noexcept
{
    return ballast::allocation::detail::allocateNoThrow(n, static_cast<std::size_t>(a));
}
void*
operator new[](std::size_t n, std::align_val_t a, std::nothrow_t const&) noexcept
{
    return ballast::allocation::detail::allocateNoThrow(n, static_cast<std::size_t>(a));
}

void
operator delete(void* p) noexcept
{
    ballast::allocation::detail::deallocate(p);
}
void
operator delete[](void* p) noexcept
{
    ballast::allocation::detail::deallocate(p);
}
void
operator delete(void* p, std::size_t) noexcept
{
    ballast::allocation::detail::deallocate(p);
}
void
operator delete[](void* p, std::size_t) noexcept
{
    ballast::allocation::detail::deallocate(p);
}
void
operator delete(void* p, std::align_val_t) noexcept
{
    ballast::allocation::detail::deallocate(p);
}
void
operator delete[](void* p, std::align_val_t) noexcept
{
    ballast::allocation::detail::deallocate(p);
}
void
operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
    ballast::allocation::detail::deallocate(p);
}
void
operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
    ballast::allocation::detail::deallocate(p);
}
void
operator delete(void* p, std::nothrow_t const&) noexcept
{
    ballast::allocation::detail::deallocate(p);
}
void
operator delete[](void* p, std::nothrow_t const&) noexcept
{
    ballast::allocation::detail::deallocate(p);
}
void
operator delete(void* p, std::align_val_t, std::nothrow_t const&) noexcept
{
    ballast::allocation::detail::deallocate(p);
}
void
operator delete[](void* p, std::align_val_t, std::nothrow_t const&) noexcept
{
    ballast::allocation::detail::deallocate(p);
}
