#include <benchmarks/libxrpl/pqc/Crypto.h>
#include <benchmarks/libxrpl/pqc/Layout.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
    unsigned int signers = 1;
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
                         "[--message-bytes N] [--signers N] [--output results.csv]\n";
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
        else if (option == "--signers")
            config.signers = number(argv[i]);
        else
            throw std::runtime_error("unknown option: " + std::string(option));
    }
    if (!config.samples || !config.parseIterations || !config.verifyIterations)
        throw std::runtime_error("samples and iteration counts must be positive");
    if (!config.signers || config.signers > kMaxSigners)
        throw std::runtime_error("signer count must be between 1 and 32");
    if (config.samples > 1000 || config.parseIterations > 100000000 ||
        config.verifyIterations > 1000000 || config.messageBytes > 16777216 / config.signers)
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

enum class Policy { Owned, View, SharedView };

constexpr std::array kPolicies{Policy::Owned, Policy::View, Policy::SharedView};
constexpr std::array<std::string_view, 3> kPolicyNames{"owned", "view", "shared_view"};

Measurement
measure(
    SignerWorkload const& workload,
    std::string const& algorithm,
    Policy policy,
    bool checkSignature,
    unsigned int iterations)
{
    std::uint64_t checksum = 0;
    // Wrapper containers are allocated once outside timing, equally for all policies.
    std::vector<OwnedFields> owners(workload.signers.size());
    std::vector<SharedFields> shared(workload.signers.size());
    std::vector<Fields> fields(workload.signers.size());
    auto const start = std::chrono::steady_clock::now();
    for (unsigned int i = 0; i < iterations; ++i)
    {
        // Parse the whole group first, keeping every field alive through verification.
        for (std::size_t signer = 0; signer < workload.signers.size(); ++signer)
        {
            auto const& record = workload.signers[signer];
            auto const input = View(*workload.wire).subspan(record.offset, record.encodedBytes);
            if (policy == Policy::Owned)
            {
                owners[signer] = parseOwned(input);
                fields[signer] = owners[signer].views();
            }
            else if (policy == Policy::SharedView)
            {
                shared[signer] = parseShared(workload.wire, record.offset, record.encodedBytes);
                fields[signer] = shared[signer].views();
            }
            else
                fields[signer] = parseViews(input);
            checksum += observe(fields[signer]);
        }
        if (checkSignature)
        {
            for (std::size_t signer = 0; signer < workload.signers.size(); ++signer)
            {
                if (!verify(algorithm, workload.signers[signer].message, fields[signer]))
                    throw std::runtime_error("valid fixture failed verification");
                ++checksum;
            }
        }
        // Field releases are timed too; no payload storage survives into the next operation.
        for (auto& owner : owners)
            owner = OwnedFields{};
        for (auto& owner : shared)
            owner = SharedFields{};
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
              "parser_copied_bytes_per_op,extra_owned_payload_bytes_per_workload,"
              "ns_per_op,operations_per_second,checksum,fixture_sha256,openssl,compiler,"
              "source_commit,implementation_sha256,fixture_kind,signer_count\n";
    for (std::string const algorithm : {"ED25519", "ML-DSA-44"})
    {
        auto const workload =
            makeSignerWorkload(algorithm, config.messageBytes, config.seed, config.signers);
        auto const& first = workload.signers.front();
        auto const fields =
            parseViews(View(*workload.wire).subspan(first.offset, first.encodedBytes));
        ParseStats stats;
        std::string fingerprint;
        std::size_t inputBytes = workload.wire->size();
        {
            Bytes fingerprintInput;
            std::vector<OwnedFields> owners;
            owners.reserve(workload.signers.size());
            for (auto const& record : workload.signers)
            {
                auto const input = View(*workload.wire).subspan(record.offset, record.encodedBytes);
                auto const view = parseViews(input);
                owners.push_back(parseOwned(input, &stats));
                auto const owned = owners.back().views();
                auto const shared = parseShared(workload.wire, record.offset, record.encodedBytes);
                for (auto const candidate : {owned, shared.views()})
                    if (!std::equal(
                            view.publicKey.begin(),
                            view.publicKey.end(),
                            candidate.publicKey.begin(),
                            candidate.publicKey.end()) ||
                        !std::equal(
                            view.signature.begin(),
                            view.signature.end(),
                            candidate.signature.begin(),
                            candidate.signature.end()))
                        throw std::runtime_error("parsers disagree on fixture bytes");
                fingerprintInput.insert(fingerprintInput.end(), input.begin(), input.end());
                fingerprintInput.insert(
                    fingerprintInput.end(), record.message.begin(), record.message.end());
                inputBytes += record.message.size();
            }
            fingerprint = sha256(fingerprintInput);
        }
        std::cerr << algorithm << ": " << config.signers << " signers; key "
                  << fields.publicKey.size() << " B, signature " << fields.signature.size()
                  << " B; fixture " << fingerprint << '\n';

        // Warm every code path before timing. Fixture creation is never timed.
        for (auto policy : kPolicies)
            (void)measure(workload, algorithm, policy, true, 10);
        for (bool checkSignature : {false, true})
        {
            unsigned int const iterations =
                checkSignature ? config.verifyIterations : config.parseIterations;
            for (unsigned int sample = 0; sample < config.samples; ++sample)
            {
                // Rotate order to reduce systematic position bias.
                for (std::size_t position = 0; position < kPolicies.size(); ++position)
                {
                    auto const index = (sample + position) % kPolicies.size();
                    auto const policy = kPolicies[index];
                    bool const owning = policy == Policy::Owned;
                    auto const result =
                        measure(workload, algorithm, policy, checkSignature, iterations);
                    output << algorithm << ',' << kPolicyNames[index] << ','
                           << (checkSignature ? "parse_verify" : "parse") << ',' << sample << ','
                           << config.seed << ',' << iterations << ',' << config.messageBytes << ','
                           << fields.publicKey.size() << ',' << fields.signature.size() << ','
                           << inputBytes << ',' << (owning ? stats.payloadAllocations : 0) << ','
                           << (owning ? stats.copiedBytes : 0) << ','
                           << (owning ? stats.copiedBytes : 0) << ',' << std::fixed
                           << std::setprecision(3) << result.nanosecondsPerOperation << ','
                           << 1e9 / result.nanosecondsPerOperation << ',' << result.checksum << ','
                           << fingerprint << ',' << csv(OpenSSL_version(OPENSSL_VERSION)) << ','
                           << csv(__VERSION__) << ',' << PQC_SOURCE_COMMIT << ','
                           << PQC_IMPLEMENTATION_SHA256 << ",synthetic_public_test_key,"
                           << config.signers << '\n';
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
