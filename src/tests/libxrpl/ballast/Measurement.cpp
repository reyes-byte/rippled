#include <benchmarks/libxrpl/ballast/measurement/Measurement.h>

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
#include <vector>

namespace ballast::measurement::tests {

struct Case
{
    std::string_view slug;
    Stage stage;
};
std::vector<Case>
cases()
{
    std::vector<Case> result;
    for (auto slug : kFixtureSlugs)
        for (auto stage : kStages)
            result.push_back({slug, stage});
    return result;
}

class BallastMeasurement : public testing::TestWithParam<Case>
{
protected:
    std::unordered_set<xrpl::uint256, beast::Uhash<>> const presets{xrpl::featureQuantum};
    xrpl::Rules const rules{presets};
};

TEST_P(BallastMeasurement, real_transaction_lifetimes_match_stage_boundaries)
{
    auto const c = GetParam();
    auto const fixture = loadFixture(c.slug, rules);
    auto const sample = measure(c.stage, fixture.wire, rules);
    EXPECT_TRUE(sample.inputConsumed);
    EXPECT_GE(sample.elapsedNanoseconds, 0);
    EXPECT_GT(sample.completed.allocations, 0);
    EXPECT_GT(sample.completed.peakLiveBytes, 0);
    EXPECT_EQ(sample.completed.allocations, sample.completed.frees);
    EXPECT_EQ(sample.completed.requestedBytes, sample.completed.freedBytes);
    EXPECT_EQ(sample.completed.liveBytes, 0);
    EXPECT_EQ(sample.completed.liveBlocks, 0);
    EXPECT_EQ(sample.completed.nonOwnedFrees, 0);
    if (c.stage == Stage::Verify)
        EXPECT_EQ(sample.operation.liveBytes, 0);
    else
        EXPECT_GT(sample.operation.liveBytes, 0);
    if (c.stage == Stage::Parse)
        EXPECT_FALSE(sample.signatureValid.has_value());
    else
        EXPECT_EQ(sample.signatureValid, true);
}

INSTANTIATE_TEST_SUITE_P(
    Matrix,
    BallastMeasurement,
    testing::ValuesIn(cases()),
    [](testing::TestParamInfo<Case> const& info) { return "Case" + std::to_string(info.index); });

class BallastMeasurementInvalid : public testing::TestWithParam<std::string_view>
{
};

TEST_P(BallastMeasurementInvalid, changed_sequence_stays_invalid_inside_measurement)
{
    std::unordered_set<xrpl::uint256, beast::Uhash<>> const presets{xrpl::featureQuantum};
    xrpl::Rules const rules{presets};
    auto const fixture = loadFixture(GetParam(), rules);
    xrpl::Blob changedWire;
    {
        xrpl::SerialIter iterator{xrpl::makeSlice(fixture.wire)};
        xrpl::STTx changed{iterator};
        changed.setFieldU32(xrpl::sfSequence, 2);
        changedWire = changed.getSerializer().getData();
    }
    for (auto stage : {Stage::Verify, Stage::Combined})
    {
        SCOPED_TRACE(stageName(stage));
        auto const sample = measure(stage, changedWire, rules);
        EXPECT_TRUE(sample.inputConsumed);
        EXPECT_EQ(sample.signatureValid, false);
        EXPECT_EQ(sample.completed.liveBytes, 0);
        EXPECT_EQ(sample.completed.allocations, sample.completed.frees);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Matrix,
    BallastMeasurementInvalid,
    testing::ValuesIn(kFixtureSlugs),
    [](testing::TestParamInfo<std::string_view> const& info) {
        return "Case" + std::to_string(info.index);
    });

TEST(BallastMeasurementExceptions, parse_failure_restores_tracking_lifetime)
{
    std::unordered_set<xrpl::uint256, beast::Uhash<>> const presets{xrpl::featureQuantum};
    xrpl::Rules const rules{presets};
    // Match the benchmark's prevalidation policy. Cold CountedObject type-name
    // registration can retain global storage; that is not failed-parse leakage.
    static_cast<void>(loadFixture(kFixtureSlugs.front(), rules));
    auto const before = allocation::liveTrackingStates();
    EXPECT_THROW(static_cast<void>(measure(Stage::Parse, xrpl::Blob{}, rules)), std::exception);
    EXPECT_EQ(allocation::liveTrackingStates(), before);
}

}  // namespace ballast::measurement::tests
