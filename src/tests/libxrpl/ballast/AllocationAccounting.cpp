#include <benchmarks/libxrpl/ballast/accounting/AllocationTracker.h>
#include <benchmarks/libxrpl/ballast/accounting/Probes.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <new>
#include <thread>

namespace ballast::allocation::tests {

using probes::Form;
using probes::Release;

struct Forms
{
    Form allocate;
    Release release;
    bool aligned;
};
constexpr std::array<Forms, 12> kForms{
    {{Form::Scalar, Release::Scalar, false},
     {Form::Array, Release::Array, false},
     {Form::AlignedScalar, Release::AlignedScalar, true},
     {Form::AlignedArray, Release::AlignedArray, true},
     {Form::NoThrowScalar, Release::NoThrowScalar, false},
     {Form::NoThrowArray, Release::NoThrowArray, false},
     {Form::AlignedNoThrowScalar, Release::AlignedNoThrowScalar, true},
     {Form::AlignedNoThrowArray, Release::AlignedNoThrowArray, true},
     {Form::Scalar, Release::SizedScalar, false},
     {Form::Array, Release::SizedArray, false},
     {Form::AlignedScalar, Release::SizedAlignedScalar, true},
     {Form::AlignedArray, Release::SizedAlignedArray, true}}};

class BallastAllocationForms : public testing::TestWithParam<Forms>
{
};

TEST_P(BallastAllocationForms, captures_requested_size_and_matching_release)
{
    auto const forms = GetParam();
    Scope scope;
    void* ptr = probes::allocate(37, forms.allocate);
    bool const nonnull = ptr != nullptr;
    bool const aligned =
        reinterpret_cast<std::uintptr_t>(ptr) % (forms.aligned ? 256 : alignof(std::max_align_t)) ==
        0;
    probes::release(ptr, forms.release, 37);
    auto const result = scope.finish();
    EXPECT_TRUE(nonnull);
    EXPECT_TRUE(aligned);
    EXPECT_EQ(result.allocations, 1);
    EXPECT_EQ(result.requestedBytes, 37);
    EXPECT_EQ(result.frees, 1);
    EXPECT_EQ(result.freedBytes, 37);
    EXPECT_EQ(result.liveBlocks, 0);
    EXPECT_EQ(result.liveBytes, 0);
    EXPECT_EQ(result.peakLiveBytes, 37);
}
INSTANTIATE_TEST_SUITE_P(Overloads, BallastAllocationForms, testing::ValuesIn(kForms));

TEST(BallastAllocationAccounting, empty_scope_excludes_its_own_metadata)
{
    auto const before = liveTrackingStates();
    Snapshot result;
    {
        Scope scope;
        result = scope.finish();
    }
    EXPECT_EQ(result.allocations, 0);
    EXPECT_EQ(result.requestedBytes, 0);
    EXPECT_EQ(result.frees, 0);
    EXPECT_EQ(liveTrackingStates(), before);
}

TEST(BallastAllocationAccounting, cumulative_peak_and_live_bytes_are_distinct)
{
    Scope scope;
    auto* a = probes::allocate(64);
    auto* b = probes::allocate(128);
    auto const atPeak = scope.snapshot();
    probes::release(a);
    auto* c = probes::allocate(32);
    auto const afterReuse = scope.snapshot();
    probes::release(b);
    probes::release(c);
    auto const result = scope.finish();
    EXPECT_EQ(atPeak.liveBytes, 192);
    EXPECT_EQ(afterReuse.liveBytes, 160);
    EXPECT_EQ(afterReuse.peakLiveBytes, 192);
    EXPECT_EQ(result.allocations, 3);
    EXPECT_EQ(result.requestedBytes, 224);
    EXPECT_EQ(result.frees, 3);
    EXPECT_EQ(result.freedBytes, 224);
    EXPECT_EQ(result.liveBytes, 0);
    EXPECT_EQ(result.peakLiveBytes, 192);
    std::cout << "BALLAST_ALLOCATION_CALIBRATION {\"requested_bytes\":" << result.requestedBytes
              << ",\"peak_live_bytes\":" << result.peakLiveBytes
              << ",\"final_live_bytes\":" << result.liveBytes
              << ",\"allocations\":" << result.allocations << ",\"frees\":" << result.frees
              << ",\"excluded_header_bytes_per_allocation\":" << allocationHeaderBytes() << "}\n";
}

TEST(BallastAllocationAccounting, real_array_new_expression_is_intercepted)
{
    Scope scope;
    void* ptr = probes::arrayExpression(41);
    probes::deleteArrayExpression(ptr);
    auto const result = scope.finish();
    EXPECT_EQ(result.allocations, 1);
    EXPECT_EQ(result.requestedBytes, 41);
    EXPECT_EQ(result.freedBytes, 41);
    EXPECT_EQ(result.liveBytes, 0);
}

TEST(BallastAllocationAccounting, freeing_preexisting_input_does_not_underflow)
{
    void* input = probes::allocate(128);
    Scope scope;
    probes::release(input);
    auto const result = scope.finish();
    EXPECT_EQ(result.allocations, 0);
    EXPECT_EQ(result.liveBytes, 0);
    EXPECT_EQ(result.freedBytes, 0);
    EXPECT_EQ(result.nonOwnedFrees, 1);
    EXPECT_EQ(result.nonOwnedFreedBytes, 128);
}

TEST(BallastAllocationAccounting, frees_after_finish_update_origin_but_new_allocations_do_not)
{
    Scope scope;
    void* ptr = probes::allocate(64);
    auto const stopped = scope.finish();
    void* unowned = probes::allocate(16);
    probes::release(unowned);
    probes::release(ptr);
    auto const later = scope.snapshot();
    EXPECT_EQ(stopped.liveBytes, 64);
    EXPECT_EQ(stopped.frees, 0);
    EXPECT_EQ(later.allocations, 1);
    EXPECT_EQ(later.requestedBytes, 64);
    EXPECT_EQ(later.freedBytes, 64);
    EXPECT_EQ(later.liveBytes, 0);
}

TEST(BallastAllocationAccounting, allocation_can_safely_outlive_scope_object)
{
    auto const before = liveTrackingStates();
    void* ptr;
    {
        Scope scope;
        ptr = probes::allocate(64);
        static_cast<void>(scope.finish());
    }
    auto const retained = liveTrackingStates();
    probes::release(ptr);
    EXPECT_EQ(retained, before + 1);
    EXPECT_EQ(liveTrackingStates(), before);
}

TEST(BallastAllocationAccounting, freeing_in_another_scope_preserves_both_ledgers)
{
    Scope first;
    void* ptr = probes::allocate(64);
    static_cast<void>(first.finish());
    Scope second;
    probes::release(ptr);
    void* own = probes::allocate(16);
    probes::release(own);
    auto const b = second.finish();
    auto const a = first.snapshot();
    EXPECT_EQ(a.freedBytes, 64);
    EXPECT_EQ(a.liveBytes, 0);
    EXPECT_EQ(b.requestedBytes, 16);
    EXPECT_EQ(b.freedBytes, 16);
    EXPECT_EQ(b.nonOwnedFreedBytes, 64);
    EXPECT_EQ(b.liveBytes, 0);
}

TEST(BallastAllocationAccounting, nested_scopes_attribute_exclusively_to_innermost)
{
    Scope outer;
    void* a = probes::allocate(16);
    Scope inner;
    void* b = probes::allocate(32);
    probes::release(a);
    probes::release(b);
    auto const inside = inner.finish();
    void* c = probes::allocate(8);
    probes::release(c);
    auto const outside = outer.finish();
    EXPECT_EQ(inside.requestedBytes, 32);
    EXPECT_EQ(inside.nonOwnedFreedBytes, 16);
    EXPECT_EQ(inside.liveBytes, 0);
    EXPECT_EQ(outside.requestedBytes, 24);
    EXPECT_EQ(outside.freedBytes, 24);
    EXPECT_EQ(outside.peakLiveBytes, 16);
}

TEST(BallastAllocationAccounting, cross_thread_free_updates_allocation_origin)
{
    std::atomic<void*> pending{nullptr};
    std::atomic<bool> done{false};
    Snapshot workerResult;
    std::thread worker([&] {
        void* ptr;
        while (!(ptr = pending.load(std::memory_order_acquire)))
            std::this_thread::yield();
        Scope workerScope;
        probes::release(ptr);
        workerResult = workerScope.finish();
        done.store(true, std::memory_order_release);
    });
    Scope scope;
    pending.store(probes::allocate(80), std::memory_order_release);
    while (!done.load(std::memory_order_acquire))
        std::this_thread::yield();
    auto const result = scope.finish();
    worker.join();
    EXPECT_EQ(result.allocations, 1);
    EXPECT_EQ(result.freedBytes, 80);
    EXPECT_EQ(result.liveBytes, 0);
    EXPECT_EQ(workerResult.allocations, 0);
    EXPECT_EQ(workerResult.nonOwnedFreedBytes, 80);
}

TEST(BallastAllocationAccounting, cross_thread_free_after_scope_destruction_keeps_metadata_alive)
{
    auto const before = liveTrackingStates();
    std::atomic<void*> pending{nullptr};
    std::thread worker([&] {
        void* ptr;
        while (!(ptr = pending.load(std::memory_order_acquire)))
            std::this_thread::yield();
        probes::release(ptr);
    });
    void* ptr;
    {
        Scope scope;
        ptr = probes::allocate(80);
        static_cast<void>(scope.finish());
    }
    auto const retained = liveTrackingStates();
    pending.store(ptr, std::memory_order_release);
    worker.join();
    EXPECT_EQ(retained, before + 1);
    EXPECT_EQ(liveTrackingStates(), before);
}

TEST(BallastAllocationAccounting, worker_allocations_are_not_inherited_by_parent_scope)
{
    std::atomic<bool> go{false}, done{false};
    std::thread worker([&] {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        void* ptr = probes::allocate(96);
        probes::release(ptr);
        done.store(true, std::memory_order_release);
    });
    Scope scope;
    go.store(true, std::memory_order_release);
    while (!done.load(std::memory_order_acquire))
        std::this_thread::yield();
    auto const result = scope.finish();
    worker.join();
    EXPECT_EQ(result.allocations, 0);
    EXPECT_EQ(result.liveBytes, 0);
}

TEST(BallastAllocationAccounting, concurrent_frees_preserve_totals)
{
    std::array<void*, 8> pointers{};
    std::array<std::thread, 4> workers;
    std::atomic<bool> go{false};
    std::atomic<unsigned int> done{0};
    for (std::size_t i = 0; i < workers.size(); ++i)
        workers[i] = std::thread([&, i] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (auto j = i; j < pointers.size(); j += workers.size())
                probes::release(pointers[j]);
            done.fetch_add(1, std::memory_order_release);
        });
    Scope scope;
    for (auto& ptr : pointers)
        ptr = probes::allocate(64);
    go.store(true, std::memory_order_release);
    while (done.load(std::memory_order_acquire) != workers.size())
        std::this_thread::yield();
    auto const result = scope.finish();
    for (auto& worker : workers)
        worker.join();
    EXPECT_EQ(result.allocations, 8);
    EXPECT_EQ(result.frees, 8);
    EXPECT_EQ(result.requestedBytes, 512);
    EXPECT_EQ(result.freedBytes, 512);
    EXPECT_EQ(result.peakLiveBytes, 512);
    EXPECT_EQ(result.liveBytes, 0);
}

TEST(BallastAllocationAccounting, zero_size_and_null_delete_have_explicit_semantics)
{
    Scope scope;
    void* ptr = probes::allocate(0);
    auto const held = scope.snapshot();
    bool const nonnull = ptr != nullptr;
    probes::release(ptr);
    for (auto forms : kForms)
        probes::release(nullptr, forms.release, 0);
    auto const result = scope.finish();
    EXPECT_TRUE(nonnull);
    EXPECT_EQ(held.liveBlocks, 1);
    EXPECT_EQ(held.liveBytes, 0);
    EXPECT_EQ(result.allocations, 1);
    EXPECT_EQ(result.frees, 1);
    EXPECT_EQ(result.requestedBytes, 0);
    EXPECT_EQ(result.liveBlocks, 0);
}

TEST(BallastAllocationAccounting, overflow_throws_or_returns_null_without_counting_success)
{
    auto const oldHandler = std::set_new_handler(nullptr);
    unsigned int caught = 0, nulls = 0;
    Scope scope;
    for (auto form : {Form::Scalar, Form::Array, Form::AlignedScalar, Form::AlignedArray})
    {
        try
        {
            void* ptr = probes::allocate(std::numeric_limits<std::size_t>::max(), form);
            probes::release(ptr);
        }
        catch (std::bad_alloc const&)
        {
            ++caught;
        }
    }
    for (auto form :
         {Form::NoThrowScalar,
          Form::NoThrowArray,
          Form::AlignedNoThrowScalar,
          Form::AlignedNoThrowArray})
        if (!probes::allocate(std::numeric_limits<std::size_t>::max(), form))
            ++nulls;
    auto const result = scope.finish();
    std::set_new_handler(oldHandler);
    EXPECT_EQ(caught, 4);
    EXPECT_EQ(nulls, 4);
    EXPECT_EQ(result.allocations, 0);
}

std::atomic<unsigned int> handlerCalls{0};
void
failingHandler()
{
    handlerCalls.fetch_add(1);
    throw std::bad_alloc();
}

TEST(BallastAllocationAccounting, new_handler_is_honored)
{
    handlerCalls.store(0);
    auto const previous = std::set_new_handler(failingHandler);
    bool caught = false;
    Scope scope;
    try
    {
        void* ptr = probes::allocate(std::numeric_limits<std::size_t>::max());
        probes::release(ptr);
    }
    catch (std::bad_alloc const&)
    {
        caught = true;
    }
    auto const result = scope.finish();
    std::set_new_handler(previous);
    EXPECT_TRUE(caught);
    EXPECT_EQ(handlerCalls.load(), 1);
    EXPECT_EQ(result.allocations, 0);
}

TEST(BallastAllocationAccounting, c_and_openssl_buffer_probes_are_outside_coverage)
{
    Scope scope;
    void* c = probes::cAllocate(96);
    void* ssl = probes::opensslAllocate(96);
    bool const allocated = c && ssl;
    probes::cRelease(c);
    probes::opensslRelease(ssl);
    auto const result = scope.finish();
    EXPECT_TRUE(allocated);
    EXPECT_EQ(result.allocations, 0);
    EXPECT_EQ(result.requestedBytes, 0);
    std::cout << "BALLAST_ALLOCATION_COVERAGE {\"cpp_requested_bytes\":" << result.requestedBytes
              << ",\"c_probe_requested_bytes\":96,\"openssl_probe_requested_bytes\":96,"
                 "\"rss_measured\":false}\n";
}

}  // namespace ballast::allocation::tests
