# STBlob move-constructor experiment

The local `include/xrpl/protocol/STBlob.h` now has a move constructor that
transfers its owned Buffer. It preserves the field metadata and increments
the object counter for the new object. The old object remains valid, with an
empty Buffer. Copy construction and copy assignment still make independent
copies; copy assignment is explicitly declared because adding a move
constructor would otherwise delete the implicit copy assignment operator.

This is an implementation change to the actual protocol type. It does not
change transaction bytes, signing payloads, rules, or the cryptographic
implementation. It introduces no borrowed pointer into the wire input.

## Execution flow

Parsing creates each STBlob with an owned Buffer. The parser and template
application store and rearrange fields using STVar. When STVar moves an inline
blob, it calls `STBlob::move()`, which constructs another STBlob from an rvalue.
Previously that construction selected the copy constructor and allocated a
second Buffer. The new move constructor selects Buffer's ownership transfer.

For a concrete 2420-byte signature, the ownership test creates a source blob
and then constructs a destination with `std::move(source)`. The baseline has
two allocations and a 4840-byte peak in that small scope. The optimized build
has one allocation and a 2420-byte peak. The destination keeps the original
address, the source becomes empty, and destruction frees the allocation once.
This is an allocation-level test, separate from the complete transaction test.

## Build both variants

Run inside `nix develop .#gcc`. Reuse the locked Conan toolchain from the
existing baseline setup; see [the native build guide](README.md) if it is absent.

```sh
cmake -S src/benchmarks/libxrpl/ballast/pq/native -B build/ballast-stblob-baseline \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/ballast-protocol/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_UNITY_BUILD=ON -DCMAKE_UNITY_BUILD_BATCH_SIZE=15 \
  -DBALLAST_STBLOB_MOVE_EXPERIMENT=OFF
cmake --build build/ballast-stblob-baseline --parallel 2
ctest --test-dir build/ballast-stblob-baseline --output-on-failure

cmake -S src/benchmarks/libxrpl/ballast/pq/native -B build/ballast-stblob-move \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/ballast-protocol/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_UNITY_BUILD=ON -DCMAKE_UNITY_BUILD_BATCH_SIZE=15 \
  -DBALLAST_STBLOB_MOVE_EXPERIMENT=ON
cmake --build build/ballast-stblob-move --parallel 2
ctest --test-dir build/ballast-stblob-move --output-on-failure
```

OFF uses the pristine pinned candidate header. ON places only the modified
STBlob header ahead of candidate includes. Both variants retain the original
dependency revision and clean-checkout gates. The experimental header hash is
recorded in `source-sha256.txt`, and the compiled dependency-manifest hash
identifies that exact manifest. CSV/JSON build flags include `stblob_move=ON`
or `OFF`. `candidate_commit` identifies the base commit; ON is an experimental
header overlay, not the pristine pinned revision alone.

Follow the [measurement guide](../../measurement/README.md) with the appropriate
build directory to generate three runs per variant. Summarize each variant
separately: the existing validator rejects combining different configurations.

## Measured result

Both configurations passed 164 tests, including the original 155 checks and
nine ownership/copy tests. The protocol subset checks transaction round trips,
transaction IDs, valid signatures, and rejection of invalid signatures. A full
node build, ledger application, production deployment, and RSS were not tested.

Three fresh processes per variant, three discarded warm-ups, and 30 samples
per fixture/stage produced 1620 validated rows per variant. All allocation
counters were stable in each group. The fresh baseline's requested, peak,
and operation-end live byte statistics matched the immutable original baseline.

Parse-only medians for 32 ML-DSA-44 signers:

| Metric                          | Baseline | Move constructor |
| ------------------------------- | -------: | ---------------: |
| Allocation calls                |      598 |              502 |
| Cumulative requested bytes      |   877665 |           680801 |
| Peak live bytes                 |   312232 |           312232 |
| Live bytes at operation end     |   131120 |           131120 |
| Live bytes after scoped cleanup |        0 |                0 |

That is 96 fewer allocations and 196864 fewer requested bytes per parse,
a 22.43% reduction in cumulative requested bytes. Peak and retained memory
did not decrease. Verification-only allocation metrics were unchanged.
Combined parse/verify requests fell from 1002385 to 805521 bytes.

These are intercepted C++ allocation requests with wire input excluded.
Verification additionally excludes the pre-parsed transaction. Stack/inline
storage, C allocation paths, OpenSSL internals, allocator bookkeeping, and
tracker metadata remain outside coverage. No latency, node RSS, node-wide
memory saving, or ledger-authorization claim follows from this experiment.

The [experiment archive](../../../../../../artifacts/ballast/stblob-move-experiment.zip)
contains compressed raw CSVs, sidecars, summaries, comparison data, manifests,
test logs, source files, the validator, fixtures, and a checksummed inventory.
Its README explains offline checking. The original baseline archive and the
dashboard's default saved dataset remain unchanged.
