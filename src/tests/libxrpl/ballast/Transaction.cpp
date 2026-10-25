#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/beast/hash/uhash.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Rules.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>

#include <gtest/gtest.h>
#include <openssl/crypto.h>

#include <iostream>
#include <optional>
#include <unordered_set>

namespace xrpl {
namespace {

// Public synthetic test credentials. Never use this passphrase for funds.
constexpr char kPassphrase[] = "ballast/secp256k1/accountset/v1";

STTx
makeSignedAccountSet(std::optional<AccountID> accountOverride = std::nullopt)
{
    auto const [publicKey, secretKey] =
        generateKeyPair(KeyType::Secp256k1, generateSeed(kPassphrase));
    STTx tx(ttACCOUNT_SET, [&](STObject& object) {
        object.setAccountID(sfAccount, accountOverride.value_or(calcAccountID(publicKey)));
        object.setFieldU32(sfSequence, 1);
        object.setFieldU32(sfFlags, tfFullyCanonicalSig);
        object.setFieldAmount(sfFee, STAmount(10ull));
        object.setFieldVL(sfSigningPubKey, publicKey.slice());
    });
    tx.sign(publicKey, secretKey);
    return tx;
}

TEST(BallastTransaction, deterministic_account_set_serializes_parses_and_checks_signature)
{
    // Rules retain a reference to presets, so declare presets first.
    std::unordered_set<UInt256, beast::Uhash<>> const presets;
    Rules const rules{presets};
    auto const original = makeSignedAccountSet();
    Blob const wire = original.getSerializer().getData();
    EXPECT_EQ(wire, makeSignedAccountSet().getSerializer().getData());

    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    EXPECT_EQ(parsed.getSerializer().getData(), wire);
    EXPECT_EQ(parsed.getTransactionID(), original.getTransactionID());
    EXPECT_EQ(parsed.getTxnType(), ttACCOUNT_SET);
    EXPECT_EQ(parsed.getFieldU32(sfSequence), 1);
    EXPECT_EQ(parsed.getFieldAmount(sfFee), STAmount(10ull));
    EXPECT_EQ(parsed.getSigningPubKey().size(), 33);
    EXPECT_FALSE(parsed.getSignature().empty());
    auto const result = parsed.checkSign(rules);
    ASSERT_TRUE(result.has_value()) << result.error();

    std::cout << "Ballast fixture: algorithm=secp256k1 signing_mode=single signer_count=1\n"
              << "passphrase=" << kPassphrase << " (public test credentials)\n"
              << "branch=" << BALLAST_SOURCE_BRANCH << " base_commit=" << BALLAST_SOURCE_COMMIT
              << " build_scope=" << BALLAST_BUILD_SCOPE
              << "\nopenssl=" << OpenSSL_version(OPENSSL_VERSION)
              << "\naccount=" << toBase58(parsed.getAccountID(sfAccount))
              << " sequence=1 fee_drops=10 rules=no_amendment_presets\n"
              << "public_key_bytes=" << parsed.getSigningPubKey().size()
              << " public_key_object_bytes=" << sizeof(PublicKey) << '\n'
              << "wire_bytes=" << wire.size() << " signature_bytes=" << parsed.getSignature().size()
              << "\ntransaction_id=" << to_string(parsed.getTransactionID())
              << "\nwire_hex=" << strHex(wire)
              << "\nsignature_valid=true ledger_authorization=not_checked memory=not_measured\n";
}

TEST(BallastTransaction, changing_signed_sequence_is_rejected_after_parsing)
{
    std::unordered_set<UInt256, beast::Uhash<>> const presets;
    Rules const rules{presets};
    auto changed = makeSignedAccountSet();
    changed.setFieldU32(sfSequence, 2);  // Keep the old signature.
    Blob const wire = changed.getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    EXPECT_FALSE(parsed.checkSign(rules).has_value());
}

TEST(BallastTransaction, signature_checker_does_not_establish_ledger_authorization)
{
    std::unordered_set<UInt256, beast::Uhash<>> const presets;
    Rules const rules{presets};
    auto const otherKeys =
        generateKeyPair(KeyType::Secp256k1, generateSeed("ballast/unrelated-account/v1"));
    auto const signedTx = makeSignedAccountSet(calcAccountID(otherKeys.first));
    Blob const wire = signedTx.getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    Blob const signingKey = parsed.getSigningPubKey();
    EXPECT_NE(parsed.getAccountID(sfAccount), calcAccountID(PublicKey(makeSlice(signingKey))));
    EXPECT_TRUE(iterator.empty());
    auto const result = parsed.checkSign(rules);
    EXPECT_TRUE(result.has_value()) << result.error();
    // A ledger could authorize this key as a regular key. No ledger is present
    // here, so this proves neither authorization nor lack of authorization.
}

}  // namespace
}  // namespace xrpl
