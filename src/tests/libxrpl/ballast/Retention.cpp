#include <benchmarks/libxrpl/ballast/retention/Retention.h>

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/hash/uhash.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <unordered_set>

namespace ballast::retention::tests {

class BallastRetention : public testing::TestWithParam<std::string_view>
{
protected:
    std::unordered_set<xrpl::uint256, beast::Uhash<>> const presets{xrpl::featureQuantum};
    xrpl::Rules const rules{presets};
};

TEST_P(BallastRetention, all_objects_remain_alive_until_snapshot_and_cleanup_balances)
{
    auto const fixture = measurement::loadFixture(GetParam(), rules);
    auto const one = measure(fixture, rules, 1);
    auto const three = measure(fixture, rules, 3);
    for (auto const& [count, sample] :
         {std::pair{std::size_t{1}, one}, std::pair{std::size_t{3}, three}})
    {
        EXPECT_EQ(sample.retainedObjects, count);
        EXPECT_EQ(sample.consumedInputs, count);
        EXPECT_EQ(sample.validSignatures, count);
        EXPECT_EQ(sample.matchingTransactionIds, count);
        EXPECT_EQ(sample.wirePayloadBytes, count * fixture.wire.size());
        EXPECT_GE(sample.wireCapacityBytes, sample.wirePayloadBytes);
        EXPECT_EQ(
            sample.inputs.liveBytes, sample.containerCapacityBytes + sample.wireCapacityBytes);
        EXPECT_EQ(sample.inputs.allocations, count + 1);
        EXPECT_GT(sample.held.liveBytes, sample.inputs.liveBytes);
        EXPECT_GT(sample.held.liveBlocks, sample.inputs.liveBlocks);
        EXPECT_GE(sample.completed.peakLiveBytes, sample.held.liveBytes);
        EXPECT_EQ(sample.completed.liveBytes, 0);
        EXPECT_EQ(sample.completed.liveBlocks, 0);
        EXPECT_EQ(sample.completed.allocations, sample.completed.frees);
        EXPECT_EQ(sample.completed.requestedBytes, sample.completed.freedBytes);
        EXPECT_EQ(sample.completed.nonOwnedFrees, 0);
        EXPECT_EQ(sample.completed.allocations, sample.held.allocations);
    }
    EXPECT_EQ(
        three.held.liveBytes - three.inputs.liveBytes,
        3 * (one.held.liveBytes - one.inputs.liveBytes));
    EXPECT_EQ(three.containerCapacityBytes, 3 * one.containerCapacityBytes);
}

TEST_P(BallastRetention, signature_failure_is_observed_without_leaving_retained_allocations)
{
    auto fixture = measurement::loadFixture(GetParam(), rules);
    {
        xrpl::SerialIter iterator{xrpl::makeSlice(fixture.wire)};
        xrpl::STTx changed{iterator};
        changed.setFieldU32(xrpl::sfSequence, 2);
        fixture.wire = changed.getSerializer().getData();
    }
    auto const sample = measure(fixture, rules, 3);
    EXPECT_EQ(sample.retainedObjects, 3);
    EXPECT_EQ(sample.validSignatures, 0);
    EXPECT_EQ(sample.matchingTransactionIds, 0);
    EXPECT_EQ(sample.completed.liveBytes, 0);
    EXPECT_EQ(sample.completed.allocations, sample.completed.frees);
}

INSTANTIATE_TEST_SUITE_P(Matrix, BallastRetention, testing::ValuesIn(measurement::kFixtureSlugs));

TEST(BallastRetentionExceptions, partial_input_setup_is_released_after_parse_failure)
{
    std::unordered_set<xrpl::uint256, beast::Uhash<>> const presets{xrpl::featureQuantum};
    xrpl::Rules const rules{presets};
    auto fixture = measurement::loadFixture(measurement::kFixtureSlugs.front(), rules);
    fixture.wire = {0xff};
    auto const before = allocation::liveTrackingStates();
    EXPECT_THROW(static_cast<void>(measure(fixture, rules, 3)), std::exception);
    EXPECT_EQ(allocation::liveTrackingStates(), before);
}

TEST(BallastRetentionExceptions, zero_and_excessive_counts_are_rejected)
{
    std::unordered_set<xrpl::uint256, beast::Uhash<>> const presets{xrpl::featureQuantum};
    xrpl::Rules const rules{presets};
    auto const fixture = measurement::loadFixture(measurement::kFixtureSlugs.front(), rules);
    EXPECT_THROW(static_cast<void>(measure(fixture, rules, 0)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(measure(fixture, rules, 1001)), std::invalid_argument);
}

}  // namespace ballast::retention::tests
