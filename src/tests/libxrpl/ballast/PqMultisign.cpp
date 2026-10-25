#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/beast/hash/uhash.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Rules.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>

#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace xrpl::ballast_multi_test {

struct MatrixCase
{
    KeyType type;
    std::size_t count;
};

constexpr std::array<MatrixCase, 6> kCases{
    {{KeyType::Secp256k1, 1},
     {KeyType::Secp256k1, 8},
     {KeyType::Secp256k1, 32},
     {KeyType::Dilithium, 1},
     {KeyType::Dilithium, 8},
     {KeyType::Dilithium, 32}}};

char const*
algorithm(MatrixCase c)
{
    return c.type == KeyType::Secp256k1 ? "secp256k1" : "ML-DSA-44";
}

std::string
signerNamespace(MatrixCase c)
{
    return c.type == KeyType::Secp256k1 ? "ballast/secp256k1/multisign/v1"
                                        : "ballast/ml-dsa-44/multisign/v1";
}

void
sortSigners(STArray& signers)
{
    std::sort(signers.begin(), signers.end(), [](STObject const& lhs, STObject const& rhs) {
        return lhs.getAccountID(sfAccount) < rhs.getAccountID(sfAccount);
    });
}

STTx
makeNativeMultisign(MatrixCase c)
{
    if (c.count < STTx::kMinMultiSigners || c.count > STTx::kMaxMultiSigners)
        throw std::invalid_argument("Signer count outside protocol bounds");
    // Same transaction owner and common fields for both algorithms and counts.
    // This also preserves the earlier root-checkout secp256k1 fixtures.
    auto const owner =
        generateKeyPair(KeyType::Secp256k1, generateSeed("ballast/secp256k1/multisign/v1/account"));
    return STTx(ttACCOUNT_SET, [&](STObject& object) {
        object.setAccountID(sfAccount, calcAccountID(owner.first));
        object.setFieldU32(sfSequence, 1);
        object.setFieldU32(sfFlags, tfFullyCanonicalSig);
        object.setFieldAmount(sfFee, STAmount(10ull));  // Not a fee sufficiency check.
        object.setFieldVL(sfSigningPubKey, Slice{});

        Serializer const shared = startMultiSigningData(object, HashPrefix::TxMultiSign);
        STArray signers(sfSigners, c.count);  // Reserves capacity; starts empty.
        for (std::size_t i = 0; i < c.count; ++i)
        {
            auto const [publicKey, secretKey] = generateKeyPair(
                c.type, generateSeed(signerNamespace(c) + "/signer/" + std::to_string(i)));
            auto const account = calcAccountID(publicKey);
            Serializer payload = shared;
            finishMultiSigningData(account, payload);

            STObject signer(sfSigner);
            signer.setAccountID(sfAccount, account);
            signer.setFieldVL(sfSigningPubKey, publicKey.slice());
            signer.setFieldVL(sfTxnSignature, sign(publicKey, secretKey, payload.slice()));
            signers.pushBack(std::move(signer));
        }
        sortSigners(signers);
        object.setFieldArray(sfSigners, signers);
        // All signatures are attached before STTx calculates its cached ID.
    });
}

bool
verifyMlWithOpenSSL(Slice keyBytes, Slice signature, Slice message)
{
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Key key(
        EVP_PKEY_new_raw_public_key_ex(
            nullptr, "ML-DSA-44", "provider=default", keyBytes.data(), keyBytes.size()),
        EVP_PKEY_free);
    if (!key)
        throw std::runtime_error("Import ML-DSA public key");
    Context context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1)
        throw std::runtime_error("Initialize ML-DSA verification");
    return EVP_DigestVerify(
               context.get(), signature.data(), signature.size(), message.data(), message.size()) ==
        1;
}

std::string
wireSha256(Blob const& wire)
{
    std::array<unsigned char, 32> hash{};
    unsigned int size = 0;
    if (EVP_Digest(wire.data(), wire.size(), hash.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != hash.size())
        throw std::runtime_error("Hash saved wire fixture");
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (auto byte : hash)
    {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

class BallastNativeMultisign : public testing::TestWithParam<MatrixCase>
{
protected:
    // Rules borrow this set; member declaration order ensures its lifetime.
    std::unordered_set<uint256, beast::Uhash<>> const presets{featureQuantum};
    Rules const rules{presets};
};

TEST_P(BallastNativeMultisign, roundtrips_checks_and_cross_verifies_actual_payloads)
{
    auto const c = GetParam();
    auto const original = makeNativeMultisign(c);
    Blob const wire = original.getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    EXPECT_EQ(parsed.getSerializer().getData(), wire);
    EXPECT_EQ(parsed.getTransactionID(), original.getTransactionID());
    EXPECT_EQ(parsed.getTxnType(), ttACCOUNT_SET);
    EXPECT_EQ(parsed.getFieldU32(sfSequence), 1);
    EXPECT_EQ(parsed.getFieldAmount(sfFee), STAmount(10ull));
    EXPECT_TRUE(parsed.getSigningPubKey().empty());
    EXPECT_FALSE(parsed.isFieldPresent(sfTxnSignature));
    auto const& signers = parsed.getFieldArray(sfSigners);
    ASSERT_EQ(signers.size(), c.count);
    Serializer const shared = startMultiSigningData(parsed, HashPrefix::TxMultiSign);
    EXPECT_EQ(shared.size(), 50);
    std::size_t totalSignatures = 0;
    std::string signatureLengths;
    for (std::size_t i = 0; i < signers.size(); ++i)
    {
        SCOPED_TRACE(i);
        auto const account = signers[i].getAccountID(sfAccount);
        EXPECT_NE(account, parsed.getAccountID(sfAccount));
        if (i)
            EXPECT_LT(signers[i - 1].getAccountID(sfAccount), account);
        Blob const key = signers[i].getFieldVL(sfSigningPubKey);
        Blob const signature = signers[i].getFieldVL(sfTxnSignature);
        EXPECT_EQ(key.size(), c.type == KeyType::Dilithium ? 1312 : 33);
        EXPECT_EQ(calcAccountID(PublicKey(makeSlice(key))), account);
        EXPECT_FALSE(signature.empty());
        Serializer payload = shared;
        finishMultiSigningData(account, payload);
        EXPECT_EQ(payload.size(), 70);
        EXPECT_TRUE(
            std::equal(account.begin(), account.end(), payload.slice().data() + shared.size()));
        if (c.type == KeyType::Dilithium)
        {
            EXPECT_EQ(signature.size(), 2420);
            EXPECT_TRUE(verifyMlWithOpenSSL(makeSlice(key), makeSlice(signature), payload.slice()));
        }
        totalSignatures += signature.size();
        if (i)
            signatureLengths += ',';
        signatureLengths += std::to_string(signature.size());
    }
    auto const result = parsed.checkSign(rules);
    ASSERT_TRUE(result.has_value()) << result.error();
    if (c.type == KeyType::Secp256k1)
        EXPECT_EQ(wire, makeNativeMultisign(c).getSerializer().getData());

    std::cout << "BALLAST_NATIVE_MULTISIGN {\"algorithm\":\"" << algorithm(c)
              << "\",\"signer_count\":" << c.count << ",\"signer_namespace\":\""
              << signerNamespace(c) << "\",\"account\":\""
              << toBase58(parsed.getAccountID(sfAccount)) << "\",\"first_signer_account\":\""
              << toBase58(signers[0].getAccountID(sfAccount)) << "\",\"shared_signing_hex\":\""
              << strHex(shared.slice()) << "\",\"shared_signing_bytes\":" << shared.size()
              << ",\"per_signer_signing_bytes\":70,\"wire_bytes\":" << wire.size()
              << ",\"public_key_bytes_per_signer\":" << (c.type == KeyType::Dilithium ? 1312 : 33)
              << ",\"total_signature_bytes\":" << totalSignatures << ",\"signature_lengths\":["
              << signatureLengths << "],\"transaction_id\":\""
              << to_string(parsed.getTransactionID()) << "\",\"candidate_commit\":\""
              << BALLAST_PQ_CANDIDATE_COMMIT << "\",\"reference_commit\":\""
              << BALLAST_PQ_REFERENCE_COMMIT << "\",\"wire_hex\":\"" << strHex(wire) << "\"}\n";
}

TEST_P(BallastNativeMultisign, changing_sequence_is_rejected)
{
    auto changed = makeNativeMultisign(GetParam());
    changed.setFieldU32(sfSequence, 2);
    Blob const wire = changed.getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    EXPECT_FALSE(parsed.checkSign(rules).has_value());
}

TEST_P(BallastNativeMultisign, changing_last_signature_is_rejected)
{
    auto changed = makeNativeMultisign(GetParam());
    auto& last = changed.peekFieldArray(sfSigners).back();
    Blob signature = last.getFieldVL(sfTxnSignature);
    ASSERT_FALSE(signature.empty());
    signature[0] ^= 1;
    last.setFieldVL(sfTxnSignature, makeSlice(signature));
    Blob const wire = changed.getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    auto const result = parsed.checkSign(rules);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Invalid signature on account "), std::string::npos);
}

TEST_P(BallastNativeMultisign, relabeling_account_is_rejected_even_after_sorting)
{
    auto const c = GetParam();
    auto changed = makeNativeMultisign(c);
    auto const replacement = calcAccountID(
        generateKeyPair(c.type, generateSeed(signerNamespace(c) + "/replacement")).first);
    auto& signers = changed.peekFieldArray(sfSigners);
    ASSERT_NE(replacement, changed.getAccountID(sfAccount));
    ASSERT_TRUE(std::none_of(signers.begin(), signers.end(), [&](STObject const& entry) {
        return entry.getAccountID(sfAccount) == replacement;
    }));
    signers[0].setAccountID(sfAccount, replacement);
    sortSigners(signers);
    Blob const wire = changed.getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    auto const result = parsed.checkSign(rules);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Invalid signature on account "), std::string::npos);
}

TEST_P(BallastNativeMultisign, quantum_gate_affects_ml_dsa_but_preserves_secp256k1)
{
    std::unordered_set<uint256, beast::Uhash<>> const emptyPresets;
    Rules const disabledRules{emptyPresets};
    Blob const wire = makeNativeMultisign(GetParam()).getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    EXPECT_TRUE(parsed.checkSign(rules).has_value());
    EXPECT_EQ(parsed.checkSign(disabledRules).has_value(), GetParam().type == KeyType::Secp256k1);
}

TEST_P(BallastNativeMultisign, replays_saved_fixture_with_matching_identity_and_signatures)
{
    auto const c = GetParam();
    auto const slug = std::string(c.type == KeyType::Dilithium ? "ml-dsa-44-" : "secp256k1-") +
        std::to_string(c.count);
    auto const base = std::string(BALLAST_MULTISIGN_FIXTURE_DIR) + "/" + slug;
    std::ifstream metadataInput(base + ".json");
    ASSERT_TRUE(metadataInput.is_open());
    ::json::Reader reader;
    ::json::Value metadata;
    ASSERT_TRUE(reader.parse(metadataInput, metadata));
    EXPECT_EQ(metadata["candidate_commit"].asString(), BALLAST_PQ_CANDIDATE_COMMIT);
    EXPECT_EQ(metadata["reference_commit"].asString(), BALLAST_PQ_REFERENCE_COMMIT);
    EXPECT_EQ(metadata["algorithm"].asString(), algorithm(c));
    EXPECT_EQ(metadata["signer_count"].asUInt(), c.count);

    std::ifstream hexInput(base + ".hex");
    ASSERT_TRUE(hexInput.is_open());
    std::string hex;
    ASSERT_TRUE(bool(std::getline(hexInput, hex)));
    auto const decoded = strUnHex(hex);
    ASSERT_TRUE(decoded.has_value());
    Blob const& wire = *decoded;  // The optional owns storage for this whole test.
    EXPECT_EQ(wireSha256(wire), metadata["wire_sha256"].asString());
    EXPECT_EQ(wire.size(), metadata["wire_bytes"].asUInt());
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    EXPECT_EQ(parsed.getSerializer().getData(), wire);
    EXPECT_EQ(to_string(parsed.getTransactionID()), metadata["transaction_id"].asString());
    EXPECT_EQ(toBase58(parsed.getAccountID(sfAccount)), metadata["account"].asString());
    EXPECT_EQ(parsed.getTxnType(), ttACCOUNT_SET);
    EXPECT_EQ(parsed.getFieldU32(sfSequence), 1);
    EXPECT_EQ(parsed.getFieldAmount(sfFee), STAmount(10ull));
    EXPECT_TRUE(parsed.getSigningPubKey().empty());
    EXPECT_FALSE(parsed.isFieldPresent(sfTxnSignature));
    auto const& signers = parsed.getFieldArray(sfSigners);
    ASSERT_EQ(signers.size(), c.count);
    ASSERT_EQ(metadata["signature_lengths"].size(), c.count);
    Serializer const shared = startMultiSigningData(parsed, HashPrefix::TxMultiSign);
    EXPECT_EQ(strHex(shared.slice()), metadata["shared_signing_hex"].asString());
    EXPECT_EQ(shared.size(), metadata["shared_signing_bytes"].asUInt());
    EXPECT_EQ(shared.size(), 50);
    std::size_t totalSignatures = 0;
    for (std::size_t i = 0; i < signers.size(); ++i)
    {
        SCOPED_TRACE(i);
        Blob const key = signers[i].getFieldVL(sfSigningPubKey);
        Blob const signature = signers[i].getFieldVL(sfTxnSignature);
        EXPECT_EQ(key.size(), metadata["public_key_bytes_per_signer"].asUInt());
        EXPECT_EQ(
            signature.size(), metadata["signature_lengths"][static_cast<unsigned int>(i)].asUInt());
        totalSignatures += signature.size();
        Serializer payload = shared;
        finishMultiSigningData(signers[i].getAccountID(sfAccount), payload);
        EXPECT_EQ(payload.size(), metadata["per_signer_signing_bytes"].asUInt());
        EXPECT_EQ(payload.size(), 70);
        if (c.type == KeyType::Dilithium)
            EXPECT_TRUE(verifyMlWithOpenSSL(makeSlice(key), makeSlice(signature), payload.slice()));
    }
    EXPECT_EQ(totalSignatures, metadata["total_signature_bytes"].asUInt());
    auto const result = parsed.checkSign(rules);
    ASSERT_TRUE(result.has_value()) << result.error();
    std::cout << "BALLAST_MULTISIGN_REPLAY " << slug << ' ' << wireSha256(wire) << ' '
              << to_string(parsed.getTransactionID()) << '\n';
}

TEST(BallastNativeMultisignStructure, duplicates_are_rejected_for_both_algorithms)
{
    std::unordered_set<uint256, beast::Uhash<>> const presets{featureQuantum};
    Rules const rules{presets};
    for (auto type : {KeyType::Secp256k1, KeyType::Dilithium})
    {
        SCOPED_TRACE(algorithm({type, 8}));
        auto changed = makeNativeMultisign({type, 8});
        auto& signers = changed.peekFieldArray(sfSigners);
        signers[1] = signers[0];
        Blob const wire = changed.getSerializer().getData();
        SerialIter iterator{makeSlice(wire)};
        STTx const parsed{iterator};
        EXPECT_TRUE(iterator.empty());
        auto const result = parsed.checkSign(rules);
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().find("Duplicate Signers"), std::string::npos);
    }
}

TEST(BallastNativeMultisignStructure, unsorted_entries_are_rejected_for_both_algorithms)
{
    std::unordered_set<uint256, beast::Uhash<>> const presets{featureQuantum};
    Rules const rules{presets};
    for (auto type : {KeyType::Secp256k1, KeyType::Dilithium})
    {
        SCOPED_TRACE(algorithm({type, 8}));
        auto changed = makeNativeMultisign({type, 8});
        auto& signers = changed.peekFieldArray(sfSigners);
        std::swap(signers[0], signers[1]);
        Blob const wire = changed.getSerializer().getData();
        SerialIter iterator{makeSlice(wire)};
        STTx const parsed{iterator};
        EXPECT_TRUE(iterator.empty());
        auto const result = parsed.checkSign(rules);
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().find("Unsorted Signers"), std::string::npos);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Cases,
    BallastNativeMultisign,
    testing::ValuesIn(kCases),
    [](testing::TestParamInfo<MatrixCase> const& info) {
        return std::string(info.param.type == KeyType::Dilithium ? "MlDsa44_" : "Secp256k1_") +
            std::to_string(info.param.count);
    });

}  // namespace xrpl::ballast_multi_test
