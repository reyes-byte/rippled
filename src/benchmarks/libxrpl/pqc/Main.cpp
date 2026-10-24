#include <benchmarks/libxrpl/pqc/Crypto.h>
#include <benchmarks/libxrpl/pqc/Layout.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace pqc_bench;

struct Config
{
    std::uint32_t seed = 42;
    unsigned int samples = 5;
    unsigned int parseIterations = 100000;
    unsigned int verifyIterations = 1000;
    unsigned int messageBytes = 1024;
    std::string output;
};

unsigned int
number(std::string_view text)
{
    unsigned int result = 0;
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("expected a nonnegative integer");
    return result;
}

Config
arguments(int argc, char** argv)
{
    Config config;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view const option = argv[i];
        if (option == "--help")
        {
            std::cout << "Usage: pqc_bench [--seed N] [--samples N] "
                         "[--parse-iterations N] [--verify-iterations N] "
                         "[--message-bytes N] [--output results.csv]\n";
            std::exit(0);
        }
        if (++i == argc)
            throw std::runtime_error("missing option value");
        if (option == "--output")
            config.output = argv[i];
        else if (option == "--seed")
            config.seed = number(argv[i]);
        else if (option == "--samples")
            config.samples = number(argv[i]);
        else if (option == "--parse-iterations")
            config.parseIterations = number(argv[i]);
        else if (option == "--verify-iterations")
            config.verifyIterations = number(argv[i]);
        else if (option == "--message-bytes")
            config.messageBytes = number(argv[i]);
        else
            throw std::runtime_error("unknown option: " + std::string(option));
    }
    if (!config.samples || !config.parseIterations || !config.verifyIterations)
        throw std::runtime_error("samples and iteration counts must be positive");
    if (config.samples > 1000 || config.parseIterations > 100000000 ||
        config.verifyIterations > 1000000 || config.messageBytes > 16777216)
        throw std::runtime_error("configuration exceeds this small bench's workload limits");
    return config;
}

// CSV quoting: double embedded quotes instead of using C++ backslash escapes.
std::string
csv(std::string const& value)
{
    std::string result = "\"";
    for (char c : value)
    {
        if (c == '"')
            result += '"';
        result += c;
    }
    return result + '"';
}

struct Measurement
{
    double nanosecondsPerOperation;
    std::uint64_t checksum;
};

Measurement
measure(
    Fixture const& fixture,
    std::string const& algorithm,
    bool owning,
    bool checkSignature,
    unsigned int iterations)
{
    std::uint64_t checksum = 0;
    auto const start = std::chrono::steady_clock::now();
    for (unsigned int i = 0; i < iterations; ++i)
    {
        // Owners stay alive until verification finishes, just like the input
        // buffer backing borrowed views. Both paths release per-record state.
        OwnedFields owner;
        Fields fields;
        if (owning)
        {
            owner = parseOwned(fixture.encoded);
            fields = owner.views();
        }
        else
            fields = parseViews(fixture.encoded);
        checksum += observe(fields);
        if (checkSignature)
        {
            if (!verify(algorithm, fixture.message, fields))
                throw std::runtime_error("valid fixture failed verification");
            ++checksum;
        }
    }
    auto const stop = std::chrono::steady_clock::now();
    double const elapsed = std::chrono::duration<double, std::nano>(stop - start).count();
    return {elapsed / iterations, checksum};
}

void
run(Config const& config, std::ostream& output)
{
    output << "algorithm,policy,stage,sample,seed,iterations,message_bytes,public_key_bytes,"
              "signature_bytes,input_bytes,parser_payload_allocations_per_op,"
              "parser_copied_bytes_per_op,extra_owned_payload_bytes_per_record,"
              "ns_per_op,operations_per_second,checksum,fixture_sha256,openssl,compiler,"
              "source_commit,implementation_sha256,fixture_kind\n";
    for (std::string const algorithm : {"ED25519", "ML-DSA-44"})
    {
        auto const fixture = makeFixture(algorithm, config.messageBytes, config.seed);
        auto const fields = parseViews(fixture.encoded);
        ParseStats stats;
        {
            auto const owner = parseOwned(fixture.encoded, &stats);
            if (!std::equal(
                    fields.publicKey.begin(),
                    fields.publicKey.end(),
                    owner.views().publicKey.begin()) ||
                !std::equal(
                    fields.signature.begin(),
                    fields.signature.end(),
                    owner.views().signature.begin()))
                throw std::runtime_error("parsers disagree on fixture bytes");
        }
        Bytes fingerprintInput = fixture.encoded;
        fingerprintInput.insert(
            fingerprintInput.end(), fixture.message.begin(), fixture.message.end());
        auto const fingerprint = sha256(fingerprintInput);
        std::cerr << algorithm << ": key " << fields.publicKey.size() << " B, signature "
                  << fields.signature.size() << " B; fixture " << fingerprint << '\n';

        // Warm both code paths before timing. Fixture creation is never timed.
        for (bool owning : {true, false})
            (void)measure(fixture, algorithm, owning, true, 10);
        for (bool checkSignature : {false, true})
        {
            unsigned int const iterations =
                checkSignature ? config.verifyIterations : config.parseIterations;
            for (unsigned int sample = 0; sample < config.samples; ++sample)
            {
                // Alternate order to reduce systematic first/second bias.
                for (unsigned int position = 0; position < 2; ++position)
                {
                    bool const owning = (sample + position) % 2 == 0;
                    auto const result =
                        measure(fixture, algorithm, owning, checkSignature, iterations);
                    output << algorithm << ',' << (owning ? "owned" : "view") << ','
                           << (checkSignature ? "parse_verify" : "parse") << ',' << sample << ','
                           << config.seed << ',' << iterations << ',' << config.messageBytes << ','
                           << fields.publicKey.size() << ',' << fields.signature.size() << ','
                           << fixture.encoded.size() + fixture.message.size() << ','
                           << (owning ? stats.payloadAllocations : 0) << ','
                           << (owning ? stats.copiedBytes : 0) << ','
                           << (owning ? stats.copiedBytes : 0) << ',' << std::fixed
                           << std::setprecision(3) << result.nanosecondsPerOperation << ','
                           << 1e9 / result.nanosecondsPerOperation << ',' << result.checksum << ','
                           << fingerprint << ',' << csv(OpenSSL_version(OPENSSL_VERSION)) << ','
                           << csv(__VERSION__) << ',' << PQC_SOURCE_COMMIT << ','
                           << PQC_IMPLEMENTATION_SHA256 << ",synthetic_public_test_key\n";
                }
            }
        }
    }
    if (!output)
        throw std::runtime_error("could not write CSV output");
}

}  // namespace

int
main(int argc, char** argv)
{
    try
    {
        auto const config = arguments(argc, argv);
        if (config.output.empty())
            run(config, std::cout);
        else
        {
            std::ofstream output(config.output);
            if (!output)
                throw std::runtime_error("could not open output file");
            run(config, output);
        }
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "pqc_bench: " << error.what() << '\n';
        return 1;
    }
}
