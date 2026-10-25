#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STBlob.h>
#include <xrpl/protocol/Serializer.h>

#include <benchmarks/libxrpl/ballast/accounting/AllocationTracker.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>

namespace xrpl::ballast_allocation_test {

class BallastAllocationProtocol : public testing::TestWithParam<std::size_t>
{
};

TEST_P(BallastAllocationProtocol, native_serialiter_stblob_allocation_is_intercepted)
{
    auto const bytes = GetParam();
    Blob const payload(bytes, 0x5a);
    Serializer encoded;
    encoded.addVL(payload.data(), payload.size());
    Blob const wire = encoded.getData();  // Input storage explicitly outside scope.
    auto const& field = bytes == 1312 ? sfSigningPubKey : sfTxnSignature;
    {
        // Initialize CountedObject bookkeeping before calibration.
        SerialIter warmup{makeSlice(wire)};
        STBlob const warm{warmup, field};
    }
    ballast::allocation::Scope scope;
    ballast::allocation::Snapshot held;
    bool consumed = false, identical = false;
    {
        SerialIter iterator{makeSlice(wire)};
        STBlob const blob{iterator, field};  // Unmodified candidate implementation.
        consumed = iterator.empty();
        identical = blob.size() == payload.size() &&
            std::equal(payload.begin(), payload.end(), blob.data());
        held = scope.snapshot();
    }  // Buffer is destroyed while the scope still records.
    auto const result = scope.finish();
    EXPECT_TRUE(consumed);
    EXPECT_TRUE(identical);
    EXPECT_EQ(held.allocations, 1);
    EXPECT_EQ(held.requestedBytes, bytes);
    EXPECT_EQ(held.liveBytes, bytes);
    EXPECT_EQ(result.allocations, 1);
    EXPECT_EQ(result.frees, 1);
    EXPECT_EQ(result.freedBytes, bytes);
    EXPECT_EQ(result.peakLiveBytes, bytes);
    EXPECT_EQ(result.liveBytes, 0);
    std::cout << "BALLAST_ALLOCATION_PROTOCOL {\"field_payload_bytes\":" << bytes
              << ",\"allocations\":" << result.allocations
              << ",\"requested_bytes\":" << result.requestedBytes
              << ",\"peak_live_bytes\":" << result.peakLiveBytes
              << ",\"final_live_bytes\":" << result.liveBytes
              << ",\"input_storage\":\"excluded\",\"scope\":\"single_vl_field_calibration\"}\n";
}

INSTANTIATE_TEST_SUITE_P(FieldSizes, BallastAllocationProtocol, testing::Values(1312, 2420));

}  // namespace xrpl::ballast_allocation_test
