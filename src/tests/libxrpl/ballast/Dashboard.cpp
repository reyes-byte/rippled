#include <Dashboard.h>

#include <boost/json.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace ballast::dashboard::tests {

Snapshot const&
snapshot()
{
    static Snapshot const data = loadSnapshot(BALLAST_DASHBOARD_DATA);
    return data;
}

bool
contains(std::vector<std::string> const& lines, std::string const& needle)
{
    for (auto const& line : lines)
        if (line.find(needle) != std::string::npos)
            return true;
    return false;
}

struct TemporaryJson
{
    std::filesystem::path path;
    TemporaryJson()
    {
        static unsigned int counter = 0;
        path = std::filesystem::temp_directory_path() /
            ("ballast-dashboard-" + std::to_string(getpid()) + "-" + std::to_string(counter++) +
             ".json");
    }
    ~TemporaryJson()
    {
        std::filesystem::remove(path);
    }
};

boost::json::value
source()
{
    std::ifstream input(BALLAST_DASHBOARD_DATA);
    return boost::json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
}

TEST(BallastDashboard, loads_all_runs_and_individual_run_views)
{
    auto const& data = snapshot();
    ASSERT_EQ(data.views.size(), 4);
    EXPECT_EQ(data.views[0].rows, 1620);
    EXPECT_EQ(data.views[0].runs, 3);
    EXPECT_EQ(data.views[1].rows, 540);
    EXPECT_EQ(data.views[1].runs, 1);
    EXPECT_EQ(data.recordedTests, 155);
}

TEST(BallastDashboard, selected_ml_dsa_values_match_recorded_native_measurements)
{
    State state;
    auto groups = visibleGroups(snapshot(), state);
    ASSERT_EQ(groups.size(), 6);
    EXPECT_EQ(groups[0]->algorithm, "secp256k1");
    EXPECT_EQ(groups[1]->algorithm, "ML-DSA-44");
    EXPECT_EQ(groups[5]->signers, 32);
    EXPECT_EQ(groups[5]->metrics[0].median, 312232);
    EXPECT_EQ(groups[5]->metrics[1].median, 877665);
    EXPECT_EQ(groups[5]->metrics[2].median, 131120);
    EXPECT_EQ(groups[5]->samples, 90);
    state.handle('2', snapshot().views.size());
    groups = visibleGroups(snapshot(), state);
    EXPECT_EQ(groups[5]->metrics[0].median, 4138);
    EXPECT_EQ(groups[5]->metrics[2].median, 0);
}

TEST(BallastDashboard, run_selection_uses_run_samples_without_changing_saved_model)
{
    State state;
    state.handle('r', snapshot().views.size());
    auto const groups = visibleGroups(snapshot(), state);
    EXPECT_EQ(groups[5]->samples, 30);
    EXPECT_EQ(groups[5]->metrics[0].median, 312232);
    EXPECT_EQ(snapshot().views[0].groups[0].samples, 90);
    for (int i = 0; i < 3; ++i)
        state.handle('r', snapshot().views.size());
    EXPECT_EQ(state.view, 0);
}

TEST(BallastDashboard, keyboard_controls_wrap_and_quit)
{
    State state;
    state.handle('j', 4);
    EXPECT_EQ(state.selected, 0);
    state.handle('k', 4);
    EXPECT_EQ(state.selected, 5);
    for (int i = 0; i < 4; ++i)
        state.handle('m', 4);
    EXPECT_EQ(state.metric, 0);
    state.handle('3', 4);
    EXPECT_EQ(state.stage, 2);
    state.handle('p', 4);
    EXPECT_TRUE(state.provenance);
    EXPECT_FALSE(state.handle('q', 4));
    EXPECT_FALSE(state.handle(3, 4));
}

TEST(BallastDashboard, render_preserves_coverage_and_stage_labels_in_80_by_24)
{
    State state;
    state.handle('2', 4);
    auto const lines = render(snapshot(), state, 80, 24);
    EXPECT_TRUE(contains(lines, "pre-parsed transaction excluded"));
    EXPECT_TRUE(contains(lines, "NOT node RSS"));
    EXPECT_TRUE(contains(lines, "not rerun here"));
    EXPECT_LE(lines.size(), 24);
    for (auto const& line : lines)
        EXPECT_LE(line.size(), 79);
}

TEST(BallastDashboard, zero_retained_verification_values_do_not_divide_by_zero)
{
    State state;
    state.stage = 1;
    state.metric = 2;
    auto const lines = render(snapshot(), state);
    EXPECT_TRUE(contains(lines, "0 B"));
    EXPECT_FALSE(contains(lines, "#"));  // All-zero medians produce empty bars.
    for (auto const& line : lines)
        if (line.find("secp256k1") != std::string::npos ||
            line.find("ML-DSA-44") != std::string::npos)
        {
            EXPECT_EQ(line.find("nan"), std::string::npos);
            EXPECT_EQ(line.find("inf"), std::string::npos);
        }
}

TEST(BallastDashboard, small_terminal_has_a_safe_fallback)
{
    auto const lines = render(snapshot(), State{}, 35, 10);
    EXPECT_TRUE(contains(lines, "80x24"));
    for (auto const& line : lines)
        EXPECT_LE(line.size(), 34);
    EXPECT_TRUE(render(snapshot(), State{}, 0, 0).empty());
}

TEST(BallastDashboard, formatters_show_units_and_strip_terminal_controls)
{
    EXPECT_EQ(formatValue(30380, 3), "30.38 us");
    EXPECT_EQ(formatValue(131120, 0), "131120 B");
    EXPECT_EQ(safeText("\033[31mhello\n"), "?[31mhello?");
}

TEST(BallastDashboard, provenance_view_distinguishes_recorded_tests)
{
    State state;
    state.provenance = true;
    auto const lines = render(snapshot(), state);
    EXPECT_TRUE(contains(lines, "155 recorded"));
    EXPECT_TRUE(contains(lines, "779859c8afa324d34d16e42028af8d2ceb5fcc8e"));
    EXPECT_TRUE(contains(lines, "PROVENANCE"));
}

TEST(BallastDashboard, plain_measurement_summary_is_accepted_without_invented_provenance)
{
    TemporaryJson temporary;
    auto const data = source();
    std::ofstream(temporary.path) << data.at("views").as_array()[0].at("summary");
    auto const loaded = loadSnapshot(temporary.path);
    EXPECT_EQ(loaded.views.size(), 1);
    EXPECT_EQ(loaded.recordedTests, 0);
    EXPECT_TRUE(loaded.candidate.empty());
}

TEST(BallastDashboard, malformed_or_incomplete_input_is_rejected)
{
    TemporaryJson temporary;
    std::ofstream(temporary.path) << "{broken";
    EXPECT_THROW(loadSnapshot(temporary.path), std::exception);
    auto data = source();
    data.at("views").as_array()[0].at("summary").as_object()["coverage"] = "node_rss";
    std::ofstream(temporary.path) << data;
    EXPECT_THROW(loadSnapshot(temporary.path), std::exception);
    data = source();
    data.at("views").as_array()[0].at("summary").at("groups").as_array().pop_back();
    std::ofstream(temporary.path) << data;
    EXPECT_THROW(loadSnapshot(temporary.path), std::exception);
}

}  // namespace ballast::dashboard::tests
