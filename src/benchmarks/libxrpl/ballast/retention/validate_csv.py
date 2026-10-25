"""Validate retained-object workloads; these are not node-queue or RSS tests."""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
FIXTURES = HERE.parent / "pq/native/fixtures/multisign"
PINS = json.loads((HERE.parent / "pq/pins.json").read_text())
COUNTS = {1, 10, 100, 1000}
METRICS = [
    "container_capacity_bytes",
    "wire_payload_bytes",
    "wire_capacity_bytes",
    "inputs_live_bytes",
    "held_live_bytes",
    "held_parsed_increment_bytes",
    "peak_live_bytes",
    "allocation_calls",
    "requested_bytes",
    "free_calls",
    "freed_bytes",
    "final_live_bytes",
    "final_live_blocks",
    "non_owned_free_calls",
    "cleanup_allocation_calls",
]
STATIC = [
    "run_id",
    "warmup_iterations",
    "candidate_commit",
    "crypto_commit",
    "measurement_source_sha256",
    "dependency_manifest_sha256",
    "compiler_id",
    "compiler_version",
    "build_type",
    "build_flags",
    "openssl_version",
    "rules",
]


def validate(path, counts=COUNTS):
    assert __debug__, "Run Python without -O"
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        fields = reader.fieldnames
        assert fields and len(fields) == len(set(fields)), "Missing/duplicate columns"
        rows = list(reader)
    assert rows, "No samples"
    expected = {}
    for path_fixture in FIXTURES.glob("*.json"):
        fixture = json.loads(path_fixture.read_text())
        wire = bytes.fromhex(path_fixture.with_suffix(".hex").read_text())
        assert hashlib.sha256(wire).hexdigest() == fixture["wire_sha256"]
        assert (
            hashlib.sha512(b"TXN\0" + wire).digest()[:32].hex().upper()
            == fixture["transaction_id"]
        )
        expected[fixture["fixture_id"]] = fixture
    assert len(expected) == 6
    groups = {}
    for row in rows:
        assert None not in row and all(value is not None for value in row.values())
        assert row["schema_version"] == "1"
        fixture = expected[row["fixture_id"]]
        count = int(row["retained_count"])
        assert count in counts
        assert row["algorithm"] == fixture["algorithm"]
        assert int(row["signer_count"]) == fixture["signer_count"]
        assert row["wire_sha256"] == fixture["wire_sha256"]
        assert row["transaction_id"] == fixture["transaction_id"]
        assert row["candidate_commit"] == PINS["candidate"]["commit"]
        assert row["crypto_commit"] == PINS["reference"]["commit"]
        assert row["workload"] == "retained_repeated_fixture_objects"
        assert row["coverage"] == "global_cpp_new_delete_requested_bytes"
        assert row["input_storage"] == "independent_copies_included"
        assert row["corpus_storage"] == "excluded" and row["rss"] == "not_measured"
        assert row["rules"] == "featureQuantum_only"
        assert int(row["warmup_iterations"]) >= 1 and int(row["elapsed_ns"]) >= 0
        for name in ["measurement_source_sha256", "dependency_manifest_sha256"]:
            assert len(row[name]) == 64 and all(
                ch in "0123456789abcdef" for ch in row[name]
            )
        for key in STATIC:
            assert row[key] == rows[0][key], f"Inconsistent {key}"
        numbers = {name: int(row[name]) for name in METRICS}
        assert all(value >= 0 for value in numbers.values())
        assert numbers["wire_payload_bytes"] == fixture["wire_bytes"] * count
        assert numbers["wire_capacity_bytes"] >= numbers["wire_payload_bytes"]
        assert (
            numbers["inputs_live_bytes"]
            == numbers["wire_capacity_bytes"] + numbers["container_capacity_bytes"]
        )
        assert (
            numbers["held_parsed_increment_bytes"]
            == numbers["held_live_bytes"] - numbers["inputs_live_bytes"]
            > 0
        )
        assert (
            numbers["inputs_live_bytes"]
            < numbers["held_live_bytes"]
            <= numbers["peak_live_bytes"]
            <= numbers["requested_bytes"]
        )
        assert numbers["allocation_calls"] == numbers["free_calls"]
        assert numbers["requested_bytes"] == numbers["freed_bytes"]
        assert (
            numbers["final_live_bytes"]
            == numbers["final_live_blocks"]
            == numbers["non_owned_free_calls"]
            == 0
        )
        for name in [
            "retained_objects",
            "consumed_inputs",
            "valid_signatures",
            "matching_transaction_ids",
        ]:
            assert int(row[name]) == count, name
        groups.setdefault((row["fixture_id"], count), []).append(row)
    assert set(groups) == {(fixture, count) for fixture in expected for count in counts}
    sizes = {len(group) for group in groups.values()}
    assert len(sizes) == 1
    for group in groups.values():
        assert sorted(int(row["sample_index"]) for row in group) == list(
            range(len(group))
        )
    sidecar = path.with_suffix(".json")
    assert sidecar.exists(), "Metadata sidecar is required"
    metadata = json.loads(sidecar.read_text())
    assert metadata["samples_per_fixture_count"] == next(iter(sizes))
    assert metadata["warmup_iterations_per_fixture_count"] == int(
        rows[0]["warmup_iterations"]
    )
    for key in STATIC:
        if key != "warmup_iterations":
            assert metadata[key] == rows[0][key], f"Sidecar differs: {key}"
    assert (
        metadata["unique_transactions"] is False
        and metadata["queue_admission"] == "not_tested"
    )
    assert (
        metadata["rss"] == "not_measured" and metadata["node_wide_lower_bound"] is False
    )
    assert metadata["input_storage"] == "independent_copies_included"
    assert metadata["container_storage"] == "included_with_inline_STTx"
    assert (
        metadata["corpus_storage"] == "excluded"
        and metadata["ledger_authorization"] == "not_checked"
    )
    assert metadata["network_submissions"] == 0
    return rows


def summarize(paths, counts=COUNTS):
    rows = [row for path in paths for row in validate(path, counts)]
    for name in STATIC:
        if name != "run_id":
            assert len({row[name] for row in rows}) == 1, f"Different {name}"
    groups = {}
    for row in rows:
        key = row["algorithm"], int(row["signer_count"]), int(row["retained_count"])
        groups.setdefault(key, []).append(row)
    result = []
    for (algorithm, signers, retained), samples in sorted(groups.items()):
        group = {
            "algorithm": algorithm,
            "signer_count": signers,
            "retained_count": retained,
            "samples": len(samples),
        }
        for metric in METRICS + ["elapsed_ns"]:
            values = [int(row[metric]) for row in samples]
            group[metric] = {
                "min": min(values),
                "median": statistics.median(values),
                "max": max(values),
                "population_stddev": statistics.pstdev(values),
            }
        group["allocation_metric_variants"] = len(
            {tuple(row[name] for name in METRICS) for row in samples}
        )
        result.append(group)
    return {
        "schema_version": 1,
        "workload": "retained_repeated_fixture_objects",
        "validated_files": len(paths),
        "rows": len(rows),
        "groups": result,
        "coverage": "global_cpp_new_delete_requested_bytes",
        "input_storage": "independent_copies_included",
        "corpus_storage": "excluded",
        "rss": "not_measured",
        "unique_transactions": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csvs", nargs="*", type=Path)
    parser.add_argument("--smoke", type=Path)
    args = parser.parse_args()
    if args.smoke:
        with tempfile.TemporaryDirectory(prefix="ballast-retention-") as directory:
            paths = []
            for index in range(2):
                path = Path(directory) / f"run-{index}.csv"
                with path.open("w") as output:
                    subprocess.run(
                        [
                            str(args.smoke),
                            "--counts",
                            "1,2",
                            "--samples",
                            "1",
                            "--warmup",
                            "1",
                            "--run-id",
                            f'run-{index},"quoted"',
                            "--metadata",
                            str(path.with_suffix(".json")),
                        ],
                        stdout=output,
                        check=True,
                        timeout=60,
                    )
                paths.append(path)
            summary = summarize(paths, {1, 2})
            assert summary["rows"] == 24
            print(json.dumps(summary))
    else:
        if not args.csvs:
            parser.error("Provide CSV paths or --smoke")
        print(json.dumps(summarize(args.csvs), indent=2))


if __name__ == "__main__":
    main()
