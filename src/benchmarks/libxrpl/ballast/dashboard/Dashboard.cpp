#include <Dashboard.h>

#include <boost/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace ballast::dashboard {
namespace {

void
require(bool value, char const* message)
{
    if (!value)
        throw std::runtime_error(message);
}

std::string
text(boost::json::object const& object, char const* key)
{
    return boost::json::value_to<std::string>(object.at(key));
}

Statistic
statistic(boost::json::value const& value)
{
    auto const& object = value.as_object();
    Statistic result{
        boost::json::value_to<double>(object.at("min")),
        boost::json::value_to<double>(object.at("median")),
        boost::json::value_to<double>(object.at("max"))};
    require(
        std::isfinite(result.minimum) && std::isfinite(result.median) &&
            std::isfinite(result.maximum) && result.minimum >= 0 &&
            result.minimum <= result.median && result.median <= result.maximum,
        "Invalid metric range");
    return result;
}

View
readView(std::string id, boost::json::object const& object)
{
    require(
        text(object, "coverage") == "global_cpp_new_delete_requested_bytes",
        "Unsupported allocation coverage");
    require(
        text(object, "input_storage") == "excluded" && text(object, "rss") == "not_measured",
        "Unsupported measurement boundaries");
    View view{
        std::move(id),
        boost::json::value_to<std::size_t>(object.at("validated_files")),
        boost::json::value_to<std::size_t>(object.at("rows")),
        {}};
    require(view.runs > 0, "No recorded runs");
    std::set<std::tuple<std::string, std::size_t, std::string>> identities;
    std::size_t samples = 0;
    for (auto const& value : object.at("groups").as_array())
    {
        auto const& entry = value.as_object();
        Group group{
            text(entry, "algorithm"),
            boost::json::value_to<std::size_t>(entry.at("signer_count")),
            text(entry, "stage"),
            boost::json::value_to<std::size_t>(entry.at("samples")),
            {},
            boost::json::value_to<std::size_t>(entry.at("allocation_metric_variants"))};
        require(
            group.algorithm == "secp256k1" || group.algorithm == "ML-DSA-44", "Unknown algorithm");
        require(
            group.signers == 1 || group.signers == 8 || group.signers == 32,
            "Unknown signer count");
        require(
            std::find(kStages.begin(), kStages.end(), group.stage) != kStages.end(),
            "Unknown stage");
        require(group.samples > 0 && group.variants > 0, "Empty sample group");
        require(
            identities.emplace(group.algorithm, group.signers, group.stage).second,
            "Duplicate sample group");
        for (std::size_t i = 0; i < kMetrics.size(); ++i)
            group.metrics[i] = statistic(entry.at(kMetrics[i]));
        require(
            group.metrics[2].maximum <= group.metrics[0].maximum &&
                group.metrics[0].maximum <= group.metrics[1].maximum,
            "Inconsistent memory metrics");
        samples += group.samples;
        view.groups.push_back(std::move(group));
    }
    require(view.groups.size() == 18 && samples == view.rows, "Incomplete fixture/stage matrix");
    return view;
}

std::string
field(std::string const& value, std::size_t width, bool right = false)
{
    auto clipped = safeText(value).substr(0, width);
    return right ? std::string(width - clipped.size(), ' ') + clipped
                 : clipped + std::string(width - clipped.size(), ' ');
}

}  // namespace

Snapshot
loadSnapshot(std::filesystem::path const& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "Cannot open dashboard data file");
    std::string document{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    auto const parsed = boost::json::parse(document);
    auto const& root = parsed.as_object();
    Snapshot result;
    if (root.contains("views"))
    {
        require(
            boost::json::value_to<int>(root.at("schema_version")) == 1,
            "Unsupported dashboard schema");
        std::set<std::string> ids;
        for (auto const& value : root.at("views").as_array())
        {
            auto const& view = value.as_object();
            auto id = text(view, "id");
            require(ids.insert(id).second, "Duplicate run view");
            result.views.push_back(readView(id, view.at("summary").as_object()));
        }
        auto const& metadata = root.at("metadata").as_object();
        result.candidate = text(metadata, "candidate_commit");
        result.crypto = text(metadata, "crypto_commit");
        result.measurementHash = text(metadata, "measurement_source_sha256");
        result.compiler = text(metadata, "compiler_id") + " " + text(metadata, "compiler_version");
        result.openssl = text(metadata, "openssl_version");
        result.sourceBundleHash = text(root, "source_bundle_sha256");
        result.recordedTests = boost::json::value_to<std::size_t>(root.at("recorded_tests_passed"));
    }
    else
        result.views.push_back(readView("all", root));  // Existing summary.json works directly.
    require(!result.views.empty(), "No dashboard views");
    return result;
}

bool
State::handle(char key, std::size_t viewCount)
{
    switch (key)
    {
        case 'q':
        case 3:
        case 4:
            return false;
        case '1':
        case '2':
        case '3':
            stage = static_cast<std::size_t>(key - '1');
            break;
        case 'm':
            metric = (metric + 1) % kMetrics.size();
            break;
        case 'r':
            view = (view + 1) % viewCount;
            break;
        case 'j':
            selected = (selected + 1) % 6;
            break;
        case 'k':
            selected = (selected + 5) % 6;
            break;
        case 'p':
            provenance = !provenance;
            break;
        default:
            break;
    }
    return true;
}

std::vector<Group const*>
visibleGroups(Snapshot const& snapshot, State const& state)
{
    std::vector<Group const*> groups;
    for (auto const& group : snapshot.views.at(state.view).groups)
        if (group.stage == kStages.at(state.stage))
            groups.push_back(&group);
    std::sort(groups.begin(), groups.end(), [](auto lhs, auto rhs) {
        return std::tuple(lhs->signers, lhs->algorithm == "ML-DSA-44") <
            std::tuple(rhs->signers, rhs->algorithm == "ML-DSA-44");
    });
    return groups;
}

std::string
safeText(std::string value)
{
    for (char& ch : value)
        if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126)
            ch = '?';
    return value;
}

std::string
formatValue(double value, std::size_t metric)
{
    std::ostringstream out;
    if (metric == 3)
        out << std::fixed << std::setprecision(2) << value / 1000.0 << " us";
    else
        out << std::fixed << std::setprecision(value == std::floor(value) ? 0 : 1) << value << " B";
    return out.str();
}

std::vector<std::string>
render(Snapshot const& snapshot, State const& state, std::size_t width, std::size_t height)
{
    if (width < 80 || height < 24)
    {
        std::vector<std::string> small{"BALLAST", "Need 80x24 terminal. q: quit"};
        small.resize(std::min(small.size(), height));
        for (auto& line : small)
            line = line.substr(0, width ? width - 1 : 0);
        return small;
    }
    auto const& view = snapshot.views.at(state.view);
    auto const groups = visibleGroups(snapshot, state);
    auto const& selected = *groups.at(state.selected);
    std::vector<std::string> lines{
        "BALLAST / POST-QUANTUM MEMORY BASELINE                       READ ONLY",
        "Recorded measurements | No builds, tests, or crypto checks run by this viewer",
        "View: " + view.id + " | " + std::to_string(view.rows) + " rows | " +
            std::to_string(view.runs) + " run(s) | Tests: " +
            (snapshot.recordedTests ? std::to_string(snapshot.recordedTests) + " recorded"
                                    : "not recorded"),
        std::string(width - 1, '-')};
    if (state.provenance)
    {
        lines.push_back("PROVENANCE / SAVED OBSERVATIONS");
        for (auto const& pair : std::vector<std::pair<std::string, std::string>>{
                 {"Candidate", snapshot.candidate},
                 {"Crypto", snapshot.crypto},
                 {"Measurement SHA-256", snapshot.measurementHash},
                 {"Bundle SHA-256", snapshot.sourceBundleHash},
                 {"Compiler", snapshot.compiler},
                 {"OpenSSL", snapshot.openssl}})
        {
            lines.push_back(pair.first + ":");
            lines.push_back(
                "  " + (pair.second.empty() ? "not available in plain summary" : pair.second));
        }
        lines.push_back("Ownership microbenchmark: separate experiment; see HANDOFF.md.");
        lines.push_back("Ledger authorization / fee eligibility / node RSS: not checked.");
    }
    else
    {
        static constexpr std::array metricNames{
            "Peak tracked bytes",
            "Cumulative requested bytes",
            "Live bytes at operation end",
            "Instrumented time"};
        lines.push_back(
            "[1] parse_only  [2] verify_only  [3] parse_verify  | " +
            std::string(kStages[state.stage]));
        lines.push_back(
            std::string(metricNames[state.metric]) +
            " | median and min..max | bars scale to largest median");
        lines.push_back("  Algorithm  N        Median                Min..max   Relative");
        lines.push_back(std::string(width - 1, '-'));
        double maximum = 0;
        for (auto const* group : groups)
            maximum = std::max(maximum, group->metrics[state.metric].median);
        auto const barWidth = std::min<std::size_t>(30, width - 59);
        for (std::size_t i = 0; i < groups.size(); ++i)
        {
            auto const& group = *groups[i];
            auto const& metric = group.metrics[state.metric];
            auto const bars = maximum == 0 || metric.median == 0
                ? 0
                : std::max<std::size_t>(
                      1, static_cast<std::size_t>(metric.median / maximum * barWidth));
            lines.push_back(
                std::string(i == state.selected ? "> " : "  ") + field(group.algorithm, 10) +
                field(std::to_string(group.signers), 3, true) + " " +
                field(formatValue(metric.median, state.metric), 13, true) + " " +
                field(
                    formatValue(metric.minimum, state.metric) + ".." +
                        formatValue(metric.maximum, state.metric),
                    25,
                    true) +
                "  " + std::string(bars, '#'));
        }
        lines.push_back(std::string(width - 1, '-'));
        lines.push_back(
            "SELECTED: " + selected.algorithm + " / " + std::to_string(selected.signers) +
            " signers / " + std::to_string(selected.samples) + " samples");
        lines.push_back(
            "Requests " + formatValue(selected.metrics[1].median, 1) + " | Peak " +
            formatValue(selected.metrics[0].median, 0) + " | Op-end " +
            formatValue(selected.metrics[2].median, 2));
        lines.push_back(
            "Allocation metric variants: " + std::to_string(selected.variants) +
            " | Source: saved summary (not revalidated against CSV on open)");
        lines.push_back(
            state.stage == 0 ? "Signature prevalidated outside parse scope; not checked inside it."
                             : "Native signature check: saved valid observation; not rerun here.");
        lines.push_back(
            state.metric == 3
                ? "Timing includes allocation-hook overhead; not production latency/TPS."
                : "Cumulative requests and peak simultaneous live bytes are different metrics.");
    }
    while (lines.size() < height - 3)
        lines.emplace_back();
    lines.resize(std::min(lines.size(), height - 3));
    lines.push_back("Coverage: intercepted C++ requests | Wire excluded | NOT node RSS");
    lines.push_back(
        state.stage == 1
            ? "Verify-only: pre-parsed transaction excluded. Stack/C/OpenSSL memory untracked."
            : "Stack/C/OpenSSL memory untracked. No ledger authorization or fee validation.");
    lines.push_back("1/2/3 stage | m metric | r run | j/k row | p provenance | q quit");
    for (auto& line : lines)
        line = safeText(std::move(line)).substr(0, width - 1);
    return lines;
}

}  // namespace ballast::dashboard
