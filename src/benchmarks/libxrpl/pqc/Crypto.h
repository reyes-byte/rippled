#pragma once

#include <benchmarks/libxrpl/pqc/Layout.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace pqc_bench {

struct Fixture
{
    Bytes message;
    Bytes encoded;
};

// Public, reproducible test keys derived from the seed. Never use for funds.
Fixture
makeFixture(std::string const& algorithm, std::size_t messageBytes, std::uint32_t seed);

inline constexpr std::size_t kMaxSigners = 32;

struct SignerRecord
{
    std::size_t offset;
    std::size_t encodedBytes;
    Bytes message;
};

struct SignerWorkload
{
    std::shared_ptr<Bytes const> wire;
    std::vector<SignerRecord> signers;
};

// Independent synthetic signer records, not a complete XRPL multisigned transaction.
// Signer i uses seed + i modulo 2^32 for both its key and its own message.
SignerWorkload
makeSignerWorkload(
    std::string const& algorithm,
    std::size_t messageBytes,
    std::uint32_t seed,
    std::size_t signerCount);

// Imports a public key on every call in all parsing policies. No key cache.
bool
verify(std::string const& algorithm, View message, Fields const& fields);

std::string
sha256(View input);

}  // namespace pqc_bench
