# Scoped native transaction measurements

This executable replays six saved multisigning fixtures through the pinned
fork's actual SerialIter/STTx parser and `checkSign()`. It measures requested
bytes reaching the calibrated global C++ allocation hooks. This is not RSS,
all heap memory, allocator residency/fragmentation, loaded-node concurrency,
or a node-wide lower bound. Protocol sources and the ownership microbenchmark
remain unchanged.

## Flow and boundaries

`loadFixture()` in [Measurement.cpp](Measurement.cpp) checks the hex/JSON
files against configure-time fingerprints, decodes the wire, checks native
round-trip/transaction ID, and prevalidates signatures. Loading streams,
hex/JSON buffers, and prevalidation transactions are destroyed before returning.
The corpus retains only wire buffers and identity strings. It generates no
signing keys or original signed transaction objects.

Main declares the preset set before Rules, which borrows it. Both algorithms
use `featureQuantum` as the only preset in the same candidate build. Common
transaction fields are identical across all six cases. This local configuration
does not establish network activation or ledger authorization.

`measure()` stores STTx inline in a stack `std::optional`, without an allocated
wrapper. Its stages are:

| Stage | Timed operation | Parsed cleanup |
| --- | --- | --- |
| `parse_only` | SerialIter -> STTx | Counted after operation, before scope ends |
| `verify_only` | `checkSign()` on a transaction parsed before the scope | Outside scope |
| `parse_verify` | STTx construction followed by `checkSign()` | Counted after operation, before scope ends |

Each fixture/stage is warmed before retained samples. This is a warm baseline,
not cold start. A failed cold-parse test exposed persistent CountedObject
type-name registration; the exception cleanup test now prevalidates first,
matching the benchmark's initialization policy rather than calling that storage
a leak or subtracting it silently.

One Snapshot records live bytes at operation end; another records the completed
scope after cleanup. Snapshot is a value copy, so later releases cannot change
an earlier result. Checks, CSV/JSON formatting, and output happen outside scopes.

## Measurement contract

- `input_storage=excluded`: retained wires, their capacities, corpus identities,
  loading, and prevalidation are outside accounting. Wire length/capacity are
  separate columns, never added to a tracked peak. All six inputs remain resident.
- Verify-only also has `preparsed_storage=excluded`. A small verification peak
  does not mean the transaction's keys/signatures are absent from memory.
- Coverage is `global_cpp_new_delete_requested_bytes`, attributed to the
  allocating thread's innermost scope. Origin frees on any thread are handled
  by the calibrated tracker. The runner uses one thread.
- Cumulative requests, peak simultaneous live bytes, and operation/scope-end
  live bytes differ. Scope totals include parsed cleanup;
  `cleanup_allocation_calls`/`cleanup_requested_bytes` disclose any new requests
  during that cleanup interval.
- C malloc/OpenSSL internals, bypassing pools/mappings, unscoped workers,
  stack/inline storage, tracker headers/state, padding, and allocator bookkeeping
  are outside attribution. STTx itself and inline PublicKey/crypto stack buffers
  do not contribute their `sizeof` to these counts.
- `elapsed_ns` is an instrumented diagnostic, not production latency/TPS. It
  excludes scope setup, snapshots, parsed cleanup, loading, warm-up, and output;
  allocation-hook overhead inside the operation remains included. Returned
  checker result/error objects are destroyed inside the timed verification block.

Parse-only rows say `signature_result=not_checked_in_scope`: the fixture was
prevalidated before measurement. Other measured rows require a valid native
checker result. Separate tests change Sequence and require rejection within
the measured verify/combined paths. Ledger signer lists, weights/quorum, fee
sufficiency, transaction application, and network submission are not checked.

## Run and validate

Use the [pinned native build](../pq/native/README.md), inside the existing Nix
shell, from the repository root:

```sh
cmake -S src/benchmarks/libxrpl/ballast/pq/native -B build/ballast-pq/native \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/ballast-protocol/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_UNITY_BUILD=ON -DCMAKE_UNITY_BUILD_BATCH_SIZE=15
cmake --build build/ballast-pq/native --parallel 2
ctest --test-dir build/ballast-pq/native --output-on-failure
mkdir -p build/ballast-pq/native/results
build/ballast-pq/native/xrpl.ballast_measure --samples 30 --warmup 3 \
  --run-id run-1 --metadata build/ballast-pq/native/results/run-1.json \
  > build/ballast-pq/native/results/run-1.csv
```

Repeat the last command with `run-2` and `run-3` in all three locations for
fresh processes. Each CSV has 540 rows: six fixtures x three stages x 30 samples.
Warm-up samples are discarded. Stdout is CSV only; errors use stderr and a
nonzero exit code. Counts must be positive integers; `--help` describes options.
A failed run may leave partial output, which validation rejects.

```sh
python3 src/benchmarks/libxrpl/ballast/measurement/validate_csv.py \
  build/ballast-pq/native/results/run-1.csv \
  build/ballast-pq/native/results/run-2.csv \
  build/ballast-pq/native/results/run-3.csv \
  > build/ballast-pq/native/results/summary.json
```

Use Python without `-O`, so validation assertions execute. The stdlib-only
validator checks identities, hashes/IDs, full matrix and sample indices,
scope labels, signature status, nonnegative/balanced counters, and sidecar
consistency when JSON is present. It reports min/median/max and population
standard deviation, and distinct allocation-metric tuples. CTest also runs two
fresh processes and checks CSV quoting with commas/quotes in run IDs.

Rows include source/crypto commits, fixture hashes, compiler/build flags,
OpenSSL identity, rules, stages, warm-up, scope policies, and sample indices.
JSON sidecars repeat provenance and limitations. `measurement_source_sha256`
fingerprints measurement code, validator, counter, and tests;
`dependency_manifest_sha256` fingerprints the native source/fixture manifest.
Keep its `source-sha256.txt` with results. Reconfigure/rebuild before recording
changed sources or assets. The older generic native-build stamp describes the
fixture harness; run JSON/CSV define the measurement contract.

## Actual local results

On 2026-10-25, Nix GCC 15.3.0 / Release / C++23 and OpenSSL 3.6.3 passed
**155/155 CTest cases without skips** in 4.74 seconds. This includes the earlier
129 checks, 18 stage-boundary cases, six invalid-Sequence cases exercising
verify/combined stages, exception cleanup, and the two-process CSV smoke test.
The new `-Wall -Wextra` target exposed signedness warnings in unchanged candidate
headers; those headers were not refactored.

Three fresh runs produced **1620 validated rows**, or 90 samples per group.
All allocation metrics had one distinct tuple in each group across repeats.
Timing varied, as recorded in the summary. All scopes finished with zero
tracked live bytes/blocks, no foreign frees, and no cleanup allocations.

Medians below are **tracked requested C++ bytes**, with input excluded:

| Algorithm | Signers | Parse peak | Live at parse end | Verify peak | Combined peak |
| --- | ---: | ---: | ---: | ---: | ---: |
| secp256k1 | 1 | 3512 | 2872 | 529 | 3512 |
| secp256k1 | 8 | 8430 | 5612 | 529 | 8430 |
| secp256k1 | 32 | 25307 | 15003 | 529 | 25307 |
| ML-DSA-44 | 1 | 18137 | 6500 | 4138 | 18137 |
| ML-DSA-44 | 8 | 80284 | 34640 | 4138 | 80284 |
| ML-DSA-44 | 32 | 312232 | 131120 | 4138 | 312232 |

Trace ML-DSA with 32 signers: its retained 120464-byte wire is outside the scope.
Parsing makes cumulative requests of 877665 bytes, peaks at 312232, and ends
the operation with 131120 tracked bytes live. Destroying the parsed object
returns that to zero. Verify-only makes cumulative requests of 124720 bytes
with a 4138-byte peak, excluding its pre-parsed transaction. Combined requests
total 1002385 bytes and peak at 312232. Allocation call sites were not profiled,
so these totals do not identify the cause of every temporary copy.

CSV/JSON files and the summary live in `build/ballast-pq/native/results/`;
build/test logs have the `measurement-` prefix in its parent. This is a local
baseline for the experimental pinned fork, not production ML-DSA readiness or
node provisioning evidence. Optimizations, protocol/resource changes, fee
proposals, AlphaNet submissions, and node-load experiments remain deferred.
