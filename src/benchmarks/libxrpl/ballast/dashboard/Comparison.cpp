#include <Dashboard.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ballast::dashboard {
namespace {

std::string
column(std::string const& value, std::size_t width, bool right = false)
{
    auto const clipped = safeText(value).substr(0, width);
    auto const padding = std::string(width - clipped.size(), ' ');
    return right ? padding + clipped : clipped + padding;
}

std::string
changeText(Difference const& change)
{
    if (change.saved == 0)
        return "unchanged";
    if (!change.percent)
        return "increase (0 base)";
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << std::abs(*change.percent)
        << (change.saved > 0 ? "% less" : "% more");
    return out.str();
}

std::string
counts(double value)
{
    auto formatted = formatValue(value, 0);
    formatted.resize(formatted.size() - 2);  // Counts use the same numeric format, without B.
    return formatted;
}

}  // namespace

Difference
difference(double baseline, double optimized)
{
    double const saved = baseline - optimized;
    return {
        saved,
        baseline ? std::optional<double>(100 * saved / baseline)
                 : (optimized == 0 ? std::optional<double>(0) : std::nullopt)};
}

std::vector<std::pair<Group const*, Group const*>>
comparisonGroups(Snapshot const& snapshot, State const& state)
{
    if (!snapshot.comparison)
        throw std::runtime_error("No comparison data");
    State baselineState = state;
    baselineState.view = 0;
    auto const baseline = visibleGroups(snapshot, baselineState);
    std::vector<std::pair<Group const*, Group const*>> rows;
    for (auto const* group : baseline)
    {
        auto const& candidates = snapshot.views.at(1).groups;
        auto const found =
            std::find_if(candidates.begin(), candidates.end(), [&](auto const& other) {
                return group->algorithm == other.algorithm && group->signers == other.signers &&
                    group->stage == other.stage;
            });
        if (found == candidates.end())
            throw std::runtime_error("Missing matching comparison group");
        rows.emplace_back(group, &*found);
    }
    return rows;
}

std::vector<std::string>
renderComparison(
    Snapshot const& snapshot,
    State const& state,
    std::size_t width,
    std::size_t height)
{
    auto const& comparison = snapshot.comparison.value();
    auto const rows = comparisonGroups(snapshot, state);
    auto const [before, after] = rows.at(state.selected);
    std::vector<std::string> lines{
        "BALLAST / COMPARE: " + comparison.label,
        "CSV validation recorded at import; signature tests recorded, not rerun",
        "Rows B:" + std::to_string(snapshot.views[0].rows) +
            " O:" + std::to_string(snapshot.views[1].rows) +
            " | Tests B:" + std::to_string(comparison.provenance[0].recordedTests) +
            " O:" + std::to_string(comparison.provenance[1].recordedTests) + " recorded",
        std::string(width - 1, '-')};
    if (state.provenance)
    {
        auto const& source = comparison.provenance.at(state.view);
        lines.push_back("PROVENANCE: " + snapshot.views.at(state.view).id + " | r changes source");
        for (auto const& [label, value] : std::vector<std::pair<std::string, std::string>>{
                 {"Candidate base", source.candidate},
                 {"Crypto", source.crypto},
                 {"Measurement SHA-256", source.measurementHash},
                 {"Compiled manifest SHA-256", source.dependencyHash},
                 {"Bundle SHA-256", snapshot.sourceBundleHash},
                 {"Compiler / OpenSSL", source.compiler + " / " + source.openssl}})
        {
            lines.push_back(label + ":");
            lines.push_back("  " + value);
        }
        auto const variant = source.flags.find("stblob_move=");
        lines.push_back(
            "Build variant: " +
            (variant == std::string::npos
                 ? "unspecified"
                 : source.flags.substr(variant, source.flags.find(';', variant) - variant)));
        lines.push_back(
            "Full flags in imported JSON. Candidate commit identifies the base revision.");
    }
    else
    {
        static constexpr std::array names{
            "Peak live bytes",
            "Cumulative requested bytes",
            "Operation-end live bytes",
            "Instrumented time"};
        lines.push_back(
            std::string(kStages.at(state.stage)) + " | " + names.at(state.metric) + " | medians");
        lines.push_back("  Algorithm  N      Baseline    Optimized        Saved  Change");
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            auto const [left, right] = rows[i];
            auto const first = left->metrics.at(state.metric).median;
            auto const second = right->metrics.at(state.metric).median;
            auto const delta = difference(first, second);
            lines.push_back(
                std::string(i == state.selected ? "> " : "  ") + column(left->algorithm, 10) +
                column(std::to_string(left->signers), 3, true) + " " +
                column(formatValue(first, state.metric), 12, true) + " " +
                column(formatValue(second, state.metric), 12, true) + " " +
                column(formatValue(delta.saved, state.metric), 12, true) + " " + changeText(delta));
        }
        lines.push_back(std::string(width - 1, '-'));
        lines.push_back(
            "SELECTED: " + before->algorithm + " / " + std::to_string(before->signers) +
            " signers | samples B:" + std::to_string(before->samples) +
            " O:" + std::to_string(after->samples));
        for (auto const& [label, index] : std::array<std::pair<char const*, std::size_t>, 3>{
                 {{"Requests", 1}, {"Peak", 0}, {"Op-end", 2}}})
        {
            auto const first = before->metrics[index].median;
            auto const second = after->metrics[index].median;
            lines.push_back(
                std::string(label) + " " + formatValue(first, index) + " -> " +
                formatValue(second, index) + " | " + changeText(difference(first, second)));
        }
        if (before->allocationCalls && after->allocationCalls)
        {
            auto const first = before->allocationCalls->median;
            auto const second = after->allocationCalls->median;
            lines.push_back(
                "Calls " + counts(first) + " -> " + counts(second) + " | " +
                changeText(difference(first, second)));
        }
        lines.push_back(
            state.metric == 3
                ? "Instrumented timing includes tracking overhead; not production latency."
                : "Saved = baseline - optimized. Negative savings indicate a regression.");
        lines.push_back(
            "Samples stay separate. Import checks CSVs; viewer does not revalidate on open.");
    }
    while (lines.size() < height - 3)
        lines.emplace_back();
    lines.resize(std::min(lines.size(), height - 3));
    lines.push_back("Coverage: intercepted C++ requests | Wire excluded | NOT node RSS");
    lines.push_back(
        state.stage == 1
            ? "Verify-only: pre-parsed transaction excluded. Stack/C/OpenSSL untracked."
            : "Stack/C/OpenSSL untracked. No ledger authorization or fee validation.");
    lines.push_back("1/2/3 stage | m metric | j/k row | p provenance | r source | q quit");
    for (auto& line : lines)
        line = safeText(std::move(line)).substr(0, width - 1);
    return lines;
}

}  // namespace ballast::dashboard
