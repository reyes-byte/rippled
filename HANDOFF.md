# Ballast hackathon handoff

Ballast measures the scoped C++ allocation cost of actual XRPL transaction
parsing and signature verification as multisignature keys/signatures grow.
The local baseline is complete. It uses the pinned experimental PQ fork,
real serialization, SerialIter -> STTx parsing, and `checkSign()` for
secp256k1 and ML-DSA-44 at 1, 8, and 32 signers.

The standalone ownership microbenchmark remains a separate experiment.
Protocol changes, optimizations, fee/resource proposals, node-load tests,
and AlphaNet submissions remain deferred.

## Get the work

```sh
git clone --depth 1 --branch hackathon/pqc https://github.com/reyes-byte/rippled.git
cd rippled
```

If you already cloned this fork, switch to `hackathon/pqc` and pull it with
`git pull --ff-only`. Check that your remote points to `reyes-byte/rippled`.

## Check the saved results without building

Python 3.9+ and its standard library are sufficient:

```sh
python3 bin/ballast_bundle.py verify artifacts/ballast/baseline-2026-10-25.zip
```

That snapshot includes three compressed CSVs, JSON sidecars/summary, fixture
bytes, revision pins, source fingerprints, and checksums. Verification checks
the payload and CSV semantics, including transaction IDs and the complete
matrix. It does **not** execute native signature cryptography; the full
workflow below runs those tests. The verifier pins the bundled CSV validator
before executing it. Checksums are integrity checks, not publisher signatures.

The committed ZIP is already portable. To inspect its raw contents:

```sh
python3 -m zipfile -e artifacts/ballast/baseline-2026-10-25.zip /tmp/ballast-results
python3 /tmp/ballast-results/verify.py verify /tmp/ballast-results
```

Existing bundle/archive destinations are never overwritten. CSV compression
is lossless and keeps each committed file below the repository's 400 KB limit.
JSON summaries remain directly readable.

## Reproduce everything with one command

A working Nix installation and network access for initial source/dependency
fetches are needed. The workflow has been developed on Linux x86_64 with the
GCC devshell; other platforms are unverified.

```sh
bash bin/ballast-handoff
```

The launcher enters `nix develop .#gcc` when needed and avoids nesting a shell
if already inside Nix. It then:

1. Reads full revisions from [pins.json](src/benchmarks/libxrpl/ballast/pq/pins.json)
   and creates separate pinned checkouts. Existing mismatched/edited checkouts
   cause a failure; the launcher does not reset them.
2. Installs the locked Conan dependency subset, configures the protocol-source
   build, and builds all calibration/native/measurement executables.
3. Runs CTest, then three fresh measurement processes, with three discarded
   warm-ups and 30 samples for each fixture/stage: 1620 retained rows.
4. Validates CSV/sidecar consistency, summarizes variation, and creates and
   verifies a portable directory and ZIP bundle.

Dependencies/checkouts/builds are cached under `build/ballast-handoff/`.
Every invocation creates a separate timestamped run directory with logs,
raw CSV/JSON, a manifested bundle, and `ballast-handoff.zip`. The last output
lines print their paths. The first dependency/source build can take time.

Options: `--work-dir DIR`, `--samples N`, `--warmup N`, `--runs N`, `--jobs N`.
The defaults use two build jobs. If Nix fails to create a private mount
namespace, use a host/session with working Nix permissions; no native build
was validated outside the required devshell.

## What the numbers mean

The saved baseline passed 155 tests and contains 1620 validated samples.
Allocation metrics were identical across repeats for each fixture/stage;
instrumented timings varied. Examples for 32 signers, in tracked requested
C++ bytes:

| Algorithm | Parse peak | Live after parse | Verify-only peak |
| --- | ---: | ---: | ---: |
| secp256k1 | 25307 | 15003 | 529 |
| ML-DSA-44 | 312232 | 131120 | 4138 |

For ML-DSA-44, a signer signs 50 common bytes plus its 20-byte AccountID even
though the 32-signer wire transaction occupies 120464 bytes. Signers are
assembled into the real Signers array and sorted by binary AccountID.
Native and independent OpenSSL tests verify ML-DSA compatibility for the
selected cases; this is not FIPS certification or production readiness.

The scope boundaries are essential:

- Retained wire/corpus storage is excluded and separately reported.
- Verify-only also excludes the transaction parsed before its scope.
- Parsed objects in parse/combined scopes are destroyed before scope end.
  The CSV distinguishes operation-end live bytes from post-cleanup live bytes.
- Coverage is intercepted global C++ allocation requests. C allocation paths,
  OpenSSL internals, stack/inline objects, tracker metadata, and allocator
  bookkeeping are outside this accounting. This is not node RSS or a
  node-wide lower bound.
- `checkSign()` establishes selected signature validity, not ledger signer
  authorization, quorum, fee sufficiency, or transaction acceptance. Fee 10
  is a fixture constant, not a recommendation. No network submissions occur.

## Handoff validation

The launcher was tested with fresh pinned source checkouts and a fresh CMake
build directory, reusing the existing Nix/Conan package caches. All 155 tests
passed; three new runs produced 1620 rows and matched the recorded allocation
metrics in every group. A machine with empty package caches was not tested.
Directory and ZIP verification passed. Controlled payload corruption and a
substituted validator were rejected. Bash syntax and the repository's Black
hook passed. The committed archive is below the 400 KB added-file limit.

## Where to read next

- [Measurement code and scope contract](src/benchmarks/libxrpl/ballast/measurement/README.md)
- [Saved-results summary](artifacts/ballast/README.md)
- [Native fixtures and signature checks](src/benchmarks/libxrpl/ballast/pq/native/README.md)
- [Allocation accounting calibration](src/benchmarks/libxrpl/ballast/accounting/README.md)
- [Original ownership microbenchmark](src/benchmarks/libxrpl/pqc/README.md)

For the demo: verify the saved bundle, show the table and scope labels, then
walk through `loadFixture()` -> `measure()` -> `writeCsvRow()` in
`src/benchmarks/libxrpl/ballast/measurement/Measurement.cpp`.
