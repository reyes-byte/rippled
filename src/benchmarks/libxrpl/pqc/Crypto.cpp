#include <benchmarks/libxrpl/pqc/Crypto.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <array>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace pqc_bench {
namespace {

using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

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

Key
testKey(std::string const& algorithm, std::mt19937& generator)
{
    std::array<unsigned char, 32> seed{};
    for (auto& byte : seed)
        byte = static_cast<unsigned char>(generator() & 255);
    EVP_PKEY* raw = nullptr;
    if (algorithm == "ED25519")
        raw = EVP_PKEY_new_raw_private_key_ex(
            nullptr, algorithm.c_str(), nullptr, seed.data(), seed.size());
    else
    {
        KeyContext context(
            EVP_PKEY_CTX_new_from_name(nullptr, algorithm.c_str(), nullptr), EVP_PKEY_CTX_free);
        require(bool(context), "create key context");
        require(EVP_PKEY_fromdata_init(context.get()) == 1, "initialize key import");
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_octet_string("seed", seed.data(), seed.size()),
            OSSL_PARAM_construct_end()};
        require(
            EVP_PKEY_fromdata(context.get(), &raw, EVP_PKEY_KEYPAIR, params) == 1,
            "import deterministic test seed");
    }
    Key key(raw, EVP_PKEY_free);
    require(bool(key), "create test key");
    return key;
}

}  // namespace

Fixture
makeFixture(std::string const& algorithm, std::size_t messageBytes, std::uint32_t seed)
{
    std::mt19937 generator(seed);
    auto key = testKey(algorithm, generator);
    Fixture fixture;
    fixture.message.resize(messageBytes);
    for (auto& byte : fixture.message)
        byte = static_cast<unsigned char>(generator() & 255);

    std::size_t publicKeyBytes = 0;
    require(
        EVP_PKEY_get_raw_public_key(key.get(), nullptr, &publicKeyBytes) == 1,
        "measure public key");
    Bytes publicKey(publicKeyBytes);
    require(
        EVP_PKEY_get_raw_public_key(key.get(), publicKey.data(), &publicKeyBytes) == 1,
        "export public key");
    publicKey.resize(publicKeyBytes);
    // XRPL tags an Ed25519 public key with 0xED before its 32 raw bytes.
    if (algorithm == "ED25519")
        publicKey.insert(publicKey.begin(), 0xed);

    DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(bool(context), "create signing context");
    EVP_PKEY_CTX* signingContext = nullptr;
    require(
        EVP_DigestSignInit(context.get(), &signingContext, nullptr, nullptr, key.get()) == 1,
        "initialize signing");
    if (algorithm != "ED25519")
    {
        int deterministic = 1;
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_int("deterministic", &deterministic), OSSL_PARAM_construct_end()};
        require(EVP_PKEY_CTX_set_params(signingContext, params) == 1, "set deterministic signing");
    }
    std::size_t signatureBytes = 0;
    require(
        EVP_DigestSign(
            context.get(), nullptr, &signatureBytes, fixture.message.data(), messageBytes) == 1,
        "measure signature");
    Bytes signature(signatureBytes);
    require(
        EVP_DigestSign(
            context.get(),
            signature.data(),
            &signatureBytes,
            fixture.message.data(),
            messageBytes) == 1,
        "sign fixture");
    signature.resize(signatureBytes);
    appendField(fixture.encoded, publicKey);
    appendField(fixture.encoded, signature);
    require(verify(algorithm, fixture.message, parseViews(fixture.encoded)), "verify new fixture");
    return fixture;
}

SignerWorkload
makeSignerWorkload(
    std::string const& algorithm,
    std::size_t messageBytes,
    std::uint32_t seed,
    std::size_t signerCount)
{
    if (!signerCount || signerCount > kMaxSigners)
        throw std::invalid_argument("signer count must be between 1 and 32");
    SignerWorkload workload;
    workload.signers.reserve(signerCount);
    Bytes wire;
    for (std::size_t i = 0; i < signerCount; ++i)
    {
        auto fixture = makeFixture(algorithm, messageBytes, static_cast<std::uint32_t>(seed + i));
        workload.signers.push_back(
            {wire.size(), fixture.encoded.size(), std::move(fixture.message)});
        wire.insert(wire.end(), fixture.encoded.begin(), fixture.encoded.end());
    }
    workload.wire = std::make_shared<Bytes const>(std::move(wire));
    return workload;
}

bool
verify(std::string const& algorithm, View message, Fields const& fields)
{
    ERR_clear_error();
    auto publicKey = fields.publicKey;
    if (algorithm == "ED25519")
    {
        if (publicKey.size() != 33 || publicKey.front() != 0xed)
            return false;
        publicKey = publicKey.subspan(1);
    }
    Key key(
        EVP_PKEY_new_raw_public_key_ex(
            nullptr, algorithm.c_str(), nullptr, publicKey.data(), publicKey.size()),
        EVP_PKEY_free);
    if (!key)
    {
        ERR_clear_error();
        return false;
    }
    DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(bool(context), "create verification context");
    require(
        EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1,
        "initialize verification");
    int const result = EVP_DigestVerify(
        context.get(),
        fields.signature.data(),
        fields.signature.size(),
        message.data(),
        message.size());
    if (result < 0)
    {
        ERR_clear_error();
        return false;
    }
    return result == 1;
}

std::string
sha256(View input)
{
    std::array<unsigned char, 32> digest{};
    unsigned int length = 0;
    require(
        EVP_Digest(input.data(), input.size(), digest.data(), &length, EVP_sha256(), nullptr) == 1,
        "hash fixture");
    std::ostringstream output;
    for (auto byte : digest)
        output << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
    return output.str();
}

}  // namespace pqc_bench
