#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/hash/uhash.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Rules.h>

#include <benchmarks/libxrpl/ballast/retention/Retention.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

std::size_t
positive(std::string_view value, std::size_t maximum)
{
    std::size_t number = 0;
    auto const [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end != value.data() + value.size() || number == 0 ||
        number > maximum)
        throw std::invalid_argument("Invalid positive count");
    return number;
}

void
validate(ballast::retention::Sample const& sample, std::size_t count)
{
    if (sample.retainedObjects != count || sample.consumedInputs != count ||
        sample.validSignatures != count || sample.matchingTransactionIds != count ||
        sample.completed.liveBytes != 0 || sample.completed.liveBlocks != 0 ||
        sample.completed.allocations != sample.completed.frees ||
        sample.completed.requestedBytes != sample.completed.freedBytes ||
        sample.completed.nonOwnedFrees != 0)
        throw std::runtime_error("Retention workload failed correctness/cleanup checks");
}

}  // namespace

int
main(int argc, char** argv)
{
    try
    {
        std::size_t samples = 1, warmups = 1;
        std::string runId = "local", metadataPath;
        std::vector<std::size_t> counts{1, 10, 100, 1000};
        for (int i = 1; i < argc; ++i)
        {
            std::string_view const option = argv[i];
            if (option == "--help")
            {
                std::cout << "Usage: xrpl.ballast_retention [--counts 1,10,100,1000] [--samples N] "
                             "[--warmup N] [--run-id TEXT] [--metadata FILE]\n"
                             "CSV to stdout. Repeated fixtures, not unique transactions or a node "
                             "queue.\n";
                return 0;
            }
            if (i + 1 == argc)
                throw std::invalid_argument("Missing option value");
            std::string_view value = argv[++i];
            if (option == "--samples")
                samples = positive(value, 1000);
            else if (option == "--warmup")
                warmups = positive(value, 1000);
            else if (option == "--run-id")
                runId = value;
            else if (option == "--metadata")
                metadataPath = value;
            else if (option == "--counts")
            {
                counts.clear();
                while (true)
                {
                    auto const comma = value.find(',');
                    auto const number =
                        positive(value.substr(0, comma), ballast::retention::kMaximumCount);
                    if (std::find(counts.begin(), counts.end(), number) != counts.end())
                        throw std::invalid_argument("Duplicate retained count");
                    counts.push_back(number);
                    if (comma == std::string_view::npos)
                        break;
                    value.remove_prefix(comma + 1);
                }
            }
            else
                throw std::invalid_argument("Unknown option");
        }
        std::unordered_set<xrpl::uint256, beast::Uhash<>> const presets{xrpl::featureQuantum};
        xrpl::Rules const rules{presets};
        std::vector<ballast::measurement::Fixture> corpus;
        for (auto slug : ballast::measurement::kFixtureSlugs)
            corpus.push_back(ballast::measurement::loadFixture(slug, rules));
        ballast::retention::writeCsvHeader(std::cout);
        for (auto const& fixture : corpus)
            for (auto count : counts)
            {
                for (std::size_t i = 0; i < warmups; ++i)
                    validate(ballast::retention::measure(fixture, rules, count), count);
                for (std::size_t i = 0; i < samples; ++i)
                {
                    auto const sample = ballast::retention::measure(fixture, rules, count);
                    validate(sample, count);
                    ballast::retention::writeCsvRow(
                        std::cout, fixture, sample, count, i, warmups, runId);
                }
            }
        if (!std::cout)
            throw std::runtime_error("CSV output failed");
        if (!metadataPath.empty())
        {
            std::ofstream metadata(metadataPath);
            metadata << ballast::retention::metadataJson(runId, samples, warmups);
            if (!metadata)
                throw std::runtime_error("Metadata output failed");
        }
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "Retention benchmark: " << error.what() << '\n';
        return 1;
    }
}
