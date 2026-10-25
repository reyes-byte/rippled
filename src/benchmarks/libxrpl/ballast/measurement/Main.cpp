#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/hash/uhash.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Rules.h>

#include <benchmarks/libxrpl/ballast/measurement/Measurement.h>

#include <charconv>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

std::size_t
positiveCount(std::string_view text)
{
    std::size_t value = 0;
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 || value > 1000000)
        throw std::invalid_argument("Counts must be integers from 1 through 1000000");
    return value;
}

void
validateSample(ballast::measurement::Sample const& sample, ballast::measurement::Stage stage)
{
    if (!sample.inputConsumed ||
        (stage != ballast::measurement::Stage::Parse && sample.signatureValid != true))
        throw std::runtime_error("Measured operation changed a validated fixture outcome");
}

}  // namespace

int
main(int argc, char** argv)
{
    try
    {
        std::size_t samples = 30, warmups = 3;
        std::string runId = "local", metadataPath;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view option = argv[i];
            if (option == "--help")
            {
                std::cout
                    << "Usage: xrpl.ballast_measure [--samples N] [--warmup N] [--run-id TEXT] "
                       "[--metadata FILE]\nCSV goes to stdout; samples=30, warmup=3.\n";
                return 0;
            }
            if (i + 1 == argc)
                throw std::invalid_argument("Missing option value");
            std::string_view value = argv[++i];
            if (option == "--samples")
                samples = positiveCount(value);
            else if (option == "--warmup")
                warmups = positiveCount(value);
            else if (option == "--run-id")
                runId = value;
            else if (option == "--metadata")
                metadataPath = value;
            else
                throw std::invalid_argument("Unknown option: " + std::string(option));
        }
        std::unordered_set<xrpl::uint256, beast::Uhash<>> const presets{xrpl::featureQuantum};
        xrpl::Rules const rules{presets};
        std::vector<ballast::measurement::Fixture> corpus;
        corpus.reserve(ballast::measurement::kFixtureSlugs.size());
        for (auto slug : ballast::measurement::kFixtureSlugs)
            corpus.push_back(ballast::measurement::loadFixture(slug, rules));
        // Only retained wire/identities remain; no generating transaction or key objects.
        ballast::measurement::writeCsvHeader(std::cout);
        for (auto const& fixture : corpus)
            for (auto stage : ballast::measurement::kStages)
            {
                for (std::size_t i = 0; i < warmups; ++i)
                    validateSample(
                        ballast::measurement::measure(stage, fixture.wire, rules), stage);
                for (std::size_t i = 0; i < samples; ++i)
                {
                    auto const sample = ballast::measurement::measure(stage, fixture.wire, rules);
                    validateSample(sample, stage);
                    ballast::measurement::writeCsvRow(
                        std::cout, fixture, stage, sample, runId, i, warmups);
                }
            }
        if (!std::cout)
            throw std::runtime_error("CSV output failed");
        if (!metadataPath.empty())
        {
            std::ofstream metadata(metadataPath);
            metadata << ballast::measurement::metadataJson(runId, samples, warmups);
            if (!metadata)
                throw std::runtime_error("Metadata output failed");
        }
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "Ballast measurement failed: " << error.what() << '\n';
        return 1;
    }
}
