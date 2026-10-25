#!/usr/bin/env python3
"""Validate a Ballast experiment archive and export comparison data for the C++ viewer."""

import argparse
import csv
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import runpy
import stat
import statistics
import tempfile
import zipfile

from ballast_bundle import VALIDATOR_SHA256, safe_relative, summary_matches

ROOT = Path(__file__).resolve().parent.parent
BALLAST = ROOT / "src/benchmarks/libxrpl/ballast"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_archive(path):
    with zipfile.ZipFile(path) as archive:
        names = [entry.filename for entry in archive.infolist()]
        require(len(names) == len(set(names)), "Duplicate archive member")
        require(
            sum(entry.file_size for entry in archive.infolist()) <= 128 * 1024 * 1024,
            "Experiment archive is too large",
        )
        for entry in archive.infolist():
            safe_relative(entry.filename)
            require(
                not stat.S_ISLNK(entry.external_attr >> 16), "Symlink archive member"
            )
        files = {name: archive.read(name) for name in names}
    expected = {}
    for line in files["checksums.sha256"].decode().splitlines():
        checksum, name = line.split("  ", 1)
        safe_relative(name)
        require(name not in expected, "Duplicate checksum path")
        require(re.fullmatch(r"[0-9a-f]{64}", checksum), "Invalid checksum")
        expected[name] = checksum
    require(
        set(files) - {"checksums.sha256"} == set(expected), "Archive inventory mismatch"
    )
    for name, checksum in expected.items():
        require(digest(files[name]) == checksum, f"Checksum mismatch: {name}")
    return files


def import_experiment(path):
    require(
        __debug__, "Run Python without -O so the CSV validator's assertions execute"
    )
    files = read_archive(path)
    validator_path = BALLAST / "measurement/validate_csv.py"
    require(
        digest(validator_path.read_bytes()) == VALIDATOR_SHA256,
        "Unknown local CSV validator",
    )
    require(
        digest(files["ballast/measurement/validate_csv.py"]) == VALIDATOR_SHA256,
        "Unknown bundled CSV validator",
    )
    # The existing validator uses the repository's fixed corpus. Establish that
    # these are the same fixtures/pins before invoking that trusted local code.
    for name in ["pq/pins.json"] + [
        str(file.relative_to(BALLAST))
        for file in sorted((BALLAST / "pq/native/fixtures/multisign").iterdir())
        if file.suffix in {".hex", ".json"}
    ]:
        require(
            files[f"ballast/{name}"] == (BALLAST / name).read_bytes(),
            f"Different pinned corpus: {name}",
        )
    validator = runpy.run_path(str(validator_path))
    datasets = []
    for variant, label in [("baseline", "Baseline"), ("move", "Move constructor")]:
        with tempfile.TemporaryDirectory(prefix="ballast-import-") as directory:
            paths = []
            for name in sorted(files):
                if not re.fullmatch(rf"{variant}/results/run-[0-9]+\.csv\.gz", name):
                    continue
                output = Path(directory) / Path(name).name.removesuffix(".gz")
                with gzip.GzipFile(fileobj=io.BytesIO(files[name])) as compressed:
                    decoded = compressed.read(64 * 1024 * 1024 + 1)
                require(len(decoded) <= 64 * 1024 * 1024, "Decoded CSV is too large")
                output.write_bytes(decoded)
                sidecar = name.removesuffix(".csv.gz") + ".json"
                output.with_suffix(".json").write_bytes(files[sidecar])
                paths.append(output)
            require(paths, f"No CSVs for {variant}")
            summary = validator["summarize"](paths)
            recorded = json.loads(files[f"{variant}/results/summary.json"])
            require(
                summary_matches(summary, recorded),
                f"Summary differs from CSVs: {variant}",
            )
            rows = []
            for output in paths:
                with output.open(newline="") as stream:
                    rows.extend(csv.DictReader(stream))
            metadata = json.loads(paths[0].with_suffix(".json").read_text())
            manifest_hash = digest(files[f"{variant}/source-sha256.txt"])
            require(
                all(row["dependency_manifest_sha256"] == manifest_hash for row in rows),
                f"Compiled manifest differs: {variant}",
            )
            fixture_hashes = {row["fixture_id"]: row["wire_sha256"] for row in rows}
            for group in summary["groups"]:
                counts = [
                    int(row["allocation_calls"])
                    for row in rows
                    if row["algorithm"] == group["algorithm"]
                    and int(row["signer_count"]) == group["signer_count"]
                    and row["stage"] == group["stage"]
                ]
                group["allocation_calls"] = {
                    "min": min(counts),
                    "median": statistics.median(counts),
                    "max": max(counts),
                    "population_stddev": statistics.pstdev(counts),
                }
            tests = re.search(
                rb"100% tests passed out of ([0-9]+)", files[f"{variant}/ctest.log"]
            )
            require(
                tests is not None, f"Missing recorded passing test count: {variant}"
            )
            datasets.append(
                {
                    "id": variant,
                    "label": label,
                    "summary": summary,
                    "metadata": metadata,
                    "fixture_sha256": fixture_hashes,
                    "recorded_tests_passed": int(tests.group(1)),
                }
            )
    before, after = datasets
    require(
        "stblob_move=OFF" in before["metadata"]["build_flags"]
        and "stblob_move=ON" in after["metadata"]["build_flags"],
        "Unexpected baseline/optimized build variants",
    )
    for key in [
        "candidate_commit",
        "crypto_commit",
        "measurement_source_sha256",
        "compiler_id",
        "compiler_version",
        "build_type",
        "openssl_version",
        "coverage",
        "input_storage",
        "samples_per_fixture_stage",
        "warmup_iterations_per_fixture_stage",
    ]:
        require(
            before["metadata"][key] == after["metadata"][key], f"Incompatible {key}"
        )
    require(
        before["fixture_sha256"] == after["fixture_sha256"],
        "Different transaction fixtures",
    )
    normalize = lambda flags: re.sub(
        r"stblob_move=(ON|OFF)", "stblob_move=VARIANT", flags
    )
    require(
        normalize(before["metadata"]["build_flags"])
        == normalize(after["metadata"]["build_flags"]),
        "Incompatible build flags",
    )
    return {
        "schema_version": 2,
        "kind": "comparison",
        "label": "STBlob move constructor",
        "source_bundle_sha256": digest(path.read_bytes()),
        "validation": "csv_validated_at_import",
        "validator_sha256": VALIDATOR_SHA256,
        "cryptography_rerun": False,
        "datasets": datasets,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("experiment", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        packet = import_experiment(args.experiment)
    except (ValueError, KeyError, OSError, AssertionError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Import failed: {error}\n")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Replace atomically so an interrupted import cannot leave half a packet.
    with tempfile.NamedTemporaryFile(
        mode="w", dir=args.output.parent, delete=False
    ) as output:
        temporary = Path(output.name)
        try:
            json.dump(packet, output, indent=2)
            output.write("\n")
            output.close()
            temporary.replace(args.output)
        finally:
            temporary.unlink(missing_ok=True)
    print(
        f"Imported {sum(data['summary']['rows'] for data in packet['datasets'])} CSV rows; "
        "signature tests recorded, not rerun"
    )


if __name__ == "__main__":
    main()
