// Compiled only against the pinned PQ candidate, never the root protocol ABI.
#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/beast/hash/uhash.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/HashPrefix.h>
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
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace xrpl {
namespace {

constexpr char kPassphrase[] = "ballast/ml-dsa-44/accountset/v1";
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;

void
require(bool success, char const* operation)
{
    if (!success)
        throw std::runtime_error(operation);
}

std::array<unsigned char, 64>
opensslSha512(Slice bytes)
{
    std::array<unsigned char, 64> digest{};
    unsigned int size = 0;
    require(
        EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha512(), nullptr) == 1,
        "OpenSSL SHA-512 failed");
    require(size == digest.size(), "Unexpected SHA-512 length");
    return digest;
}

Key
opensslKeyFromXrplSeed(Seed const& seed)
{
    // The fork expands the 16-byte XRPL seed by SHA512Half to obtain xi.
    // Compute that expansion independently rather than calling its hash helper.
    auto xi = opensslSha512(Slice(seed.data(), seed.size()));
    KeyContext context(
        EVP_PKEY_CTX_new_from_name(nullptr, "ML-DSA-44", "provider=default"), EVP_PKEY_CTX_free);
    require(bool(context), "Create ML-DSA key context");
    require(EVP_PKEY_fromdata_init(context.get()) == 1, "Initialize seed import");
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string("seed", xi.data(), 32), OSSL_PARAM_construct_end()};
    EVP_PKEY* raw = nullptr;
    require(EVP_PKEY_fromdata(context.get(), &raw, EVP_PKEY_KEYPAIR, params) == 1, "Import xi");
    return Key(raw, EVP_PKEY_free);
}

Blob
exportKey(EVP_PKEY* key, char const* parameter, std::size_t size)
{
    Blob bytes(size);
    std::size_t actual = 0;
    require(
        EVP_PKEY_get_octet_string_param(key, parameter, bytes.data(), size, &actual) == 1,
        "Export key component");
    require(actual == size, "Unexpected key length");
    return bytes;
}

bool
opensslVerify(Slice publicKey, Slice signature, Slice message)
{
    Key key(
        EVP_PKEY_new_raw_public_key_ex(
            nullptr, "ML-DSA-44", "provider=default", publicKey.data(), publicKey.size()),
        EVP_PKEY_free);
    require(bool(key), "Import ML-DSA public key");
    DigestContext digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(bool(digest), "Allocate verify context");
    require(
        EVP_DigestVerifyInit(digest.get(), nullptr, nullptr, nullptr, key.get()) == 1,
        "Initialize verify");
    return EVP_DigestVerify(
               digest.get(), signature.data(), signature.size(), message.data(), message.size()) ==
        1;
}

Blob
opensslSign(EVP_PKEY* key, Slice message)
{
    DigestContext digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(bool(digest), "Allocate signing context");
    require(
        EVP_DigestSignInit(digest.get(), nullptr, nullptr, nullptr, key) == 1,
        "Initialize signing");
    Blob signature(2420);
    auto size = signature.size();
    require(
        EVP_DigestSign(digest.get(), signature.data(), &size, message.data(), message.size()) == 1,
        "OpenSSL sign transaction payload");
    require(size == signature.size(), "Unexpected signature length");
    return signature;
}

Blob
singleSigningPayload(STTx const& tx)
{
    // STTx.cpp's getSigningData is file-local. Use the same public serializer
    // operations for the independent check; no alternate codec is involved.
    Serializer data;
    data.add32(HashPrefix::TxSign);
    tx.addWithoutSigningFields(data);
    return data.getData();
}

STTx
makeSignedAccountSet(
    KeyType type = KeyType::Dilithium,
    std::optional<AccountID> accountOverride = std::nullopt)
{
    auto const passphrase =
        type == KeyType::Secp256k1 ? "ballast/secp256k1/accountset/v1" : kPassphrase;
    auto const [publicKey, secretKey] = generateKeyPair(type, generateSeed(passphrase));
    STTx tx(ttACCOUNT_SET, [&](STObject& object) {
        object.setAccountID(sfAccount, accountOverride.value_or(calcAccountID(publicKey)));
        object.setFieldU32(sfSequence, 1);
        object.setFieldU32(sfFlags, tfFullyCanonicalSig);
        object.setFieldAmount(sfFee, STAmount(10ull));
        object.setFieldVL(sfSigningPubKey, publicKey.slice());
    });
    tx.sign(publicKey, secretKey);  // Uses the candidate's actual cryptoPrng adapter.
    return tx;
}

class BallastPqTransaction : public testing::Test
{
protected:
    // Rules borrow presets: the set must be constructed first and outlive them.
    std::unordered_set<uint256, beast::Uhash<>> const quantumPresets{featureQuantum};
    Rules const quantumRules{quantumPresets};
};

TEST_F(BallastPqTransaction, native_seed_derivation_and_public_key_recovery_match_openssl)
{
    auto const seed = generateSeed(kPassphrase);
    auto const expectedSeed = opensslSha512(Slice(kPassphrase, sizeof(kPassphrase) - 1));
    EXPECT_EQ(Slice(seed.data(), seed.size()), Slice(expectedSeed.data(), 16));
    auto const [publicKey, secretKey] = generateKeyPair(KeyType::Dilithium, seed);
    auto const independent = opensslKeyFromXrplSeed(seed);
    Blob const expectedPublic = exportKey(independent.get(), "pub", 1312);
    Blob const expectedSecret = exportKey(independent.get(), "priv", 2560);
    EXPECT_EQ(publicKey.slice(), makeSlice(expectedPublic));
    EXPECT_EQ(Slice(secretKey.data(), secretKey.size()), makeSlice(expectedSecret));
    EXPECT_EQ(derivePublicKey(KeyType::Dilithium, secretKey), publicKey);
}

TEST_F(BallastPqTransaction, native_transaction_serializes_parses_and_cross_verifies)
{
    auto const original = makeSignedAccountSet();
    Blob const wire = original.getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    EXPECT_EQ(parsed.getSerializer().getData(), wire);
    EXPECT_EQ(parsed.getTransactionID(), original.getTransactionID());
    EXPECT_EQ(parsed.getTxnType(), ttACCOUNT_SET);
    EXPECT_EQ(parsed.getFieldU32(sfSequence), 1);
    EXPECT_EQ(parsed.getFieldAmount(sfFee), STAmount(10ull));
    Blob const publicKey = parsed.getSigningPubKey();
    Blob const signature = parsed.getSignature();
    ASSERT_EQ(publicKey.size(), 1312);
    ASSERT_EQ(signature.size(), 2420);
    auto const result = parsed.checkSign(quantumRules);
    ASSERT_TRUE(result.has_value()) << result.error();
    Blob const payload = singleSigningPayload(parsed);
    EXPECT_TRUE(opensslVerify(makeSlice(publicKey), makeSlice(signature), makeSlice(payload)));
    EXPECT_FALSE(opensslVerify(makeSlice(publicKey), makeSlice(signature), makeSlice(wire)));
    std::cout
        << "Ballast PQ native fixture: algorithm=ML-DSA-44 signing_mode=single signer_count=1\n"
        << "passphrase=" << kPassphrase << " (public test credentials)\n"
        << "candidate_commit=" << BALLAST_PQ_CANDIDATE_COMMIT
        << " reference_commit=" << BALLAST_PQ_REFERENCE_COMMIT
        << "\nopenssl=" << OpenSSL_version(OPENSSL_VERSION)
        << "\naccount=" << toBase58(parsed.getAccountID(sfAccount))
        << " sequence=1 fee_drops=10 rules=featureQuantum_only\n"
        << "public_key_bytes=" << publicKey.size()
        << " public_key_object_bytes=" << sizeof(PublicKey)
        << " secret_key_bytes=2560 signing_payload_bytes=" << payload.size()
        << "\nwire_bytes=" << wire.size() << " signature_bytes=" << signature.size()
        << "\ntransaction_id=" << to_string(parsed.getTransactionID())
        << "\nwire_hex=" << strHex(wire)
        << "\nsignature_valid=true ledger_authorization=not_checked memory=not_measured\n";
}

TEST_F(BallastPqTransaction, changing_signed_sequence_is_rejected_after_parsing)
{
    auto tx = makeSignedAccountSet();
    tx.setFieldU32(sfSequence, 2);
    Blob const wire = tx.getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    EXPECT_FALSE(parsed.checkSign(quantumRules).has_value());
}

TEST_F(BallastPqTransaction, disabled_quantum_rules_reject_a_cryptographically_valid_signature)
{
    std::unordered_set<uint256, beast::Uhash<>> const emptyPresets;
    Rules const disabledRules{emptyPresets};
    Blob const wire = makeSignedAccountSet().getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    EXPECT_TRUE(parsed.checkSign(quantumRules).has_value());
    EXPECT_FALSE(parsed.checkSign(disabledRules).has_value());
    Blob const payload = singleSigningPayload(parsed);
    EXPECT_TRUE(opensslVerify(
        makeSlice(parsed.getSigningPubKey()),
        makeSlice(parsed.getSignature()),
        makeSlice(payload)));
}

TEST_F(BallastPqTransaction, native_checker_accepts_openssl_signature_on_actual_signing_payload)
{
    auto tx = makeSignedAccountSet();
    auto const key = opensslKeyFromXrplSeed(generateSeed(kPassphrase));
    Blob const payload = singleSigningPayload(tx);
    tx.setFieldVL(sfTxnSignature, makeSlice(opensslSign(key.get(), makeSlice(payload))));
    // Parse again so the transaction ID is calculated from the replaced signature.
    Blob const wire = tx.getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    auto const result = parsed.checkSign(quantumRules);
    EXPECT_TRUE(result.has_value()) << result.error();
}

TEST_F(BallastPqTransaction, signature_check_does_not_establish_ledger_authorization)
{
    auto const other =
        generateKeyPair(KeyType::Secp256k1, generateSeed("ballast/unrelated-account/v1"));
    Blob const wire = makeSignedAccountSet(KeyType::Dilithium, calcAccountID(other.first))
                          .getSerializer()
                          .getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    Blob const key = parsed.getSigningPubKey();
    EXPECT_NE(parsed.getAccountID(sfAccount), calcAccountID(PublicKey(makeSlice(key))));
    EXPECT_TRUE(parsed.checkSign(quantumRules).has_value());
    // A regular key could be ledger-authorized; no ledger is consulted here.
}

TEST_F(BallastPqTransaction, secp256k1_control_preserves_original_fixture_id)
{
    Blob const wire = makeSignedAccountSet(KeyType::Secp256k1).getSerializer().getData();
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    EXPECT_EQ(wire.size(), 151);
    EXPECT_EQ(parsed.getSigningPubKey().size(), 33);
    EXPECT_EQ(
        to_string(parsed.getTransactionID()),
        "39A8B8A605F55C35F928C651FD0547E1D8844117335A2B9C3D1F9ACA6F6A7F2B");
    EXPECT_TRUE(parsed.checkSign(quantumRules).has_value());
}

TEST_F(BallastPqTransaction, replay_saved_wire_fixture)
{
    std::ifstream input(BALLAST_PQ_FIXTURE_HEX_PATH);
    ASSERT_TRUE(input.is_open()) << BALLAST_PQ_FIXTURE_HEX_PATH;
    std::string hex;
    ASSERT_TRUE(bool(std::getline(input, hex)));
    auto const decoded = strUnHex(hex);
    ASSERT_TRUE(decoded.has_value());
    Blob const& wire = *decoded;  // decoded owns the bytes until this test returns.
    ASSERT_FALSE(wire.empty());
    std::array<unsigned char, 32> hash{};
    unsigned int hashSize = 0;
    ASSERT_EQ(
        EVP_Digest(wire.data(), wire.size(), hash.data(), &hashSize, EVP_sha256(), nullptr), 1);
    ASSERT_EQ(hashSize, hash.size());
    EXPECT_EQ(strHex(makeSlice(hash)), BALLAST_PQ_FIXTURE_SHA256);
    SerialIter iterator{makeSlice(wire)};
    STTx const parsed{iterator};
    EXPECT_TRUE(iterator.empty());
    EXPECT_EQ(parsed.getSerializer().getData(), wire);
    EXPECT_EQ(parsed.getSigningPubKey().size(), 1312);
    EXPECT_EQ(parsed.getSignature().size(), 2420);
    EXPECT_EQ(
        parsed.getAccountID(sfAccount),
        calcAccountID(generateKeyPair(KeyType::Dilithium, generateSeed(kPassphrase)).first));
    EXPECT_EQ(parsed.getTxnType(), ttACCOUNT_SET);
    EXPECT_EQ(parsed.getFieldU32(sfSequence), 1);
    EXPECT_EQ(parsed.getFieldAmount(sfFee), STAmount(10ull));
    EXPECT_EQ(to_string(parsed.getTransactionID()), BALLAST_PQ_FIXTURE_TXID);
    auto const result = parsed.checkSign(quantumRules);
    ASSERT_TRUE(result.has_value()) << result.error();
    Blob const payload = singleSigningPayload(parsed);
    EXPECT_TRUE(opensslVerify(
        makeSlice(parsed.getSigningPubKey()),
        makeSlice(parsed.getSignature()),
        makeSlice(payload)));
    std::cout << "replayed_transaction_id=" << to_string(parsed.getTransactionID()) << '\n';
}

}  // namespace
}  // namespace xrpl
