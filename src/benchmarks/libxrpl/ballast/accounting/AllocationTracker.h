#pragma once

#include <cstddef>

namespace ballast::allocation {

struct Snapshot
{
    std::size_t allocations = 0;
    std::size_t requestedBytes = 0;
    std::size_t frees = 0;
    std::size_t freedBytes = 0;
    std::size_t liveBlocks = 0;
    std::size_t liveBytes = 0;
    std::size_t peakLiveBytes = 0;
    std::size_t nonOwnedFrees = 0;
    std::size_t nonOwnedFreedBytes = 0;
};

namespace detail {
struct State;
}

// Attribution is exclusive to the innermost scope on the allocating thread.
// Frees update the allocation's origin, including after finish/on other threads.
// Scope must finish/destruct on its creating thread in reverse nesting order.
class Scope
{
    detail::State* state_;
    detail::State* previous_;
    bool recording_ = true;

public:
    ~Scope();
    Scope();
    Scope(Scope const&) = delete;
    Scope&
    operator=(Scope const&) = delete;
    Scope(Scope&&) = delete;
    Scope&
    operator=(Scope&&) = delete;

    // Stops attribution before callers print, assert, or otherwise allocate.
    [[nodiscard]] Snapshot
    finish() noexcept;
    // Outstanding allocations can still be freed after finish().
    [[nodiscard]] Snapshot
    snapshot() const noexcept;
};

// Calibration diagnostics; tracker metadata is excluded from requested bytes.
[[nodiscard]] std::size_t
liveTrackingStates() noexcept;
[[nodiscard]] std::size_t
allocationHeaderBytes() noexcept;

}  // namespace ballast::allocation
