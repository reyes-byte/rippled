// An interoperability gate for the candidate fork's crypto dependency.
// No XRPL transaction, ledger, allocation counter, or production RNG is tested.
extern "C" {
#include <api.h>
}

#include <gtest/gtest.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;
using View = std::span<unsigned char const>;
using Seed = std::array<unsigned char, 32>;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

// One-shot public test entropy. Not a replacement for XRPL's cryptoPrng().
thread_local std::optional<Seed> nextTestEntropy;

void
require(bool success, char const* operation)
{
    if (!success)
    {
        std::array<char, 256> error{};
        ERR_error_string_n(ERR_get_error(), error.data(), error.size());
        throw std::runtime_error(std::string(operation) + ": " + error.data());
    }
}

Seed
testSeed(unsigned char offset = 0)
{
    Seed seed{};
    for (std::size_t i = 0; i < seed.size(); ++i)
        seed[i] = static_cast<unsigned char>(offset + i);
    return seed;
}

Key
opensslKey(Seed seed)
{
    KeyContext context(
        EVP_PKEY_CTX_new_from_name(nullptr, "ML-DSA-44", "provider=default"), EVP_PKEY_CTX_free);
    require(bool(context), "create default-provider key context");
    require(EVP_PKEY_fromdata_init(context.get()) == 1, "initialize seed import");
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string("seed", seed.data(), seed.size()),
        OSSL_PARAM_construct_end()};
    EVP_PKEY* raw = nullptr;
    require(EVP_PKEY_fromdata(context.get(), &raw, EVP_PKEY_KEYPAIR, params) == 1, "import seed");
    return Key(raw, EVP_PKEY_free);
}

Bytes
exportKey(EVP_PKEY* key, char const* parameter, std::size_t size)
{
    Bytes result(size);
    std::size_t actual = 0;
    require(
        EVP_PKEY_get_octet_string_param(key, parameter, result.data(), size, &actual) == 1,
        "export key component");
    require(actual == size, "unexpected key component length");
    return result;
}

struct ReferenceKey
{
    Bytes publicKey = Bytes(pqcrystals_dilithium2_PUBLICKEYBYTES);
    Bytes secretKey = Bytes(pqcrystals_dilithium2_SECRETKEYBYTES);
};

ReferenceKey
referenceKey(Seed seed)
{
    ReferenceKey key;
    nextTestEntropy = seed;
    require(
        pqcrystals_dilithium2_ref_keypair(key.publicKey.data(), key.secretKey.data()) == 0,
        "reference key generation");
    require(!nextTestEntropy, "reference did not consume seed");
    return key;
}

Bytes
signReference(ReferenceKey const& key, View message, View context = {}, bool deterministic = true)
{
    if (deterministic)
        nextTestEntropy = Seed{};  // FIPS 204 deterministic variant: rnd = 0^32.
    Bytes signature(pqcrystals_dilithium2_BYTES);
    auto size = signature.size();
    require(
        pqcrystals_dilithium2_ref_signature(
            signature.data(),
            &size,
            message.data(),
            message.size(),
            context.data(),
            context.size(),
            key.secretKey.data()) == 0,
        "reference signing");
    require(!nextTestEntropy, "reference did not consume signing entropy");
    require(size == signature.size(), "unexpected reference signature length");
    return signature;
}

void
setContext(EVP_PKEY_CTX* context, View value)
{
    if (value.empty())
        return;  // Retain OpenSSL's standard empty-context encoding.
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(
            "context-string", const_cast<unsigned char*>(value.data()), value.size()),
        OSSL_PARAM_construct_end()};
    require(EVP_PKEY_CTX_set_params(context, params) == 1, "set ML-DSA context");
}

Bytes
signOpenSSL(EVP_PKEY* key, View message, View context = {}, bool deterministic = true)
{
    DigestContext digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(bool(digest), "allocate signing context");
    EVP_PKEY_CTX* signing = nullptr;
    require(
        EVP_DigestSignInit(digest.get(), &signing, nullptr, nullptr, key) == 1,
        "initialize signing");
    setContext(signing, context);
    int enabled = deterministic ? 1 : 0;
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_int("deterministic", &enabled), OSSL_PARAM_construct_end()};
    require(EVP_PKEY_CTX_set_params(signing, params) == 1, "set signing mode");
    Bytes signature(pqcrystals_dilithium2_BYTES);
    auto size = signature.size();
    require(
        EVP_DigestSign(digest.get(), signature.data(), &size, message.data(), message.size()) == 1,
        "OpenSSL signing");
    require(size == signature.size(), "unexpected OpenSSL signature length");
    return signature;
}

bool
verifyOpenSSL(View publicKey, View signature, View message, View context = {})
{
    Key key(
        EVP_PKEY_new_raw_public_key_ex(
            nullptr, "ML-DSA-44", "provider=default", publicKey.data(), publicKey.size()),
        EVP_PKEY_free);
    require(bool(key), "import public key");
    DigestContext digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(bool(digest), "allocate verification context");
    EVP_PKEY_CTX* verification = nullptr;
    require(
        EVP_DigestVerifyInit(digest.get(), &verification, nullptr, nullptr, key.get()) == 1,
        "initialize verification");
    setContext(verification, context);
    auto const result = EVP_DigestVerify(
        digest.get(), signature.data(), signature.size(), message.data(), message.size());
    ERR_clear_error();  // Malformed signatures may put errors on this thread's queue.
    return result == 1;
}

bool
verifyReference(View publicKey, View signature, View message, View context = {})
{
    return pqcrystals_dilithium2_ref_verify(
               signature.data(),
               signature.size(),
               message.data(),
               message.size(),
               context.data(),
               context.size(),
               publicKey.data()) == 0;
}

Bytes
testMessage(std::size_t size)
{
    Bytes message(size);
    for (std::size_t i = 0; i < size; ++i)
        message[i] = static_cast<unsigned char>(i & 255);
    return message;
}

std::string
sha256(View bytes)
{
    std::array<unsigned char, 32> hash{};
    unsigned int size = 0;
    require(
        EVP_Digest(bytes.data(), bytes.size(), hash.data(), &size, EVP_sha256(), nullptr) == 1,
        "hash artifact");
    std::ostringstream out;
    for (auto byte : hash)
        out << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
    return out.str();
}

class BallastPqInterop : public testing::TestWithParam<std::size_t>
{
};

TEST_P(BallastPqInterop, same_seed_keys_and_deterministic_signatures_match)
{
    auto const reference = referenceKey(testSeed());
    auto const openssl = opensslKey(testSeed());
    EXPECT_EQ(reference.publicKey, exportKey(openssl.get(), "pub", 1312));
    EXPECT_EQ(reference.secretKey, exportKey(openssl.get(), "priv", 2560));
    auto const message = testMessage(GetParam());
    auto const signature = signReference(reference, message);
    auto const other = signOpenSSL(openssl.get(), message);
    EXPECT_EQ(signature, other);
    EXPECT_TRUE(verifyOpenSSL(reference.publicKey, signature, message));
    EXPECT_TRUE(verifyReference(reference.publicKey, other, message));
    std::cout << "BALLAST_PQ_FIXTURE {\"scope\":\"crypto_dependency_interop\","
                 "\"input_kind\":\"byte_pattern\",\"seed_recipe\":\"00_to_1f\","
                 "\"signing_mode\":\"deterministic\",\"context_hex\":\"\",\"message_bytes\":"
              << message.size() << ",\"public_key_bytes\":" << reference.publicKey.size()
              << ",\"secret_key_bytes\":" << reference.secretKey.size()
              << ",\"signature_bytes\":" << signature.size() << ",\"public_key_sha256\":\""
              << sha256(reference.publicKey) << "\",\"message_sha256\":\"" << sha256(message)
              << "\",\"signature_sha256\":\"" << sha256(signature) << "\",\"openssl\":\""
              << OpenSSL_version(OPENSSL_VERSION) << "\",\"candidate_commit\":\""
              << BALLAST_PQ_CANDIDATE_COMMIT << "\",\"reference_commit\":\""
              << BALLAST_PQ_REFERENCE_COMMIT << "\"}\n";
}

INSTANTIATE_TEST_SUITE_P(MessageSizes, BallastPqInterop, testing::Values(0, 1, 70, 4096));

TEST(BallastPqChecks, nonempty_context_interoperates_and_is_bound_to_signature)
{
    auto const reference = referenceKey(testSeed());
    auto const openssl = opensslKey(testSeed());
    auto const message = testMessage(70);
    Bytes const context{'b', 'a', 'l', 'l', 'a', 's', 't'};
    auto const signature = signReference(reference, message, context);
    auto const other = signOpenSSL(openssl.get(), message, context);
    EXPECT_EQ(signature, other);
    EXPECT_TRUE(verifyOpenSSL(reference.publicKey, signature, message, context));
    EXPECT_TRUE(verifyReference(reference.publicKey, other, message, context));
    EXPECT_FALSE(verifyOpenSSL(reference.publicKey, signature, message));
    EXPECT_FALSE(verifyReference(reference.publicKey, other, message));
}

TEST(BallastPqChecks, altered_message_key_signature_and_truncation_are_rejected)
{
    auto const reference = referenceKey(testSeed());
    auto const wrongKey = referenceKey(testSeed(128));
    auto message = testMessage(70);
    auto signature = signReference(reference, message);
    auto rejectBoth = [&](View key, View sig, View msg) {
        EXPECT_FALSE(verifyOpenSSL(key, sig, msg));
        EXPECT_FALSE(verifyReference(key, sig, msg));
    };
    message[0] ^= 1;
    rejectBoth(reference.publicKey, signature, message);
    message[0] ^= 1;
    rejectBoth(wrongKey.publicKey, signature, message);
    signature[0] ^= 1;
    rejectBoth(reference.publicKey, signature, message);
    signature[0] ^= 1;
    signature.pop_back();
    rejectBoth(reference.publicKey, signature, message);
}

TEST(BallastPqChecks, randomized_signing_cross_verifies)
{
    auto const reference = referenceKey(testSeed());
    auto const openssl = opensslKey(testSeed());
    auto const message = testMessage(70);
    auto const signature = signReference(reference, message, {}, false);
    auto const other = signOpenSSL(openssl.get(), message, {}, false);
    EXPECT_EQ(signature.size(), 2420);
    EXPECT_EQ(other.size(), 2420);
    EXPECT_TRUE(verifyOpenSSL(reference.publicKey, signature, message));
    EXPECT_TRUE(verifyReference(reference.publicKey, other, message));
}

}  // namespace

// The pinned archive expects this symbol from its caller. In XRPL it uses
// cryptoPrng(). Here fixed entropy is consumed once for reproducible fixtures;
// randomized-signing checks use OpenSSL RAND_bytes. Neither adapter is a mock
// signature algorithm, but only the latter draws fresh randomness.
extern "C" void
randombytes(unsigned char* output, std::size_t size)
{
    if (nextTestEntropy)
    {
        if (size != nextTestEntropy->size())
            std::abort();
        std::copy(nextTestEntropy->begin(), nextTestEntropy->end(), output);
        nextTestEntropy.reset();
        return;
    }
    if (size > 32 || RAND_bytes(output, static_cast<int>(size)) != 1)
        std::abort();
}
