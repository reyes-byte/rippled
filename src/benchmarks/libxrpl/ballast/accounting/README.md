# C++ allocation accounting calibration

This checkpoint validates a scoped counter before using it for transaction
measurements. It reports **successful requested bytes reaching the executable's
replacement global C++ new/delete functions**. It is not RSS, allocator
residency, all heap memory, fragmentation, a node-wide lower bound, or a
validator sizing estimate. The ownership microbenchmark is unchanged.

## Execution flow and lifetime

`Scope` installs a thread-local attribution state. Each intercepted allocation
reserves a header and aligned payload using C `malloc`; the header records
the original pointer, requested size, and origin state. It increments the
origin's counters and reference count. No C++ container, stream, or heap map
is used inside a hook.

On delete, the header identifies the origin even if the block is freed after
`finish()`, in another scope, or on a worker thread. Per-origin counters use
a short atomic spin lock. The state has one reference from the Scope object
and one per outstanding allocation. It is freed after the last reference,
so a late free never reads a destroyed Scope object.

`finish()` stops attribution and returns a Snapshot **by value**. Printing and
test assertions happen afterward, so their allocations do not pollute that
result. A still-live Scope can take another snapshot to observe subsequent
frees; the earlier returned Snapshot remains unchanged. Destruction also
finishes a scope. Scopes cannot move/copy and must finish in reverse nesting
order on their creating thread; violations abort.

Nested scopes use exclusive attribution to the innermost scope. Other threads
do not inherit a scope. Frees update the allocation's origin on any thread;
`nonOwnedFrees` separately records blocks freed by the current scope that it
did not allocate. This prevents preexisting input or cross-scope frees from
making its own live-byte counter underflow.

## Metrics and a concrete trace

| Metric | Meaning |
| --- | --- |
| allocations / requestedBytes | Successful intercepted allocations and cumulative requested sizes |
| frees / freedBytes | Releases of allocations originating in this scope, including later releases |
| liveBlocks / liveBytes | Origin-owned blocks/bytes not yet released at snapshot time |
| peakLiveBytes | Largest simultaneous live-byte count for this origin |
| nonOwnedFrees / nonOwnedFreedBytes | Observed frees during this scope of blocks attributed elsewhere or outside a scope |

The calibration allocates 64 and 128 bytes, frees the 64-byte block, allocates
32 bytes, then frees the remaining blocks. Cumulative requests are **224**,
peak live bytes are **192**, and final live bytes are **0**. The 32-byte
request raises cumulative bytes without exceeding the previous peak.

Zero-byte allocations count as one block and zero requested bytes; internally
at least one byte is reserved. Null deletes do nothing. Failed allocation
attempts are not counted as successes. The throwing/nothrow paths preserve
new-handler behavior. Eight allocation and twelve release entry points cover
scalar/array, aligned, sized-delete, and nothrow forms. These correspond to
the [C++ allocation/deallocation forms](https://eel.is/c++draft/new.delete).

## Coverage checks

`AllocationAccounting.cpp` exercises known sizes, alignment, peak/lifetime
boundaries, scope nesting, input created outside the scope, cross-thread
and concurrent frees, worker-allocation exclusion, metadata retained after
Scope destruction, zero sizes, failures, and new-handler invocation.
`Probes.cpp` is compiled separately without unity or LTO so allocation and
release calls cannot be optimized together across the test/probe boundary.
The counter observes calls that actually occur;
[C++ permits allocation elision](https://eel.is/c++draft/expr.new), so it
does not count hypothetical allocations removed by the compiler.

Controlled 96-byte C `malloc` and `OPENSSL_malloc` probes are deliberately
not intercepted. This verifies an exclusion for those calls, not complete
knowledge of every OpenSSL path. Direct C allocations, custom pools/mappings,
stack/inline storage, non-forwarding class allocators, worker allocations
without a scope, and requests outside the scope are outside attribution.

The native build adds `AllocationProtocol.cpp`: after warming bookkeeping,
actual `SerialIter -> STBlob -> Buffer` parsing is checked for 1312-byte and
2420-byte fields. Input buffers and warm-up are outside the scope. These are
controlled single-field calibration inputs, **not complete transactions or
per-transaction memory measurements**.

Separate `_counted` copies of the existing native single-sign and multisign
test executables link the hook, while the original binaries do not. The same
valid/invalid signatures, amendment gates, and saved fixtures are checked
with interception enabled. This establishes behavior for this build and
observed paths, rather than assuming symbol interposition covers every allocator.
`CountedMain.cpp` keeps attribution active during those tests. It deliberately
does not publish that session's totals, which include GTest and fixture setup.

## Instrumentation costs

The header, alignment padding, per-scope state, and actual malloc bookkeeping
are excluded from requested-byte metrics. The header is present even for
allocations outside a scope so later deletes remain compatible. This changes
the sizes passed to malloc and adds locking/reference-count overhead: use
these results for scoped requested-byte accounting, not allocator performance,
fragmentation, or instrumented RSS comparisons. An excluded wire buffer must
remain explicitly labeled `input_storage=excluded`; do not add its length to
the counter and rename the sum RSS.

## Run

Use the existing Nix shell and cached Ballast Conan toolchain. Standalone
calibration needs no candidate protocol build:

```sh
nix develop .#gcc
cmake -S src/benchmarks/libxrpl/ballast/accounting -B build/ballast-accounting \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/ballast-protocol/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/ballast-accounting --parallel 2
ctest --test-dir build/ballast-accounting --output-on-failure
```

For the actual parser probes and counted transaction regression copies, use
the [native build](../pq/native/README.md). Build all its targets, then run
CTest. No protocol source, ledger state, or network deployment is changed.

`accounting-source-sha256.txt` identifies the hooks/probes/calibration source;
`accounting-build-config.txt` records compiler and flags. In the native build
these files are under `build/ballast-pq/native/accounting/` alongside the
calibration and protocol logs. The parent `source-sha256.txt` separately
identifies the candidate protocol sources and transaction fixture harness.

## Actual validation

On 2026-10-25, the Nix GCC 15.3.0 / Release / C++23 build with OpenSSL 3.6.3
passed **129/129 CTest cases without skips** in 3.44 seconds: 53 existing
functional cases, 46 native transaction checks with attribution active, 28
calibration cases, and two actual STBlob field probes. The direct calibration
suite also passed. Both field sizes produced one allocation, requested/peak
bytes equal to 1312 or 2420, and zero final live bytes, with input excluded.

All 28 calibration tests passed under AddressSanitizer and
UndefinedBehaviorSanitizer with leak detection enabled and no reported errors.
That sanitizer build covers the counter, probes, and calibration tests; the
protocol and prebuilt dependencies were not rebuilt with sanitizers. No
ThreadSanitizer or full server build was run.

`nm -C` found all 20 strong allocation/release replacements in the two counted
transaction binaries and the actual-field probe binary, and none in the
original transaction binaries. Source manifests matched disk, including the
sanitized counter/calibration sources. Nix clang-format and `git diff --check`
passed; the full pre-commit suite was not run. Both pinned checkouts remained
clean and saved fixtures were preserved.

Local results are in `build/ballast-pq/native/accounting/`: `calibration-final.log`,
`protocol.log`, source/build metadata, and `accounting-validation.json`.
The parent directory has `accounting-build-active.log` and
`accounting-ctest-active.log`. Sanitized output and build metadata are in
`build/ballast-accounting-sanitized/`. The observed header size is 32 bytes
on this build, excluded along with alignment padding and scope state.

This completes selected accounting calibration. The subsequent
[measurement milestone](../measurement/README.md) implements parse-only,
verify-only, and combined measurements over the saved transaction matrix,
with warm-up and explicit input boundaries. Its CSVs retain this counter's
limited coverage; they do not expand the measurements to RSS or every allocator.
