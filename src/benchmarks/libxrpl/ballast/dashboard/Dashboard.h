#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ballast::dashboard {

inline constexpr std::array<char const*, 3> kStages{"parse_only", "verify_only", "parse_verify"};
inline constexpr std::array<char const*, 4> kMetrics{
    "peak_live_bytes",
    "requested_bytes",
    "live_bytes_at_operation_end",
    "elapsed_ns"};

struct Statistic
{
    double minimum;
    double median;
    double maximum;
};

struct Group
{
    std::string algorithm;
    std::size_t signers;
    std::string stage;
    std::size_t samples;
    std::array<Statistic, 4> metrics;
    std::size_t variants;
};

struct View
{
    std::string id;
    std::size_t runs;
    std::size_t rows;
    std::vector<Group> groups;
};

struct Snapshot
{
    std::vector<View> views;
    std::string candidate;
    std::string crypto;
    std::string measurementHash;
    std::string compiler;
    std::string openssl;
    std::string sourceBundleHash;
    std::size_t recordedTests = 0;
};

struct State
{
    std::size_t stage = 0;
    std::size_t metric = 0;
    std::size_t view = 0;
    std::size_t selected = 5;
    bool provenance = false;

    // Returns false when the user requests exit.
    bool
    handle(char key, std::size_t viewCount);
};

Snapshot
loadSnapshot(std::filesystem::path const& path);
std::vector<Group const*>
visibleGroups(Snapshot const& snapshot, State const& state);
std::string
safeText(std::string text);
std::string
formatValue(double value, std::size_t metric);
std::vector<std::string>
render(
    Snapshot const& snapshot,
    State const& state,
    std::size_t width = 110,
    std::size_t height = 26);
int
interactive(Snapshot const& snapshot, State& state);

}  // namespace ballast::dashboard
