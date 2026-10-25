#include <benchmarks/libxrpl/pqc/Crypto.h>
#include <benchmarks/libxrpl/pqc/Layout.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

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
        EXPECT_THROW(parseShared(std::make_shared<Bytes const>(input)), std::runtime_error);
    }
}

TEST(PqcFraming, empty_fields_need_no_payload_allocations)
{
    Bytes const input{0, 0};
    ParseStats stats;
    auto const owned = parseOwned(input, &stats);
    EXPECT_TRUE(owned.views().publicKey.empty());
    EXPECT_TRUE(parseViews(input).signature.empty());
    auto const shared = parseShared(std::make_shared<Bytes const>(input));
    EXPECT_TRUE(shared.views().publicKey.empty());
    EXPECT_TRUE(shared.views().signature.empty());
    EXPECT_EQ(stats.payloadAllocations, 0);
    EXPECT_EQ(stats.copiedBytes, 0);
}

TEST(PqcFraming, shared_fields_reject_null_and_default_to_empty)
{
    EXPECT_THROW(parseShared(nullptr), std::invalid_argument);
    SharedFields const empty;
    EXPECT_TRUE(empty.views().publicKey.empty());
    EXPECT_TRUE(empty.views().signature.empty());
}

TEST(PqcFraming, invalid_edits_preserve_borrowed_fields)
{
    auto const wire = std::make_shared<Bytes const>(Bytes{2, 1, 2, 1, 3});
    auto parsed = parseShared(wire);
    auto const before = parsed.views();
    EXPECT_THROW(parsed.setPublicKeyByte(2, 99), std::out_of_range);
    EXPECT_THROW(parsed.setSignatureByte(1, 99), std::out_of_range);
    EXPECT_EQ(parsed.views().publicKey.data(), before.publicKey.data());
    EXPECT_EQ(parsed.views().signature.data(), before.signature.data());
    EXPECT_EQ(*wire, (Bytes{2, 1, 2, 1, 3}));

    SharedFields empty;
    EXPECT_THROW(empty.setPublicKeyByte(0, 99), std::out_of_range);
    EXPECT_THROW(empty.setSignatureByte(0, 99), std::out_of_range);
    auto emptyFields = parseShared(std::make_shared<Bytes const>(Bytes{0, 0}));
    EXPECT_THROW(emptyFields.setPublicKeyByte(0, 99), std::out_of_range);
    EXPECT_THROW(emptyFields.setSignatureByte(0, 99), std::out_of_range);
}

TEST(PqcFraming, shared_subranges_are_bounded_by_the_owner)
{
    auto const wire = std::make_shared<Bytes const>(Bytes{255, 2, 1, 2, 1, 3, 255});
    auto const parsed = parseShared(wire, 1, 5);
    EXPECT_EQ(parsed.views().publicKey.data(), wire->data() + 2);
    EXPECT_EQ(parsed.views().signature.data(), wire->data() + 5);
    EXPECT_EQ(parsed.views().publicKey.size(), 2);
    EXPECT_EQ(parsed.views().signature.size(), 1);
    EXPECT_THROW(parseShared(wire, wire->size() + 1, 0), std::out_of_range);
    EXPECT_THROW(parseShared(wire, 1, wire->size()), std::out_of_range);
    EXPECT_THROW(parseShared(wire, std::numeric_limits<std::size_t>::max(), 0), std::out_of_range);
    EXPECT_THROW(parseShared(wire, 1, 0), std::runtime_error);
    EXPECT_THROW(parseShared(wire, 1), std::runtime_error);  // trailing byte
    EXPECT_THROW(parseShared(nullptr, 1, 5), std::invalid_argument);
}

TEST(PqcWorkload, rejects_invalid_signer_counts)
{
    EXPECT_THROW(makeSignerWorkload("ED25519", 64, 42, 0), std::invalid_argument);
    EXPECT_THROW(makeSignerWorkload("ED25519", 64, 42, 33), std::invalid_argument);
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
    auto const shared = parseShared(std::make_shared<Bytes const>(fixture.encoded));
    EXPECT_TRUE(verify(GetParam(), fixture.message, shared.views()));
    bool const pq = std::string(GetParam()) == "ML-DSA-44";
    EXPECT_EQ(view.publicKey.size(), pq ? 1312 : 33);
    EXPECT_EQ(view.signature.size(), pq ? 2420 : 64);
}

TEST_P(PqcSignatures, one_signer_workload_matches_existing_fixture)
{
    auto const fixture = makeFixture(GetParam(), 64, 42);
    auto const workload = makeSignerWorkload(GetParam(), 64, 42, 1);
    ASSERT_EQ(workload.signers.size(), 1);
    EXPECT_EQ(*workload.wire, fixture.encoded);
    EXPECT_EQ(workload.signers[0].message, fixture.message);
    EXPECT_EQ(workload.signers[0].offset, 0);
    EXPECT_EQ(workload.signers[0].encodedBytes, fixture.encoded.size());
}

TEST_P(PqcSignatures, thirty_two_signers_share_one_wire_and_all_verify)
{
    // Exercise seed wraparound too: signer 1 uses seed zero.
    auto workload =
        makeSignerWorkload(GetParam(), 64, std::numeric_limits<std::uint32_t>::max(), 32);
    ASSERT_EQ(workload.signers.size(), 32);
    std::weak_ptr<Bytes const> lifetime = workload.wire;
    std::vector<OwnedFields> owned;
    std::vector<SharedFields> shared;
    std::set<Bytes> keys;
    ParseStats stats;
    std::size_t nextOffset = 0;
    for (auto const& record : workload.signers)
    {
        EXPECT_EQ(record.offset, nextOffset);
        auto const input = View(*workload.wire).subspan(record.offset, record.encodedBytes);
        auto const view = parseViews(input);
        owned.push_back(parseOwned(input, &stats));
        shared.push_back(parseShared(workload.wire, record.offset, record.encodedBytes));
        EXPECT_EQ(shared.back().views().publicKey.data(), view.publicKey.data());
        EXPECT_EQ(shared.back().views().signature.data(), view.signature.data());
        EXPECT_NE(owned.back().views().publicKey.data(), view.publicKey.data());
        keys.emplace(view.publicKey.begin(), view.publicKey.end());
        nextOffset += record.encodedBytes;
    }
    EXPECT_EQ(nextOffset, workload.wire->size());
    EXPECT_EQ(keys.size(), 32);
    EXPECT_EQ(stats.payloadAllocations, 64);
    EXPECT_EQ(stats.copiedBytes, std::string(GetParam()) == "ML-DSA-44" ? 119424 : 3104);
    EXPECT_FALSE(verify(GetParam(), workload.signers[1].message, shared[0].views()));

    // All 64 field payloads are live here. Shared wrappers retain the original
    // buffer even after the workload releases it; owned fields are independent.
    workload.wire.reset();
    EXPECT_FALSE(lifetime.expired());
    for (std::size_t i = 0; i < workload.signers.size(); ++i)
    {
        EXPECT_TRUE(verify(GetParam(), workload.signers[i].message, owned[i].views()));
        EXPECT_TRUE(verify(GetParam(), workload.signers[i].message, shared[i].views()));
    }
    shared.clear();
    EXPECT_TRUE(lifetime.expired());
}

TEST_P(PqcSignatures, shared_fields_survive_caller_release_copy_and_move)
{
    auto fixture = makeFixture(GetParam(), 64, 42);
    SharedFields survivor;
    std::weak_ptr<Bytes const> lifetime;
    {
        auto wire = std::make_shared<Bytes const>(std::move(fixture.encoded));
        lifetime = wire;
        auto const borrowed = parseViews(*wire);
        auto parsed = parseShared(wire);
        EXPECT_EQ(parsed.views().publicKey.data(), borrowed.publicKey.data());
        EXPECT_EQ(parsed.views().signature.data(), borrowed.signature.data());
        survivor = parsed;
        wire.reset();
        EXPECT_FALSE(lifetime.expired());
        EXPECT_TRUE(verify(GetParam(), fixture.message, parsed.views()));
    }
    EXPECT_FALSE(lifetime.expired());
    auto moved = std::move(survivor);
    EXPECT_TRUE(survivor.views().publicKey.empty());
    EXPECT_TRUE(survivor.views().signature.empty());
    EXPECT_TRUE(verify(GetParam(), fixture.message, moved.views()));
    moved = SharedFields{};
    EXPECT_TRUE(lifetime.expired());
}

TEST_P(PqcSignatures, signature_edits_detach_and_preserve_other_wrappers)
{
    auto fixture = makeFixture(GetParam(), 64, 42);
    auto const wire = std::make_shared<Bytes const>(std::move(fixture.encoded));
    auto const original = parseShared(wire);
    auto const before = original.views();
    auto edited = original;
    EXPECT_EQ(edited.views().signature.data(), before.signature.data());

    auto const originalByte = before.signature.front();
    edited.setSignatureByte(0, originalByte ^ 1);
    EXPECT_NE(edited.views().signature.data(), before.signature.data());
    EXPECT_EQ(edited.views().publicKey.data(), before.publicKey.data());
    EXPECT_EQ(original.views().signature.front(), originalByte);
    EXPECT_EQ(parseViews(*wire).signature.front(), originalByte);
    EXPECT_TRUE(verify(GetParam(), fixture.message, original.views()));
    EXPECT_FALSE(verify(GetParam(), fixture.message, edited.views()));

    // A second edit of an unshared materialized field reuses its storage.
    auto const materialized = edited.views().signature.data();
    edited.setSignatureByte(0, originalByte);
    EXPECT_EQ(edited.views().signature.data(), materialized);
    EXPECT_TRUE(verify(GetParam(), fixture.message, edited.views()));

    // Copying an already edited wrapper shares that field until the next write.
    auto fork = edited;
    EXPECT_EQ(fork.views().signature.data(), materialized);
    fork.setSignatureByte(0, originalByte ^ 1);
    EXPECT_NE(fork.views().signature.data(), materialized);
    EXPECT_EQ(edited.views().signature.data(), materialized);
    EXPECT_TRUE(verify(GetParam(), fixture.message, edited.views()));
    EXPECT_FALSE(verify(GetParam(), fixture.message, fork.views()));
}

TEST_P(PqcSignatures, public_key_edits_leave_signature_and_wire_unchanged)
{
    auto fixture = makeFixture(GetParam(), 64, 42);
    auto const wire = std::make_shared<Bytes const>(std::move(fixture.encoded));
    auto const original = parseShared(wire);
    auto const before = original.views();
    auto edited = original;
    auto const changedByte = static_cast<unsigned char>(before.publicKey.front() ^ 1);
    edited.setPublicKeyByte(0, changedByte);
    EXPECT_EQ(edited.views().publicKey.front(), changedByte);
    EXPECT_NE(edited.views().publicKey.data(), before.publicKey.data());
    EXPECT_EQ(edited.views().signature.data(), before.signature.data());
    EXPECT_EQ(original.views().publicKey.front(), before.publicKey.front());
    EXPECT_EQ(parseViews(*wire).publicKey.front(), before.publicKey.front());
    EXPECT_TRUE(verify(GetParam(), fixture.message, original.views()));
}

TEST_P(PqcSignatures, rejects_every_truncation)
{
    auto const fixture = makeFixture(GetParam(), 64, 42);
    for (std::size_t length = 0; length < fixture.encoded.size(); ++length)
    {
        View const truncated(fixture.encoded.data(), length);
        EXPECT_THROW(parseViews(truncated), std::runtime_error);
        EXPECT_THROW(parseOwned(truncated), std::runtime_error);
        EXPECT_THROW(
            parseShared(std::make_shared<Bytes const>(truncated.begin(), truncated.end())),
            std::runtime_error);
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
        auto const shared = parseShared(std::make_shared<Bytes const>(wire));
        EXPECT_FALSE(verify(GetParam(), message, shared.views()));
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
