#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pqc_bench {

using Bytes = std::vector<unsigned char>;
using View = std::span<unsigned char const>;

// XRPL's variable-length field limit, not a whole-transaction size limit.
inline constexpr std::size_t kMaxFieldBytes = 918744;

struct Fields
{
    View publicKey;
    View signature;
};

struct OwnedFields
{
    std::unique_ptr<unsigned char[]> publicKey;
    std::unique_ptr<unsigned char[]> signature;
    std::size_t publicKeyBytes = 0;
    std::size_t signatureBytes = 0;

    [[nodiscard]] Fields
    views() const;
};

struct ParseStats
{
    std::size_t payloadAllocations = 0;
    std::size_t copiedBytes = 0;
};

// This is an extraction of XRPL VL framing, not a complete STObject parser.
void
appendField(Bytes& output, View field);

// Input must remain alive and immutable until the returned views are unused.
Fields
parseViews(View input);

// Models STBlob/getVLBuffer ownership: one exact-size allocation and memcpy
// per nonempty field. Counters exclude input storage and crypto internals.
OwnedFields
parseOwned(View input, ParseStats* stats = nullptr);

// Out-of-line observation keeps parse-only work visible to the optimizer.
std::uint64_t
observe(Fields const& fields);

}  // namespace pqc_bench
