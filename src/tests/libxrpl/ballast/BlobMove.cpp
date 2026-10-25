#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STBlob.h>
#include <xrpl/protocol/detail/STVar.h>

#include <benchmarks/libxrpl/ballast/accounting/AllocationTracker.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace xrpl::ballast_blob_move_test {

// A move constructor must not remove the pre-existing copy API.
static_assert(std::is_copy_constructible_v<STBlob>);
static_assert(std::is_copy_assignable_v<STBlob>);
#if BALLAST_EXPECT_STBLOB_MOVE
static_assert(std::is_nothrow_move_constructible_v<STBlob>);
#endif

bool
sameBytes(STBlob const& blob, Blob const& expected)
{
    return blob.size() == expected.size() &&
        std::equal(expected.begin(), expected.end(), blob.data());
}

class BallastBlobMove : public testing::TestWithParam<std::size_t>
{
protected:
    void
    SetUp() override
    {
        // Initialize the persistent object counter outside measured scopes.
        STBlob const warmup{sfTxnSignature};
    }
};

TEST_P(BallastBlobMove, move_construction_preserves_bytes_and_balances_allocations)
{
    auto const size = GetParam();
    Blob const payload(size, 0x5a);
    bool sameAddress = false, bytesMatch = false, sourceEmpty = false, fieldsMatch = false;
    ballast::allocation::Scope scope;
    {
        STBlob source{sfTxnSignature, payload.data(), payload.size()};
        auto const* original = source.data();
        STBlob destination{std::move(source)};
        sameAddress = destination.data() == original;
        bytesMatch = sameBytes(destination, payload);
        sourceEmpty = source.size() == 0 && source.data() == nullptr;
        fieldsMatch =
            destination.getFName() == sfTxnSignature && source.getFName() == sfTxnSignature;
    }
    auto const result = scope.finish();
    EXPECT_TRUE(bytesMatch);
    EXPECT_TRUE(fieldsMatch);
    EXPECT_EQ(sameAddress, bool(BALLAST_EXPECT_STBLOB_MOVE));
    EXPECT_EQ(sourceEmpty, bool(BALLAST_EXPECT_STBLOB_MOVE));
    EXPECT_EQ(result.allocations, BALLAST_EXPECT_STBLOB_MOVE ? 1 : 2);
    EXPECT_EQ(result.requestedBytes, size * (BALLAST_EXPECT_STBLOB_MOVE ? 1 : 2));
    EXPECT_EQ(result.peakLiveBytes, size * (BALLAST_EXPECT_STBLOB_MOVE ? 1 : 2));
    EXPECT_EQ(result.frees, result.allocations);
    EXPECT_EQ(result.freedBytes, result.requestedBytes);
    EXPECT_EQ(result.liveBytes, 0);
    EXPECT_EQ(result.nonOwnedFrees, 0);
}

TEST_P(BallastBlobMove, stvar_move_uses_the_blob_ownership_transfer)
{
    auto const size = GetParam();
    Blob const payload(size, 0xa5);
    bool bytesMatch = false, sameAddress = false;
    ballast::allocation::Scope scope;
    {
        auto source = detail::makeStvar<STBlob>(sfSigningPubKey, payload.data(), payload.size());
        auto const* original = source.get().downcast<STBlob>().data();
        detail::STVar destination{std::move(source)};
        auto const& blob = destination.get().downcast<STBlob>();
        bytesMatch = sameBytes(blob, payload);
        sameAddress = blob.data() == original;
    }
    auto const result = scope.finish();
    EXPECT_TRUE(bytesMatch);
    EXPECT_EQ(sameAddress, bool(BALLAST_EXPECT_STBLOB_MOVE));
    EXPECT_EQ(result.allocations, BALLAST_EXPECT_STBLOB_MOVE ? 1 : 2);
    EXPECT_EQ(result.liveBytes, 0);
    EXPECT_EQ(result.frees, result.allocations);
    EXPECT_EQ(result.nonOwnedFrees, 0);
}

TEST_P(BallastBlobMove, copy_construction_and_assignment_keep_independent_buffers)
{
    Blob const payload(GetParam(), 0x5a);
    Blob const replacement(GetParam(), 0xa5);
    STBlob source{sfTxnSignature, payload.data(), payload.size()};
    STBlob copied{source};
    STBlob assigned{sfTxnSignature};
    assigned = source;
    EXPECT_NE(source.data(), copied.data());
    EXPECT_NE(source.data(), assigned.data());
    source = makeSlice(replacement);
    EXPECT_TRUE(sameBytes(source, replacement));
    EXPECT_TRUE(sameBytes(copied, payload));
    EXPECT_TRUE(sameBytes(assigned, payload));
}

TEST_P(BallastBlobMove, source_can_be_reassigned_after_move_without_changing_destination)
{
    Blob const payload(GetParam(), 0x5a);
    Blob const replacement(GetParam(), 0xa5);
    STBlob source{sfTxnSignature, payload.data(), payload.size()};
    STBlob destination{std::move(source)};
    source = makeSlice(replacement);
    EXPECT_TRUE(sameBytes(source, replacement));
    EXPECT_TRUE(sameBytes(destination, payload));
}

INSTANTIATE_TEST_SUITE_P(FieldSizes, BallastBlobMove, testing::Values(1312, 2420));

TEST(BallastBlobMoveEmpty, moving_empty_blob_does_not_allocate)
{
    STBlob const warmup{sfTxnSignature};
    ballast::allocation::Scope scope;
    bool empty = false;
    {
        STBlob source{sfTxnSignature};
        STBlob destination{std::move(source)};
        empty = destination.size() == 0 && destination.data() == nullptr && source.size() == 0;
    }
    auto const result = scope.finish();
    EXPECT_TRUE(empty);
    EXPECT_EQ(result.allocations, 0);
    EXPECT_EQ(result.liveBytes, 0);
}

}  // namespace xrpl::ballast_blob_move_test
