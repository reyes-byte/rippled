#pragma once

#include <benchmarks/libxrpl/ballast/measurement/Measurement.h>

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

namespace ballast::retention {

inline constexpr std::size_t kMaximumCount = 1000;

struct Sample
{
    allocation::Snapshot inputs;
    allocation::Snapshot held;
    allocation::Snapshot completed;
    std::size_t retainedObjects = 0;
    std::size_t consumedInputs = 0;
    std::size_t validSignatures = 0;
    std::size_t matchingTransactionIds = 0;
    std::size_t containerCapacityBytes = 0;
    std::size_t wirePayloadBytes = 0;
    std::size_t wireCapacityBytes = 0;
    std::int64_t elapsedNanoseconds = 0;
};

// Hold independent wire copies and actual parsed STTx objects together. All
// container, wire and parsed storage is released before completing the scope.
Sample
measure(measurement::Fixture const& fixture, xrpl::Rules const& rules, std::size_t count);
void
writeCsvHeader(std::ostream& out);
void
writeCsvRow(
    std::ostream& out,
    measurement::Fixture const& fixture,
    Sample const& sample,
    std::size_t count,
    std::size_t sampleIndex,
    std::size_t warmups,
    std::string_view runId);
std::string
metadataJson(std::string_view runId, std::size_t samples, std::size_t warmups);

}  // namespace ballast::retention
