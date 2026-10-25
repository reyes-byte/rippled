"""Validate scoped Ballast CSVs and summarize repeated measurements (stdlib only)."""

import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import statistics
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
FIXTURES = HERE.parent / "pq/native/fixtures/multisign"
PINS = json.loads((HERE.parent / "pq/pins.json").read_text())
STAGES = {"parse_only", "verify_only", "parse_verify"}
METRICS = ["allocation_calls", "requested_bytes", "free_calls", "freed_bytes",
           "peak_live_bytes", "live_bytes_at_operation_end", "live_bytes_at_scope_end",
           "live_blocks_at_scope_end", "cleanup_allocation_calls", "cleanup_requested_bytes",
           "non_owned_free_calls"]
STATIC = ["run_id", "warmup_iterations", "candidate_commit", "crypto_commit",
          "measurement_source_sha256", "dependency_manifest_sha256", "compiler_id",
          "compiler_version", "build_type", "build_flags", "openssl_version"]


def validate(path):
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        fields = reader.fieldnames
        assert fields and len(fields) == len(set(fields)), "Missing/duplicate CSV columns"
        rows = list(reader)
    assert rows, "CSV has no samples"
    expected = {}
    for file in FIXTURES.glob("*.json"):
        fixture = json.loads(file.read_text())
        wire = bytes.fromhex(file.with_suffix(".hex").read_text())
        assert hashlib.sha256(wire).hexdigest() == fixture["wire_sha256"]
        assert hashlib.sha512(b"TXN\0" + wire).digest()[:32].hex().upper() == fixture["transaction_id"]
        expected[fixture["fixture_id"]] = fixture
    assert len(expected) == 6
    groups = {}
    for row in rows:
        assert None not in row and all(value is not None for value in row.values()), "Malformed CSV row"
        assert row["schema_version"] == "1"
        fixture = expected[row["fixture_id"]]
        stage = row["stage"]
        assert stage in STAGES
        assert row["algorithm"] == fixture["algorithm"] and row["signing_mode"] == "multi"
        assert int(row["signer_count"]) == fixture["signer_count"]
        assert row["wire_sha256"] == fixture["wire_sha256"]
        assert row["transaction_id"] == fixture["transaction_id"]
        assert int(row["wire_bytes"]) == fixture["wire_bytes"]
        assert int(row["wire_capacity_bytes"]) >= int(row["wire_bytes"])
        assert row["candidate_commit"] == PINS["candidate"]["commit"]
        assert row["crypto_commit"] == PINS["reference"]["commit"]
        assert row["coverage"] == "global_cpp_new_delete_requested_bytes"
        assert row["allocation_thread_policy"] == "allocating_thread_innermost_scope"
        assert row["input_storage"] == "excluded"
        assert row["initialization"] == "prevalidated_then_stage_warmed"
        assert row["rules"] == "featureQuantum_only"
        assert row["elapsed_scope"] == "instrumented_operation_excludes_parsed_cleanup"
        assert row["preparsed_storage"] == ("excluded" if stage == "verify_only" else "not_preparsed")
        assert row["parsed_destruction"] == ("after_scope" if stage == "verify_only" else "inside_scope_after_operation")
        assert row["signature_result"] == ("not_checked_in_scope" if stage == "parse_only" else "valid")
        assert int(row["warmup_iterations"]) >= 1 and int(row["elapsed_ns"]) >= 0
        for name in ["measurement_source_sha256", "dependency_manifest_sha256"]:
            assert len(row[name]) == 64 and all(ch in "0123456789abcdef" for ch in row[name])
        for key in STATIC:
            assert row[key] == rows[0][key], f"Inconsistent {key}"
        for name in METRICS:
            assert int(row[name]) >= 0, name
        assert int(row["allocation_calls"]) == int(row["free_calls"])
        assert int(row["requested_bytes"]) == int(row["freed_bytes"])
        assert int(row["live_bytes_at_scope_end"]) == int(row["live_blocks_at_scope_end"]) == 0
        assert int(row["non_owned_free_calls"]) == 0
        assert int(row["live_bytes_at_operation_end"]) <= int(row["peak_live_bytes"]) <= int(row["requested_bytes"])
        if stage == "verify_only":
            assert int(row["live_bytes_at_operation_end"]) == 0
        else:
            assert int(row["live_bytes_at_operation_end"]) > 0
        groups.setdefault((row["fixture_id"], stage), []).append(row)
    assert set(groups) == {(fixture, stage) for fixture in expected for stage in STAGES}
    sizes = {len(group) for group in groups.values()}
    assert len(sizes) == 1
    for group in groups.values():
        assert sorted(int(row["sample_index"]) for row in group) == list(range(len(group)))
    sidecar = path.with_suffix(".json")
    if sidecar.exists():
        metadata = json.loads(sidecar.read_text())
        assert metadata["samples_per_fixture_stage"] == len(next(iter(groups.values())))
        assert metadata["warmup_iterations_per_fixture_stage"] == int(rows[0]["warmup_iterations"])
        for key in STATIC:
            if key != "warmup_iterations":
                assert metadata[key] == rows[0][key], f"Sidecar differs: {key}"
        assert metadata["rss"] == "not_measured" and metadata["node_wide_lower_bound"] is False
    return rows


def summarize(paths):
    all_rows = [row for path in paths for row in validate(path)]
    for field in [name for name in STATIC if name != "run_id"]:
        assert len({row[field] for row in all_rows}) == 1, f"Different configurations: {field}"
    groups = {}
    for row in all_rows:
        groups.setdefault((row["algorithm"], int(row["signer_count"]), row["stage"]), []).append(row)
    summary = []
    for (algorithm, count, stage), rows in sorted(groups.items()):
        result = {"algorithm": algorithm, "signer_count": count, "stage": stage, "samples": len(rows)}
        for field in ["requested_bytes", "peak_live_bytes", "live_bytes_at_operation_end", "elapsed_ns"]:
            values = [int(row[field]) for row in rows]
            result[field] = {"min": min(values), "median": statistics.median(values),
                             "max": max(values), "population_stddev": statistics.pstdev(values)}
        result["allocation_metric_variants"] = len({tuple(row[name] for name in METRICS) for row in rows})
        summary.append(result)
    return {"schema_version": 1, "validated_files": len(paths), "rows": len(all_rows),
            "coverage": "global_cpp_new_delete_requested_bytes", "input_storage": "excluded",
            "rss": "not_measured", "timing": "instrumented_diagnostic", "groups": summary}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csvs", nargs="*", type=Path)
    parser.add_argument("--smoke", type=Path, help="Run the benchmark twice and validate its CSV/JSON output")
    args = parser.parse_args()
    if args.smoke:
        assert not args.csvs
        with tempfile.TemporaryDirectory(prefix="ballast-csv-") as directory:
            paths = []
            for i in range(2):
                path = Path(directory) / f"run-{i}.csv"
                run_id = f'smoke-{i},"quoted"'
                result = subprocess.run([str(args.smoke), "--samples", "2", "--warmup", "3",
                                         "--run-id", run_id, "--metadata", str(path.with_suffix(".json"))],
                                        check=True, capture_output=True, text=True, timeout=60)
                path.write_text(result.stdout)
                assert list(csv.DictReader(io.StringIO(result.stdout)))[0]["run_id"] == run_id
                paths.append(path)
            print(json.dumps(summarize(paths), indent=2))
    else:
        if not args.csvs:
            parser.error("Provide CSV paths or --smoke")
        print(json.dumps(summarize(args.csvs), indent=2))


if __name__ == "__main__":
    main()
