#pragma once

#include <benchmarks/libxrpl/pqc/Layout.h>

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

// Imports a public key on every call in both parsing policies. No key cache.
bool
verify(std::string const& algorithm, View message, Fields const& fields);

std::string
sha256(View input);

}  // namespace pqc_bench
