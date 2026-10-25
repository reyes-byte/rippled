#!/usr/bin/env python3
"""Create, verify, and archive a portable Ballast results handoff (stdlib only)."""

import argparse
import csv
import gzip
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import runpy
import shutil
import stat
import subprocess
import tempfile
import zipfile

VALIDATOR_SHA256 = "154de3902174e321df4ffd9d03a7b34bdbc8d5dec3f7e119a7cac9ef269e600b"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def safe_relative(name):
    path = PurePosixPath(name)
    require("\\" not in name and ":" not in name, "Nonportable path")
    require(not path.is_absolute() and ".." not in path.parts, "Unsafe path")
    require(path.parts and str(path) == name, "Noncanonical path")
    return path


def summarize(bundle, csv_paths):
    validator = bundle / "ballast/measurement/validate_csv.py"
    require(digest(validator) == VALIDATOR_SHA256, "Unknown bundled validator version")
    return runpy.run_path(str(validator))["summarize"](csv_paths)


def summary_matches(actual, recorded):
    if isinstance(actual, dict):
        if not isinstance(recorded, dict) or actual.keys() != recorded.keys():
            return False
        return all(
            (
                math.isclose(actual[key], recorded[key], rel_tol=1e-12, abs_tol=1e-9)
                if key == "population_stddev"
                else summary_matches(actual[key], recorded[key])
            )
            for key in actual
        )
    if isinstance(actual, list):
        return (
            isinstance(recorded, list)
            and len(actual) == len(recorded)
            and all(summary_matches(a, b) for a, b in zip(actual, recorded))
        )
    return actual == recorded


def verify_directory(bundle):
    bundle = bundle.resolve()
    manifest = bundle / "checksums.sha256"
    expected = set()
    for line in manifest.read_text().splitlines():
        checksum, name = line.split("  ", 1)
        safe_relative(name)
        require(re.fullmatch(r"[0-9a-f]{64}", checksum), "Invalid checksum")
        require(name not in expected, "Duplicate manifest path")
        expected.add(name)
        path = bundle / name
        require(path.is_file() and not path.is_symlink(), f"Missing file: {name}")
        require(path.resolve().is_relative_to(bundle), "Path escapes bundle")
        require(digest(path) == checksum, f"Checksum mismatch: {name}")
    actual = set()
    for path in bundle.rglob("*"):
        require(not path.is_symlink(), "Symlinks are not permitted in bundles")
        if path.is_file() and path != manifest:
            actual.add(path.relative_to(bundle).as_posix())
    require(actual == expected, "Bundle file inventory differs from manifest")

    with tempfile.TemporaryDirectory(prefix="ballast-verify-") as directory:
        csv_paths = []
        for compressed in sorted((bundle / "results").glob("run-*.csv.gz")):
            csv_path = Path(directory) / compressed.name.removesuffix(".gz")
            csv_path.write_bytes(gzip.decompress(compressed.read_bytes()))
            shutil.copyfile(
                compressed.with_name(csv_path.with_suffix(".json").name),
                csv_path.with_suffix(".json"),
            )
            csv_paths.append(csv_path)
        require(csv_paths, "No CSVs in bundle")
        summary = summarize(bundle, csv_paths)
        recorded = json.loads((bundle / "results/summary.json").read_text())
        require(summary_matches(summary, recorded), "Summary differs from raw CSVs")
        provenance = json.loads((bundle / "provenance.json").read_text())
        require(summary["rows"] == provenance["rows"], "Row count mismatch")
        manifest_hash = digest(bundle / "results/source-sha256.txt")
        for path in csv_paths:
            with path.open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            require(
                all(row["dependency_manifest_sha256"] == manifest_hash for row in rows),
                "Source manifest differs from CSV provenance",
            )
            require(
                digest(path) == provenance["csv_sha256"][path.name],
                "Decoded CSV fingerprint mismatch",
            )
    return {
        "verified_files": len(expected),
        "validated_runs": summary["validated_files"],
        "rows": summary["rows"],
        "fixture_stage_groups": len(summary["groups"]),
        "coverage": summary["coverage"],
        "rss": "not_measured",
        "cryptography_reexecuted": False,
    }


def verify(path):
    if path.is_dir():
        return verify_directory(path)
    with tempfile.TemporaryDirectory(prefix="ballast-unzip-") as directory:
        with zipfile.ZipFile(path) as archive:
            names = [entry.filename for entry in archive.infolist()]
            require(len(names) == len(set(names)), "Duplicate archive member")
            for entry in archive.infolist():
                safe_relative(entry.filename.rstrip("/"))
                require(
                    not stat.S_ISLNK(entry.external_attr >> 16),
                    "Symlink archive member",
                )
            archive.extractall(directory)
        return verify_directory(Path(directory))


def archive_bundle(bundle, output):
    report = verify_directory(bundle)
    require(not output.exists(), f"Archive already exists: {output}")
    require(
        not output.resolve().is_relative_to(bundle.resolve()),
        "Archive must be outside the manifested bundle directory",
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "x", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(bundle.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(bundle).as_posix())
    require(verify(output) == report, "Archive differs from directory payload")
    return report


def create_bundle(repo, results, output, ctest_log=None):
    require(not output.exists(), f"Bundle directory already exists: {output}")
    validator = repo / "src/benchmarks/libxrpl/ballast/measurement/validate_csv.py"
    require(
        digest(validator) == VALIDATOR_SHA256,
        "Update the handoff's validator pin before packaging",
    )
    csv_paths = sorted(results.glob("run-*.csv"))
    require(csv_paths, "No run-*.csv files found")
    require(
        all(path.with_suffix(".json").is_file() for path in csv_paths),
        "Every CSV needs its JSON sidecar",
    )
    summary = runpy.run_path(str(validator))["summarize"](csv_paths)
    recorded = json.loads((results / "summary.json").read_text())
    require(summary_matches(summary, recorded), "Existing summary differs from CSVs")
    require((results / "source-sha256.txt").is_file(), "Source manifest required")
    output.mkdir(parents=True)
    payload = output / "results"
    payload.mkdir()
    csv_hashes = {}
    for path in csv_paths:
        csv_hashes[path.name] = digest(path)
        (payload / (path.name + ".gz")).write_bytes(
            gzip.compress(path.read_bytes(), mtime=0)
        )
        shutil.copyfile(
            path.with_suffix(".json"), payload / path.with_suffix(".json").name
        )
    for name in [
        "summary.json",
        "source-sha256.txt",
        "accounting-source-sha256.txt",
        "validation.json",
    ]:
        if (results / name).is_file():
            shutil.copyfile(results / name, payload / name)
    fixture_root = repo / "src/benchmarks/libxrpl/ballast/pq/native/fixtures"
    shutil.copytree(fixture_root, output / "ballast/pq/native/fixtures")
    shutil.copyfile(
        repo / "src/benchmarks/libxrpl/ballast/pq/pins.json",
        output / "ballast/pq/pins.json",
    )
    copied_validator = output / "ballast/measurement/validate_csv.py"
    copied_validator.parent.mkdir(parents=True)
    shutil.copyfile(validator, copied_validator)
    shutil.copyfile(__file__, output / "verify.py")
    provenance = {
        "schema": 1,
        "repository_head_at_packaging": subprocess.check_output(
            ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True
        ).strip(),
        "rows": summary["rows"],
        "runs": len(csv_paths),
        "csv_sha256": csv_hashes,
        "compression": "gzip_mtime_zero_lossless",
        "coverage": summary["coverage"],
        "input_storage": "excluded",
        "rss": "not_measured",
        "ledger_authorization": "not_checked",
        "node_wide_lower_bound": False,
        "offline_verification": "payload_integrity_and_csv_semantics_only",
    }
    if ctest_log:
        text = ctest_log.read_text()
        match = re.search(
            r"100% tests passed(?:,\s*0 tests failed)? out of (\d+)", text
        )
        require(match and "***Skipped" not in text, "CTest did not complete cleanly")
        provenance["ctest_passed"] = int(match.group(1))
        shutil.copyfile(ctest_log, payload / "ctest.log")
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    (output / "README.md").write_text(
        "# Ballast results handoff\n\n"
        "Verify without Nix or a compiler:\n\n"
        "```sh\npython3 verify.py verify .\n```\n\n"
        "This checks file integrity, fixture fingerprints/transaction IDs, the full "
        "CSV matrix, sidecars, and the recorded summary. It does not rerun signature "
        "cryptography or prove authenticity of the publisher. Full native tests are "
        "available through bin/ballast-handoff in the source repository.\n\n"
        "Results are warm scoped C++ allocation requests, not RSS or all heap memory. "
        "Input storage is excluded; verify-only also excludes the parsed transaction. "
        "Timing includes instrumentation overhead. No ledger authorization, fee "
        "validation, network submission, or node-wide lower bound is claimed.\n\n"
        "CSVs are gzip-compressed losslessly; JSON sidecars and summary remain readable. "
        "The source manifest identifies measured code, not every source file in this "
        "data bundle. Public fixture keys must never be used for funds.\n"
    )
    manifest_lines = [
        f"{digest(path)}  {path.relative_to(output).as_posix()}\n"
        for path in sorted(output.rglob("*"))
        if path.is_file()
    ]
    (output / "checksums.sha256").write_text("".join(manifest_lines))
    return verify_directory(output)


def main():
    require(__debug__, "Run verification without Python -O")
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create")
    create.add_argument(
        "--repo", type=Path, default=Path(__file__).resolve().parents[1]
    )
    create.add_argument("--results", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--ctest-log", type=Path)
    check = commands.add_parser("verify")
    check.add_argument("bundle", type=Path)
    archive = commands.add_parser("archive")
    archive.add_argument("bundle", type=Path)
    archive.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "create":
        report = create_bundle(
            args.repo.resolve(), args.results, args.output, args.ctest_log
        )
    elif args.command == "archive":
        report = archive_bundle(args.bundle, args.output)
    else:
        report = verify(args.bundle)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
