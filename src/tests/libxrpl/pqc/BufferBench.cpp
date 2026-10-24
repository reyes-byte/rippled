#include <benchmarks/libxrpl/pqc/Crypto.h>
#include <benchmarks/libxrpl/pqc/Layout.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace pqc_bench {
namespace {

TEST(PqcFraming, matches_known_xrpl_length_prefixes)
{
    struct Example
    {
        std::size_t length;
        Bytes prefix;
    };
    for (auto const& example : std::array<Example, 6>{
             Example{0, {0}},
             {192, {192}},
             {193, {193, 0}},
             {12480, {240, 255}},
             {12481, {241, 0, 0}},
             {918744, {254, 212, 23}}})
    {
        SCOPED_TRACE(example.length);
        Bytes const field(example.length, 0x5a);
        Bytes encoded;
        appendField(encoded, field);
        ASSERT_GE(encoded.size(), example.prefix.size());
        EXPECT_TRUE(std::equal(example.prefix.begin(), example.prefix.end(), encoded.begin()));
        appendField(encoded, {});
        auto const views = parseViews(encoded);
        EXPECT_EQ(views.publicKey.size(), example.length);
        EXPECT_TRUE(std::equal(field.begin(), field.end(), views.publicKey.begin()));
        auto const owned = parseOwned(encoded);
        EXPECT_EQ(owned.publicKeyBytes, example.length);
    }
    Bytes output;
    EXPECT_THROW(appendField(output, Bytes(kMaxFieldBytes + 1)), std::runtime_error);
}

TEST(PqcFraming, rejects_invalid_and_incomplete_prefixes)
{
    for (auto const& input :
         std::array<Bytes, 6>{Bytes{255}, {193}, {241, 0}, {254, 212, 24}, {1}, {0, 0, 1}})
    {
        EXPECT_THROW(parseViews(input), std::runtime_error);
        EXPECT_THROW(parseOwned(input), std::runtime_error);
    }
}

TEST(PqcFraming, empty_fields_need_no_payload_allocations)
{
    Bytes const input{0, 0};
    ParseStats stats;
    auto const owned = parseOwned(input, &stats);
    EXPECT_TRUE(owned.views().publicKey.empty());
    EXPECT_TRUE(parseViews(input).signature.empty());
    EXPECT_EQ(stats.payloadAllocations, 0);
    EXPECT_EQ(stats.copiedBytes, 0);
}

TEST(PqcFraming, views_borrow_and_owners_copy)
{
    Bytes input;
    appendField(input, Bytes{1, 2, 3});
    appendField(input, Bytes{4, 5});
    auto const view = parseViews(input);
    ParseStats stats;
    auto const owned = parseOwned(input, &stats);
    EXPECT_EQ(view.publicKey.data(), input.data() + 1);
    EXPECT_EQ(view.signature.data(), input.data() + 5);
    EXPECT_NE(owned.views().publicKey.data(), view.publicKey.data());
    EXPECT_EQ(stats.payloadAllocations, 2);
    EXPECT_EQ(stats.copiedBytes, 5);
    input[1] = 99;
    EXPECT_EQ(view.publicKey.front(), 99);
    EXPECT_EQ(owned.views().publicKey.front(), 1);
}

class PqcSignatures : public ::testing::TestWithParam<char const*>
{
};

TEST_P(PqcSignatures, fixtures_are_reproducible_and_both_parsers_verify)
{
    auto const fixture = makeFixture(GetParam(), 1024, 42);
    auto const repeated = makeFixture(GetParam(), 1024, 42);
    EXPECT_EQ(fixture.message, repeated.message);
    EXPECT_EQ(fixture.encoded, repeated.encoded);
    auto const view = parseViews(fixture.encoded);
    auto const owned = parseOwned(fixture.encoded);
    EXPECT_TRUE(verify(GetParam(), fixture.message, view));
    EXPECT_TRUE(verify(GetParam(), fixture.message, owned.views()));
    bool const pq = std::string(GetParam()) == "ML-DSA-44";
    EXPECT_EQ(view.publicKey.size(), pq ? 1312 : 33);
    EXPECT_EQ(view.signature.size(), pq ? 2420 : 64);
}

TEST_P(PqcSignatures, rejects_every_truncation)
{
    auto const fixture = makeFixture(GetParam(), 64, 42);
    for (std::size_t length = 0; length < fixture.encoded.size(); ++length)
    {
        View const truncated(fixture.encoded.data(), length);
        EXPECT_THROW(parseViews(truncated), std::runtime_error);
        EXPECT_THROW(parseOwned(truncated), std::runtime_error);
    }
}

TEST_P(PqcSignatures, rejects_changed_message_key_and_signature)
{
    auto const fixture = makeFixture(GetParam(), 64, 42);
    auto const other = makeFixture(GetParam(), 64, 43);
    for (int mutation = 0; mutation < 3; ++mutation)
    {
        Bytes message = fixture.message;
        Bytes wire = fixture.encoded;
        if (mutation == 0)
            message.front() ^= 1;
        else if (mutation == 1)
        {
            auto const fields = parseViews(wire);
            auto const otherKey = parseViews(other.encoded).publicKey;
            std::copy(
                otherKey.begin(),
                otherKey.end(),
                wire.begin() + (fields.publicKey.data() - wire.data()));
        }
        else
            wire.back() ^= 1;
        EXPECT_FALSE(verify(GetParam(), message, parseViews(wire)));
        auto const owner = parseOwned(wire);
        EXPECT_FALSE(verify(GetParam(), message, owner.views()));
    }
}

TEST_P(PqcSignatures, empty_message_verifies)
{
    auto const fixture = makeFixture(GetParam(), 0, 42);
    EXPECT_TRUE(verify(GetParam(), fixture.message, parseViews(fixture.encoded)));
}

INSTANTIATE_TEST_SUITE_P(Algorithms, PqcSignatures, ::testing::Values("ED25519", "ML-DSA-44"));

}  // namespace
}  // namespace pqc_bench
