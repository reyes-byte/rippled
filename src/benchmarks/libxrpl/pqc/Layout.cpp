#include <benchmarks/libxrpl/pqc/Layout.h>

#include <cstring>
#include <stdexcept>
#include <utility>

namespace pqc_bench {
namespace {

class Reader
{
    View remaining_;

public:
    explicit Reader(View input) : remaining_(input)
    {
    }

    unsigned char
    byte()
    {
        if (remaining_.empty())
            throw std::runtime_error("truncated length prefix");
        auto const result = remaining_.front();
        remaining_ = remaining_.subspan(1);
        return result;
    }

    View
    field()
    {
        auto const first = byte();
        std::size_t length = first;
        if (first == 255)
            throw std::runtime_error("invalid length prefix");
        if (first >= 241)
        {
            auto const second = byte();
            auto const third = byte();
            length = 12481 + (first - 241) * 65536 + second * 256 + third;
        }
        else if (first >= 193)
        {
            auto const second = byte();
            length = 193 + (first - 193) * 256 + second;
        }
        if (length > kMaxFieldBytes)
            throw std::runtime_error("field exceeds XRPL VL limit");
        if (length > remaining_.size())
            throw std::runtime_error("truncated field payload");
        auto const result = remaining_.first(length);
        remaining_ = remaining_.subspan(length);
        return result;
    }

    void
    finish() const
    {
        if (!remaining_.empty())
            throw std::runtime_error("unexpected trailing bytes");
    }
};

std::unique_ptr<unsigned char[]>
copyField(View input, ParseStats* stats)
{
    if (input.empty())
        return {};
    auto output = std::make_unique_for_overwrite<unsigned char[]>(input.size());
    std::memcpy(output.get(), input.data(), input.size());
    if (stats)
    {
        ++stats->payloadAllocations;
        stats->copiedBytes += input.size();
    }
    return output;
}

void
setByte(View current, std::shared_ptr<Bytes>& owned, std::size_t index, unsigned char value)
{
    if (index >= current.size())
        throw std::out_of_range("field byte index out of range");
    // First edit copies the wire field. Editing a copied wrapper detaches again
    // if its materialized field is shared; otherwise reuse the existing storage.
    if (!owned || owned.use_count() > 1)
        owned = std::make_shared<Bytes>(current.begin(), current.end());
    (*owned)[index] = value;
}

}  // namespace

Fields
OwnedFields::views() const
{
    return {{publicKey.get(), publicKeyBytes}, {signature.get(), signatureBytes}};
}

SharedFields::SharedFields(std::shared_ptr<Bytes const> owner, Fields fields)
    : owner_(std::move(owner)), fields_(fields)
{
}

Fields
SharedFields::views() const
{
    if (!owner_)
        return {};
    return {
        publicKey_ ? View(*publicKey_) : fields_.publicKey,
        signature_ ? View(*signature_) : fields_.signature};
}

void
SharedFields::setPublicKeyByte(std::size_t index, unsigned char value)
{
    setByte(views().publicKey, publicKey_, index, value);
}

void
SharedFields::setSignatureByte(std::size_t index, unsigned char value)
{
    setByte(views().signature, signature_, index, value);
}

void
appendField(Bytes& output, View field)
{
    auto const length = field.size();
    if (length > kMaxFieldBytes)
        throw std::runtime_error("field exceeds XRPL VL limit");
    if (length <= 192)
        output.push_back(static_cast<unsigned char>(length));
    else if (length <= 12480)
    {
        auto const offset = length - 193;
        output.push_back(static_cast<unsigned char>(193 + offset / 256));
        output.push_back(static_cast<unsigned char>(offset % 256));
    }
    else
    {
        auto const offset = length - 12481;
        output.push_back(static_cast<unsigned char>(241 + offset / 65536));
        output.push_back(static_cast<unsigned char>((offset / 256) % 256));
        output.push_back(static_cast<unsigned char>(offset % 256));
    }
    output.insert(output.end(), field.begin(), field.end());
}

Fields
parseViews(View input)
{
    Reader reader(input);
    auto const publicKey = reader.field();
    auto const signature = reader.field();
    reader.finish();
    return {publicKey, signature};
}

SharedFields
parseShared(std::shared_ptr<Bytes const> owner, std::size_t offset, std::size_t bytes)
{
    if (!owner)
        throw std::invalid_argument("null wire buffer owner");
    if (offset > owner->size())
        throw std::out_of_range("wire buffer offset out of range");
    auto const remaining = owner->size() - offset;
    if (bytes == std::dynamic_extent)
        bytes = remaining;
    if (bytes > remaining)
        throw std::out_of_range("wire buffer length out of range");
    auto const fields = parseViews(View(*owner).subspan(offset, bytes));
    return SharedFields(std::move(owner), fields);
}

OwnedFields
parseOwned(View input, ParseStats* stats)
{
    Reader reader(input);
    OwnedFields result;
    auto const publicKey = reader.field();
    result.publicKeyBytes = publicKey.size();
    result.publicKey = copyField(publicKey, stats);
    auto const signature = reader.field();
    result.signatureBytes = signature.size();
    result.signature = copyField(signature, stats);
    reader.finish();
    return result;
}

std::uint64_t
observe(Fields const& fields)
{
    std::uint64_t result = 0;
    for (auto field : {fields.publicKey, fields.signature})
    {
        result += field.size();
        if (!field.empty())
            result += field.front() + field.back();
    }
    return result;
}

}  // namespace pqc_bench
