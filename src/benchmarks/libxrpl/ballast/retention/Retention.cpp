#include <benchmarks/libxrpl/ballast/retention/Retention.h>

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>

#include <boost/json.hpp>

#include <openssl/crypto.h>

#include <MeasurementBuild.h>
#include <RetentionBuild.h>

#include <chrono>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace ballast::retention {
namespace {

struct Entry
{
    xrpl::Blob wire;
    std::optional<xrpl::STTx> transaction;
};

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
    out << "\",";
}

}  // namespace

Sample
measure(measurement::Fixture const& fixture, xrpl::Rules const& rules, std::size_t count)
{
    if (count == 0 || count > kMaximumCount)
        throw std::invalid_argument("Retained count must be from 1 through 1000");
    xrpl::uint256 expectedId;
    if (!expectedId.parseHex(fixture.transactionId))
        throw std::invalid_argument("Invalid fixture transaction ID");

    Sample sample;
    allocation::Scope scope;
    {
        std::vector<Entry> entries;
        // Reserve before parsing: existing parsed objects never need relocating.
        // Its allocation includes Entry and inline STTx storage, unlike the
        // single-operation benchmark's stack-allocated optional.
        entries.reserve(count);
        sample.containerCapacityBytes = entries.capacity() * sizeof(Entry);
        for (std::size_t i = 0; i < count; ++i)
        {
            entries.emplace_back();
            entries.back().wire = fixture.wire;
            sample.wirePayloadBytes += entries.back().wire.size();
            sample.wireCapacityBytes += entries.back().wire.capacity();
        }
        sample.inputs = scope.snapshot();
        auto const start = std::chrono::steady_clock::now();
        for (auto& entry : entries)
        {
            xrpl::SerialIter iterator{xrpl::makeSlice(entry.wire)};
            entry.transaction.emplace(iterator);
            sample.consumedInputs += iterator.empty();
            sample.matchingTransactionIds += entry.transaction->getTransactionID() == expectedId;
            {
                auto const checked = entry.transaction->checkSign(rules);
                sample.validSignatures += checked.has_value();
            }
            ++sample.retainedObjects;
        }
        auto const stop = std::chrono::steady_clock::now();
        sample.elapsedNanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
        sample.held = scope.snapshot();
    }  // Destroy parsed objects, independent wires and container while counting.
    sample.completed = scope.finish();
    return sample;
}

void
writeCsvHeader(std::ostream& out)
{
    out << "schema_version,run_id,fixture_id,algorithm,signer_count,retained_count,sample_index,"
           "warmup_iterations,workload,coverage,input_storage,corpus_storage,rss,wire_sha256,"
           "transaction_id,candidate_commit,crypto_commit,measurement_source_sha256,"
           "dependency_manifest_sha256,compiler_id,compiler_version,build_type,build_flags,openssl_"
           "version,rules,"
           "container_capacity_bytes,wire_payload_bytes,wire_capacity_bytes,inputs_live_bytes,"
           "held_live_bytes,held_parsed_increment_bytes,peak_live_bytes,allocation_calls,"
           "requested_bytes,free_calls,freed_bytes,final_live_bytes,final_live_blocks,"
           "non_owned_free_calls,cleanup_allocation_calls,retained_objects,consumed_inputs,"
           "valid_signatures,matching_transaction_ids,elapsed_ns\n";
}

void
writeCsvRow(
    std::ostream& out,
    measurement::Fixture const& fixture,
    Sample const& sample,
    std::size_t count,
    std::size_t sampleIndex,
    std::size_t warmups,
    std::string_view runId)
{
    namespace base = measurement::build;
    out << "1,";
    textField(out, runId);
    textField(out, fixture.id);
    textField(out, fixture.algorithm);
    out << fixture.signerCount << ',' << count << ',' << sampleIndex << ',' << warmups << ',';
    for (std::string_view value :
         {"retained_repeated_fixture_objects",
          "global_cpp_new_delete_requested_bytes",
          "independent_copies_included",
          "excluded",
          "not_measured"})
        textField(out, value);
    for (std::string_view value :
         {std::string_view(fixture.wireSha256),
          std::string_view(fixture.transactionId),
          base::candidateCommit,
          base::cryptoCommit,
          std::string_view(build::sourceHash),
          std::string_view(build::dependencyHash),
          base::compiler,
          base::compilerVersion,
          base::buildType,
          base::flags})
        textField(out, value);
    textField(out, OpenSSL_version(OPENSSL_VERSION));
    textField(out, "featureQuantum_only");
    auto const& final = sample.completed;
    out << sample.containerCapacityBytes << ',' << sample.wirePayloadBytes << ','
        << sample.wireCapacityBytes << ',' << sample.inputs.liveBytes << ','
        << sample.held.liveBytes << ',' << sample.held.liveBytes - sample.inputs.liveBytes << ','
        << final.peakLiveBytes << ',' << final.allocations << ',' << final.requestedBytes << ','
        << final.frees << ',' << final.freedBytes << ',' << final.liveBytes << ','
        << final.liveBlocks << ',' << final.nonOwnedFrees << ','
        << final.allocations - sample.held.allocations << ',' << sample.retainedObjects << ','
        << sample.consumedInputs << ',' << sample.validSignatures << ','
        << sample.matchingTransactionIds << ',' << sample.elapsedNanoseconds << '\n';
}

std::string
metadataJson(std::string_view runId, std::size_t samples, std::size_t warmups)
{
    namespace base = measurement::build;
    boost::json::object value{
        {"schema_version", 1},
        {"run_id", std::string(runId)},
        {"samples_per_fixture_count", samples},
        {"warmup_iterations_per_fixture_count", warmups},
        {"workload", "retained_repeated_fixture_objects"},
        {"coverage", "global_cpp_new_delete_requested_bytes"},
        {"input_storage", "independent_copies_included"},
        {"corpus_storage", "excluded"},
        {"rss", "not_measured"},
        {"ledger_authorization", "not_checked"},
        {"queue_admission", "not_tested"},
        {"unique_transactions", false},
        {"network_submissions", 0},
        {"node_wide_lower_bound", false},
        {"candidate_commit", std::string(base::candidateCommit)},
        {"crypto_commit", std::string(base::cryptoCommit)},
        {"measurement_source_sha256", build::sourceHash},
        {"dependency_manifest_sha256", build::dependencyHash},
        {"compiler_id", std::string(base::compiler)},
        {"compiler_version", std::string(base::compilerVersion)},
        {"build_type", std::string(base::buildType)},
        {"build_flags", std::string(base::flags)},
        {"openssl_version", OpenSSL_version(OPENSSL_VERSION)},
        {"rules", "featureQuantum_only"},
        {"timing", "instrumented_parse_verify_batch_excludes_input_setup_and_cleanup"},
        {"container_storage", "included_with_inline_STTx"},
        {"tracker_metadata", "excluded"}};
    return boost::json::serialize(value) + "\n";
}

}  // namespace ballast::retention
