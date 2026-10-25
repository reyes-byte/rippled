# Recorded Ballast baseline

`dashboard.json` is a derived display packet for the
[C++ terminal dashboard](../../src/benchmarks/libxrpl/ballast/dashboard/README.md).
It contains combined and individual-run summaries of the immutable ZIP below.
The viewer checks its structure and scope labels, but does not revalidate CSVs
or rerun signatures on opening. Run `bash bin/ballast-dashboard` to browse it.

`baseline-2026-10-25.zip` is the immutable results snapshot for this handoff.
It contains three losslessly compressed CSVs (1620 rows total), JSON sidecars
and summary, complete wire fixtures, revision pins, source fingerprints,
recorded validation, and a checksummed inventory.

Verify it without Nix or a compiler:

```sh
python3 bin/ballast_bundle.py verify artifacts/ballast/baseline-2026-10-25.zip
```

Extract with Python's zipfile CLI to read `results/summary.json`, or decompress
any `results/run-*.csv.gz` using gzip. Offline verification checks integrity and
CSV consistency; it does not rerun native signature cryptography.

The original baseline passed 155 tests. It used Nix GCC 15.3.0, C++23, Release,
OpenSSL 3.6.3, three discarded warm-ups and 30 samples per fixture/stage in each
of three fresh processes. Allocation metrics were stable across repeats.

Medians in tracked requested C++ bytes, with retained wire storage excluded:

| Algorithm | Signers | Parse peak | Live after parse | Verify-only peak |
| --------- | ------: | ---------: | ---------------: | ---------------: |
| secp256k1 |       1 |       3512 |             2872 |              529 |
| secp256k1 |       8 |       8430 |             5612 |              529 |
| secp256k1 |      32 |      25307 |            15003 |              529 |
| ML-DSA-44 |       1 |      18137 |             6500 |             4138 |
| ML-DSA-44 |       8 |      80284 |            34640 |             4138 |
| ML-DSA-44 |      32 |     312232 |           131120 |             4138 |

Combined parse/verify peaks matched parse-only peaks in this dataset. Final
tracked live bytes were zero after scoped cleanup. Verify-only excludes the
transaction parsed before its scope. C allocation paths, OpenSSL internals,
stack/inline storage, tracker metadata and allocator bookkeeping remain outside
coverage. Timing is instrumented diagnostic data. No RSS, node-wide lower
bound, ledger authorization, fee sufficiency, or production-readiness claim
is made. No ledger network submissions were performed.

See [HANDOFF.md](../../HANDOFF.md) for the one-command reproduction workflow.
The ZIP is intentionally stored as a single file so text formatters do not
rewrite fingerprinted CSV/JSON/fixture bytes inside a recorded snapshot.
