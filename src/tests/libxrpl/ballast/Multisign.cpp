#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/beast/hash/uhash.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/KeyType.h>
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

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace xrpl {
namespace {

// Public synthetic credentials. The same signer keys are used across counts.
constexpr char kMultisignNamespace[] = "ballast/secp256k1/multisign/v1";

void
sortBallastSigners(STArray& signers)
{
    std::sort(signers.begin(), signers.end(), [](STObject const& lhs, STObject const& rhs) {
        return lhs.getAccountID(sfAccount) < rhs.getAccountID(sfAccount);
    });
}

STTx
makeMultisignedAccountSet(std::size_t signerCount)
{
    if (signerCount < STTx::kMinMultiSigners || signerCount > STTx::kMaxMultiSigners)
        throw std::invalid_argument("Ballast signer count must be between 1 and 32");
    auto const ownerKeys = generateKeyPair(
        KeyType::Secp256k1, generateSeed(std::string(kMultisignNamespace) + "/account"));

    // Complete assembly inside the callback so STTx calculates its transaction
    // ID after all signatures are attached, rather than caching an unsigned ID.
    return STTx(ttACCOUNT_SET, [&](STObject& object) {
        object.setAccountID(sfAccount, calcAccountID(ownerKeys.first));
        object.setFieldU32(sfSequence, 1);
        object.setFieldU32(sfFlags, tfFullyCanonicalSig);
        object.setFieldAmount(sfFee, STAmount(10ull));  // Fixture constant, no fee validation.
        object.setFieldVL(sfSigningPubKey, Slice{});

        auto const shared = startMultiSigningData(object, HashPrefix::TxMultiSign);
        STArray signers(sfSigners, signerCount);
        for (std::size_t i = 0; i < signerCount; ++i)
        {
            auto const [publicKey, secretKey] = generateKeyPair(
                KeyType::Secp256k1,
                generateSeed(std::string(kMultisignNamespace) + "/signer/" + std::to_string(i)));
            auto const account = calcAccountID(publicKey);
            Serializer payload = shared;
            finishMultiSigningData(account, payload);

            STObject signer(sfSigner);
            signer.setAccountID(sfAccount, account);
            signer.setFieldVL(sfSigningPubKey, publicKey.slice());
            signer.setFieldVL(sfTxnSignature, sign(publicKey, secretKey, payload.slice()));
            signers.pushBack(std::move(signer));
        }
        sortBallastSigners(signers);
        object.setFieldArray(sfSigners, signers);
    });
}

class BallastMultisign : public ::testing::TestWithParam<std::size_t>
{
};

TEST_P(BallastMultisign, deterministic_transaction_roundtrips_and_verifies)
{
    std::unordered_set<UInt256, beast::Uhash<>> const presets;
    Rules const rules{presets};
    auto const original = makeMultisignedAccountSet(GetParam());
    Blob const wire = original.getSerializer().getData();
    EXPECT_EQ(wire, makeMultisignedAccountSet(GetParam()).getSerializer().getData());

    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    EXPECT_EQ(parsed.getSerializer().getData(), wire);
    EXPECT_EQ(parsed.getTransactionID(), original.getTransactionID());
    EXPECT_EQ(parsed.getTxnType(), ttACCOUNT_SET);
    EXPECT_TRUE(parsed.getSigningPubKey().empty());
    EXPECT_FALSE(parsed.isFieldPresent(sfTxnSignature));
    auto const& signers = parsed.getFieldArray(sfSigners);
    ASSERT_EQ(signers.size(), GetParam());

    auto const shared = startMultiSigningData(parsed, HashPrefix::TxMultiSign);
    std::size_t totalSignatureBytes = 0;
    std::string signatureLengths;
    for (std::size_t i = 0; i < signers.size(); ++i)
    {
        auto const account = signers[i].getAccountID(sfAccount);
        EXPECT_NE(account, parsed.getAccountID(sfAccount));
        if (i != 0)
            EXPECT_LT(signers[i - 1].getAccountID(sfAccount), account);
        EXPECT_EQ(signers[i].getFieldVL(sfSigningPubKey).size(), 33);
        auto const signature = signers[i].getFieldVL(sfTxnSignature);
        EXPECT_FALSE(signature.empty());
        totalSignatureBytes += signature.size();
        if (i != 0)
            signatureLengths += ',';
        signatureLengths += std::to_string(signature.size());

        Serializer payload = shared;
        finishMultiSigningData(account, payload);
        EXPECT_EQ(payload.size(), shared.size() + account.size());
        EXPECT_TRUE(
            std::equal(account.begin(), account.end(), payload.slice().data() + shared.size()));
    }
    auto const result = parsed.checkSign(rules);
    ASSERT_TRUE(result.has_value()) << result.error();

    std::cout << "Ballast multisign fixture: algorithm=secp256k1 signing_mode=multi signer_count="
              << GetParam() << "\npassphrase_namespace=" << kMultisignNamespace
              << "\nbranch=" << BALLAST_SOURCE_BRANCH << " base_commit=" << BALLAST_SOURCE_COMMIT
              << " build_scope=" << BALLAST_BUILD_SCOPE
              << "\naccount=" << toBase58(parsed.getAccountID(sfAccount))
              << " sequence=1 fee_drops=10 rules=no_amendment_presets\n"
              << "wire_bytes=" << wire.size() << " public_key_bytes_per_signer=33"
              << " total_signature_bytes=" << totalSignatureBytes
              << "\nsignature_lengths=" << signatureLengths
              << "\nshared_signing_bytes=" << shared.size()
              << " per_signer_signing_bytes=" << shared.size() + AccountID::size()
              << "\nfirst_signer_account=" << toBase58(signers[0].getAccountID(sfAccount))
              << "\ntransaction_id=" << to_string(parsed.getTransactionID())
              << "\nwire_hex=" << strHex(wire)
              << "\nsignature_valid=true ledger_authorization=not_checked memory=not_measured\n";
}

TEST_P(BallastMultisign, changing_signed_sequence_is_rejected)
{
    std::unordered_set<UInt256, beast::Uhash<>> const presets;
    Rules const rules{presets};
    auto changed = makeMultisignedAccountSet(GetParam());
    changed.setFieldU32(sfSequence, 2);
    Blob const wire = changed.getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    EXPECT_FALSE(parsed.checkSign(rules).has_value());
}

TEST_P(BallastMultisign, relabeling_a_signer_account_invalidates_its_signature)
{
    std::unordered_set<UInt256, beast::Uhash<>> const presets;
    Rules const rules{presets};
    auto changed = makeMultisignedAccountSet(GetParam());
    auto const replacementKeys = generateKeyPair(
        KeyType::Secp256k1, generateSeed(std::string(kMultisignNamespace) + "/replacement"));
    auto const replacement = calcAccountID(replacementKeys.first);
    EXPECT_NE(replacement, changed.getAccountID(sfAccount));
    auto& signers = changed.peekFieldArray(sfSigners);
    ASSERT_TRUE(std::none_of(signers.begin(), signers.end(), [&](STObject const& signer) {
        return signer.getAccountID(sfAccount) == replacement;
    }));
    signers[0].setAccountID(sfAccount, replacement);  // Keep its key and signature.
    sortBallastSigners(signers);  // Preserve ordering, so the signature check is exercised.
    Blob const wire = changed.getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    auto const result = parsed.checkSign(rules);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Invalid signature on account "), std::string::npos);
}

TEST(BallastMultisignStructure, duplicate_signers_are_rejected)
{
    std::unordered_set<UInt256, beast::Uhash<>> const presets;
    Rules const rules{presets};
    auto changed = makeMultisignedAccountSet(8);
    auto& signers = changed.peekFieldArray(sfSigners);
    signers[1] = signers[0];
    Blob const wire = changed.getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    auto const result = parsed.checkSign(rules);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Duplicate Signers"), std::string::npos);
}

TEST(BallastMultisignStructure, unsorted_signers_are_rejected)
{
    std::unordered_set<UInt256, beast::Uhash<>> const presets;
    Rules const rules{presets};
    auto changed = makeMultisignedAccountSet(8);
    auto& signers = changed.peekFieldArray(sfSigners);
    std::swap(signers[0], signers[1]);
    Blob const wire = changed.getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    auto const result = parsed.checkSign(rules);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Unsorted Signers"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(SignerCounts, BallastMultisign, ::testing::Values(1, 8, 32));

}  // namespace
}  // namespace xrpl
