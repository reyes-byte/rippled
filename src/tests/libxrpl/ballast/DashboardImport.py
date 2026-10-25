"""Exercise the actual experiment importer and controlled corruptions."""

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "bin"))
from ballast_dashboard_import import import_experiment

ARCHIVE = ROOT / "artifacts/ballast/stblob-move-experiment.zip"


class DashboardImport(unittest.TestCase):
    def changed_archive(self, change, reseal=False):
        temporary = tempfile.TemporaryDirectory(prefix="ballast-import-test-")
        self.addCleanup(temporary.cleanup)
        with zipfile.ZipFile(ARCHIVE) as archive:
            files = {name: archive.read(name) for name in archive.namelist()}
        change(files)
        if reseal:
            files["checksums.sha256"] = "".join(
                f"{hashlib.sha256(data).hexdigest()}  {name}\n"
                for name, data in sorted(files.items())
                if name != "checksums.sha256"
            ).encode()
        output = Path(temporary.name) / "changed.zip"
        with zipfile.ZipFile(output, "w") as archive:
            for name, data in files.items():
                archive.writestr(name, data)
        return output

    def test_actual_archive_produces_independent_comparison_datasets(self):
        packet = import_experiment(ARCHIVE)
        self.assertEqual(packet["validation"], "csv_validated_at_import")
        self.assertFalse(packet["cryptography_rerun"])
        self.assertEqual(
            [data["summary"]["rows"] for data in packet["datasets"]], [1620, 1620]
        )
        self.assertEqual(
            [data["recorded_tests_passed"] for data in packet["datasets"]], [164, 164]
        )
        groups = [
            next(
                group
                for group in data["summary"]["groups"]
                if group["algorithm"] == "ML-DSA-44"
                and group["signer_count"] == 32
                and group["stage"] == "parse_only"
            )
            for data in packet["datasets"]
        ]
        self.assertEqual(
            [group["allocation_calls"]["median"] for group in groups], [598, 502]
        )

    def test_modified_payload_without_updated_checksum_is_rejected(self):
        def change(files):
            files["baseline/results/run-1.csv.gz"] += b"changed"

        with self.assertRaisesRegex(ValueError, "Checksum mismatch"):
            import_experiment(self.changed_archive(change))

    def test_bundled_validator_is_pinned_and_never_executed(self):
        def change(files):
            files["ballast/measurement/validate_csv.py"] = (
                b"raise RuntimeError('must never execute')"
            )

        with self.assertRaisesRegex(ValueError, "Unknown bundled CSV validator"):
            import_experiment(self.changed_archive(change, reseal=True))

    def test_forged_summary_is_rejected_even_with_updated_checksums(self):
        def change(files):
            value = json.loads(files["move/results/summary.json"])
            value["rows"] += 1
            files["move/results/summary.json"] = json.dumps(value).encode()

        with self.assertRaisesRegex(ValueError, "Summary differs from CSVs"):
            import_experiment(self.changed_archive(change, reseal=True))

    def test_different_pinned_fixture_corpus_is_rejected(self):
        def change(files):
            name = next(name for name in files if name.endswith(".hex"))
            files[name] += b"00"

        with self.assertRaisesRegex(ValueError, "Different pinned corpus"):
            import_experiment(self.changed_archive(change, reseal=True))


if __name__ == "__main__":
    unittest.main()
