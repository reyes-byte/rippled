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

// Copies share storage; edits detach only the field being changed.
class SharedFields
{
    std::shared_ptr<Bytes const> owner_;
    Fields fields_{};
    std::shared_ptr<Bytes> publicKey_;
    std::shared_ptr<Bytes> signature_;

    SharedFields(std::shared_ptr<Bytes const> owner, Fields fields);
    friend SharedFields
    parseShared(std::shared_ptr<Bytes const> owner, std::size_t offset, std::size_t bytes);

public:
    SharedFields() = default;

    // Views borrow storage. Reacquire them after editing this wrapper.
    [[nodiscard]] Fields
    views() const;

    // Bounds-checked, fixed-size edits; no mutable spans escape the wrapper.
    void
    setPublicKeyByte(std::size_t index, unsigned char value);

    void
    setSignatureByte(std::size_t index, unsigned char value);
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

// The owner must have no mutable aliases. Rejects null owners and invalid ranges.
SharedFields
parseShared(
    std::shared_ptr<Bytes const> owner,
    std::size_t offset = 0,
    std::size_t bytes = std::dynamic_extent);

// Models STBlob/getVLBuffer ownership: one exact-size allocation and memcpy
// per nonempty field. Counters exclude input storage and crypto internals.
OwnedFields
parseOwned(View input, ParseStats* stats = nullptr);

// Out-of-line observation keeps parse-only work visible to the optimizer.
std::uint64_t
observe(Fields const& fields);

}  // namespace pqc_bench
