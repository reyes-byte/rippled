#include <benchmarks/libxrpl/ballast/measurement/Measurement.h>

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_value.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>

#include <openssl/evp.h>

#include <MeasurementBuild.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace ballast::measurement {
namespace {

void
require(bool value, char const* message)
{
    if (!value)
        throw std::runtime_error(message);
}

std::string
readFile(std::string const& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read " + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string
sha256(xrpl::Slice bytes)
{
    std::array<unsigned char, 32> digest{};
    unsigned int size = 0;
    require(
        EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) == 1,
        "SHA-256 failed");
    require(size == digest.size(), "Unexpected SHA-256 length");
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest)
    {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

// Always quote text, including run IDs or build flags containing commas/quotes.
void
textField(std::ostream& out, std::string_view value)
{
    out << '"';
    for (char ch : value)
    {
        if (ch == '"')
            out << '"';
        out << ch;
    }
    out << '"' << ',';
}

}  // namespace

Fixture
loadFixture(std::string_view slug, xrpl::Rules const& rules)
{
    auto const fingerprint =
        std::find_if(build::fixtures.begin(), build::fixtures.end(), [&](auto const& entry) {
            return entry.slug == slug;
        });
    require(fingerprint != build::fixtures.end(), "Unknown fixture slug");
    auto const base = std::string(build::fixtureDirectory) + "/" + std::string(slug);
    auto const metadataText = readFile(base + ".json");
    auto const hexText = readFile(base + ".hex");
    require(
        sha256(xrpl::makeSlice(metadataText)) == fingerprint->metadataSha256,
        "Fixture metadata differs from configured build; reconfigure/rebuild");
    require(
        sha256(xrpl::makeSlice(hexText)) == fingerprint->hexSha256,
        "Fixture hex differs from configured build; reconfigure/rebuild");
    ::json::Reader reader;
    ::json::Value metadata;
    require(reader.parse(metadataText, metadata), "Invalid fixture metadata JSON");
    require(
        metadata["candidate_commit"].asString() == build::candidateCommit,
        "Fixture candidate revision mismatch");
    require(
        metadata["reference_commit"].asString() == build::cryptoCommit,
        "Fixture crypto revision mismatch");
    std::string_view hex = hexText;
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r'))
        hex.remove_suffix(1);
    auto decoded = xrpl::strUnHex(hex);
    require(decoded.has_value(), "Invalid fixture hex");
    Fixture fixture{
        metadata["fixture_id"].asString(),
        metadata["algorithm"].asString(),
        metadata["signer_count"].asUInt(),
        std::move(*decoded),
        metadata["wire_sha256"].asString(),
        metadata["transaction_id"].asString()};
    require(fixture.wire.size() == metadata["wire_bytes"].asUInt(), "Fixture wire size mismatch");
    require(
        sha256(xrpl::makeSlice(fixture.wire)) == fixture.wireSha256, "Fixture wire hash mismatch");
    xrpl::SerialIter iterator{xrpl::makeSlice(fixture.wire)};
    xrpl::STTx const parsed{iterator};
    require(iterator.empty(), "Fixture has unread wire bytes");
    require(parsed.getSerializer().getData() == fixture.wire, "Fixture round-trip mismatch");
    require(
        xrpl::to_string(parsed.getTransactionID()) == fixture.transactionId, "Fixture ID mismatch");
    require(
        parsed.getSigningPubKey().empty() && !parsed.isFieldPresent(xrpl::sfTxnSignature),
        "Fixture is not multisigned");
    require(
        parsed.getFieldArray(xrpl::sfSigners).size() == fixture.signerCount,
        "Signer count mismatch");
    require(parsed.checkSign(rules).has_value(), "Fixture signature prevalidation failed");
    return fixture;  // parsed, JSON, streams and hex storage are destroyed here.
}

Sample
measure(Stage stage, xrpl::Blob const& wire, xrpl::Rules const& rules)
{
    Sample sample;
    // Optional stores STTx inline; no extra heap allocation for a wrapper.
    std::optional<xrpl::STTx> parsed;
    if (stage == Stage::Verify)
    {
        xrpl::SerialIter iterator{xrpl::makeSlice(wire)};
        parsed.emplace(iterator);
        sample.inputConsumed = iterator.empty();
    }
    {
        allocation::Scope scope;
        auto const start = std::chrono::steady_clock::now();
        if (stage != Stage::Verify)
        {
            xrpl::SerialIter iterator{xrpl::makeSlice(wire)};
            parsed.emplace(iterator);
            sample.inputConsumed = iterator.empty();
        }
        if (stage != Stage::Parse)
        {
            auto const result = parsed->checkSign(rules);
            sample.signatureValid = result.has_value();
        }  // Any returned error string is destroyed while counting.
        auto const stop = std::chrono::steady_clock::now();
        sample.elapsedNanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
        sample.operation = scope.snapshot();
        if (stage != Stage::Verify)
            parsed.reset();  // Cleanup is counted, but excluded from elapsed_ns.
        sample.completed = scope.finish();
    }
    // Verify-only's preexisting parsed transaction is destroyed outside scope.
    return sample;
}

std::string_view
stageName(Stage stage)
{
    switch (stage)
    {
        case Stage::Parse:
            return "parse_only";
        case Stage::Verify:
            return "verify_only";
        case Stage::Combined:
            return "parse_verify";
    }
    throw std::invalid_argument("Unknown measurement stage");
}

void
writeCsvHeader(std::ostream& out)
{
    out << "schema_version,run_id,fixture_id,algorithm,signing_mode,signer_count,stage,sample_"
           "index,"
           "warmup_iterations,wire_bytes,wire_capacity_bytes,wire_sha256,transaction_id,candidate_"
           "commit,"
           "crypto_commit,measurement_source_sha256,dependency_manifest_sha256,compiler_id,"
           "compiler_version,"
           "build_type,build_flags,openssl_version,rules,coverage,allocation_thread_policy,input_"
           "storage,"
           "preparsed_storage,parsed_destruction,initialization,signature_result,elapsed_ns,"
           "elapsed_scope,"
           "allocation_calls,requested_bytes,free_calls,freed_bytes,peak_live_bytes,live_bytes_at_"
           "operation_end,"
           "live_bytes_at_scope_end,live_blocks_at_scope_end,cleanup_allocation_calls,cleanup_"
           "requested_bytes,"
           "non_owned_free_calls\n";
}

void
writeCsvRow(
    std::ostream& out,
    Fixture const& fixture,
    Stage stage,
    Sample const& sample,
    std::string_view runId,
    std::size_t index,
    std::size_t warmups)
{
    out << "1,";
    textField(out, runId);
    textField(out, fixture.id);
    textField(out, fixture.algorithm);
    textField(out, "multi");
    out << fixture.signerCount << ',';
    textField(out, stageName(stage));
    out << index << ',' << warmups << ',' << fixture.wire.size() << ',' << fixture.wire.capacity()
        << ',';
    for (std::string_view value :
         {std::string_view(fixture.wireSha256),
          std::string_view(fixture.transactionId),
          build::candidateCommit,
          build::cryptoCommit,
          build::sourceHash,
          build::dependencyManifestHash,
          build::compiler,
          build::compilerVersion,
          build::buildType,
          build::flags,
          std::string_view(OpenSSL_version(OPENSSL_VERSION)),
          std::string_view("featureQuantum_only"),
          std::string_view("global_cpp_new_delete_requested_bytes"),
          std::string_view("allocating_thread_innermost_scope"),
          std::string_view("excluded")})
        textField(out, value);
    textField(out, stage == Stage::Verify ? "excluded" : "not_preparsed");
    textField(out, stage == Stage::Verify ? "after_scope" : "inside_scope_after_operation");
    textField(out, "prevalidated_then_stage_warmed");
    textField(
        out,
        !sample.signatureValid ? "not_checked_in_scope"
                               : (*sample.signatureValid ? "valid" : "invalid"));
    out << sample.elapsedNanoseconds << ',';
    textField(out, "instrumented_operation_excludes_parsed_cleanup");
    auto const& s = sample.completed;
    out << s.allocations << ',' << s.requestedBytes << ',' << s.frees << ',' << s.freedBytes << ','
        << s.peakLiveBytes << ',' << sample.operation.liveBytes << ',' << s.liveBytes << ','
        << s.liveBlocks << ',' << s.allocations - sample.operation.allocations << ','
        << s.requestedBytes - sample.operation.requestedBytes << ',' << s.nonOwnedFrees << '\n';
}

std::string
metadataJson(std::string_view runId, std::size_t samples, std::size_t warmups)
{
    ::json::Value metadata;
    metadata["schema_version"] = 1;
    metadata["run_id"] = std::string(runId);
    metadata["samples_per_fixture_stage"] = static_cast<unsigned int>(samples);
    metadata["warmup_iterations_per_fixture_stage"] = static_cast<unsigned int>(warmups);
    metadata["candidate_commit"] = std::string(build::candidateCommit);
    metadata["crypto_commit"] = std::string(build::cryptoCommit);
    metadata["measurement_source_sha256"] = std::string(build::sourceHash);
    metadata["dependency_manifest_sha256"] = std::string(build::dependencyManifestHash);
    metadata["compiler_id"] = std::string(build::compiler);
    metadata["compiler_version"] = std::string(build::compilerVersion);
    metadata["build_type"] = std::string(build::buildType);
    metadata["build_flags"] = std::string(build::flags);
    metadata["openssl_version"] = OpenSSL_version(OPENSSL_VERSION);
    metadata["build_scope"] = "candidate_protocol_source_subset";
    metadata["input_storage"] = "excluded";
    metadata["coverage"] = "global_cpp_new_delete_requested_bytes";
    metadata["tracker_metadata"] = "excluded";
    metadata["rss"] = "not_measured";
    metadata["ledger_authorization"] = "not_checked";
    metadata["fee_validation"] = "not_checked";
    metadata["node_wide_lower_bound"] = false;
    metadata["timing"] =
        "instrumented_diagnostic_excludes_parsed_cleanup; not production throughput";
    return ::json::StyledWriter{}.write(metadata);
}

}  // namespace ballast::measurement
