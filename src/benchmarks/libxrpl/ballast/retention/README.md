# Retaining parsed XRPL transactions

This workload keeps 1, 10, 100, or 1000 actual STTx objects alive together.
Each has an independent copy of the saved transaction's wire bytes. The real
SerialIter/STTx parser and `checkSign()` run for every object on one thread.
The two algorithms and 1/8/32 signer fixtures are unchanged.

These are repeated copies of each fixture, with the same transaction ID.
They are not 1000 unique transactions or submissions to an actual node queue.
Queue admission, duplicate handling, ledger authorization, fees, network
acceptance, parallel execution, node RSS, and production capacity are untested.
The workload demonstrates controlled object retention in the protocol subset.

## C++ execution flow and ownership

`measure()` creates a vector of Entry objects. Each Entry owns a Blob containing
wire bytes and an optional STTx. Reserving vector capacity before parsing keeps
existing parsed objects from being relocated as more entries are added.

One allocation scope covers the complete lifetime:

1. Reserve the vector and copy one independent wire buffer per entry.
2. Take the inputs snapshot: container storage plus all wire capacities.
3. Parse each entry, require its input to be consumed, check its transaction ID,
   and run the real signature checker. All earlier entries remain alive.
4. Take the held snapshot with all transactions, wires, and container alive.
5. Destroy the vector and its entries inside the allocation scope.
6. Finish the scope and require balanced allocation/free counts and zero tracked
   live bytes/blocks. Snapshot values remain available after destruction.

Entry declares the wire before the optional transaction, so C++ destroys the
transaction before its wire. The existing STBlob implementation owns its field
buffers; this benchmark introduces no borrowed wire slices.

The source fixture corpus, loading, fixture fingerprint checks, initial
round-trip/signature prevalidation, and shared Rules are outside the scope.
Container storage includes the inline STTx/optional/Blob space. This differs
from the single-operation benchmark, whose optional STTx lives on the stack.
Independent wire copies are included here and explicitly reported separately.

## Run

Inside `nix develop .#gcc`, use the existing locked Conan toolchain and pristine
pinned checkouts from [the native build](../pq/native/README.md):

```sh
cmake -S src/benchmarks/libxrpl/ballast/pq/native -B build/ballast-retention-move \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/ballast-protocol/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_UNITY_BUILD=ON -DCMAKE_UNITY_BUILD_BATCH_SIZE=15 \
  -DBALLAST_STBLOB_MOVE_EXPERIMENT=ON
cmake --build build/ballast-retention-move --parallel 2
ctest --test-dir build/ballast-retention-move --output-on-failure
mkdir -p build/ballast-retention-move/results
build/ballast-retention-move/xrpl.ballast_retention \
  --counts 1,10,100,1000 --samples 1 --warmup 1 --run-id move-1 \
  --metadata build/ballast-retention-move/results/run-1.json \
  > build/ballast-retention-move/results/run-1.csv
```

Repeat the measurement in fresh processes using run-2 and run-3 paths/IDs.
For the baseline, configure a separate `build/ballast-retention-baseline` with
`-DBALLAST_STBLOB_MOVE_EXPERIMENT=OFF`. The overlay and candidate-base semantics
are described in [the move experiment](../pq/native/STBLOB_MOVE.md).
There is a limit of 1000 retained objects per batch. Samples/warm-ups are
positive, with defaults of one each. Warm-ups are discarded.

Validate/summarize each variant separately with Python assertions enabled
(do not use Python `-O`):

```sh
python3 src/benchmarks/libxrpl/ballast/retention/validate_csv.py \
  build/ballast-retention-move/results/run-1.csv \
  build/ballast-retention-move/results/run-2.csv \
  build/ballast-retention-move/results/run-3.csv \
  > build/ballast-retention-move/results/summary.json
```

The retention-specific `retention-source-sha256.txt` fingerprints the original
native dependencies/fixtures, allocation tracker, measurement loader, and the
new retention code/validator/tests. Its compiled hash is in CSV/JSON, alongside
the base/crypto commits, compiler, OpenSSL, variant flags, and retention source
hash. The original single-operation manifest is not appended after compilation.

## Reading the columns

| Column                      | Meaning                                                                                    |
| --------------------------- | ------------------------------------------------------------------------------------------ |
| retained_count              | Objects alive together, each replaying the same fixture                                    |
| container_capacity_bytes    | Vector backing storage, including inline Entry/STTx space                                  |
| wire_payload_bytes          | Sum of actual input lengths                                                                |
| wire_capacity_bytes         | Sum of allocated independent input-buffer capacities                                       |
| inputs_live_bytes           | Container plus independent wire storage, before parsing                                    |
| held_parsed_increment_bytes | Additional tracked live bytes after parsing/checking                                       |
| held_live_bytes             | Container, wires, and parsed heap storage alive together                                   |
| peak_live_bytes             | Largest tracked live-byte count during the complete scope                                  |
| requested_bytes             | Cumulative allocation requests, including input/container setup                            |
| valid_signatures            | Number of transactions whose complete `checkSign()` succeeded, not individual signer count |
| matching_transaction_ids    | Transactions matching the saved fixture's repeated ID                                      |
| final_live_bytes / blocks   | Tracked storage remaining after collection destruction; must be zero                       |
| elapsed_ns                  | Instrumented batch parse/verify time; input setup and cleanup excluded                     |

Coverage remains intercepted C++ requested bytes, not all allocations or RSS.
C allocation paths, OpenSSL internals, allocator bookkeeping/padding, tracker
metadata, and stack storage are excluded. Released tracked bytes do not imply
that the allocator returned pages to the operating system. Timing includes
allocation-hook overhead and is not production throughput or latency.

## Actual results

Both configurations passed 179 tests: the prior 164 plus 14 retention lifetime,
signature-failure and exception/bounds cases, and a two-process CSV smoke test.
Three fresh processes per variant produced 72 rows each (144 total). Each of
the 24 groups per variant has three retained samples after discarded warm-ups.
Every recorded transaction consumed its input, matched the fixture ID, and
passed native signature checking. Allocation counters were stable in each
group; cleanup left zero tracked bytes/blocks, with no foreign frees.

For ML-DSA-44 with 32 signers, both variants measured:

| Retained objects | Wire capacity | Parsed increment | Container | Held live bytes | Peak live bytes |
| ---------------: | ------------: | ---------------: | --------: | --------------: | --------------: |
|                1 |        120464 |           131120 |       152 |          251736 |          432848 |
|               10 |       1204640 |          1311200 |      1520 |         2517360 |         2698472 |
|              100 |      12046400 |         13112000 |     15200 |        25173600 |        25354712 |
|             1000 |     120464000 |        131120000 |    152000 |       251736000 |       251917112 |

The 1000-object held value is about 240.1 MiB of tracked requested storage.
Cumulative requests for that batch were 1123001000 B for the baseline and
926137000 B for the move constructor. Held and peak bytes were identical.
These figures include input/container storage, so their scope differs from
the earlier parse-only 22.43% allocation-traffic reduction.

The [saved retention archive](../../../../../artifacts/ballast/retention-experiment.zip)
contains raw compressed CSVs, sidecars, summaries, source manifests, test logs,
fixture corpus, validator, and implementation sources. Its README describes
offline checks. Earlier immutable archives and dashboard data remain unchanged.
The existing dashboard comparison reads the single-operation experiment;
retention results use a separate schema and are not imported into that view.
