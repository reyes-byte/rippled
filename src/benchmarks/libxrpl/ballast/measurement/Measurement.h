#pragma once

#include <xrpl/basics/Blob.h>
#include <xrpl/protocol/Rules.h>

#include <benchmarks/libxrpl/ballast/accounting/AllocationTracker.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace ballast::measurement {

enum class Stage { Parse, Verify, Combined };
inline constexpr std::array kStages{Stage::Parse, Stage::Verify, Stage::Combined};
inline constexpr std::array<std::string_view, 6> kFixtureSlugs{
    "secp256k1-1",
    "secp256k1-8",
    "secp256k1-32",
    "ml-dsa-44-1",
    "ml-dsa-44-8",
    "ml-dsa-44-32"};

struct Fixture
{
    std::string id;
    std::string algorithm;
    std::size_t signerCount;
    xrpl::Blob wire;
    std::string wireSha256;
    std::string transactionId;
};

struct Sample
{
    allocation::Snapshot operation;
    allocation::Snapshot completed;
    std::optional<bool> signatureValid;  // Unset for parse-only.
    bool inputConsumed = false;
    std::int64_t elapsedNanoseconds = 0;
};

// Loading, fingerprint checks, native round-trip/signature prevalidation, and
// all temporary loading objects finish before returning the retained wire.
Fixture
loadFixture(std::string_view slug, xrpl::Rules const& rules);
Sample
measure(Stage stage, xrpl::Blob const& wire, xrpl::Rules const& rules);
std::string_view
stageName(Stage stage);
void
writeCsvHeader(std::ostream& out);
void
writeCsvRow(
    std::ostream& out,
    Fixture const& fixture,
    Stage stage,
    Sample const& sample,
    std::string_view runId,
    std::size_t index,
    std::size_t warmups);
std::string
metadataJson(std::string_view runId, std::size_t samples, std::size_t warmups);

}  // namespace ballast::measurement
