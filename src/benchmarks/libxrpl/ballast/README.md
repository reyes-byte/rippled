# Ballast: real transaction baseline

This follows the supplied Ballast PDF as a revised measurement brief. The
existing `../pqc` standalone ownership microbenchmark is preserved. Further
optimization work is paused.

## First runnable milestone

`xrpl.ballast_smoke` is a focused GoogleTest executable using this checkout's
unmodified transaction and crypto code. It does not use the standalone PQC
codec or replace the signature checker.

The fixture uses the public test passphrase
`ballast/secp256k1/accountset/v1`, secp256k1, and a complete single-signed
`AccountSet`: account derived from the signing key, sequence 1, fee 10 drops,
and the fully canonical signature flag. The fee is a fixture constant, not a
fee recommendation. No amendment presets are enabled.

Execution flow:

1. `generateSeed()` and `generateKeyPair()` produce the test key pair.
2. The `STTx` assembler sets the transaction fields; `STTx::sign()` signs them.
3. The real `Serializer` emits complete transaction bytes, including field IDs.
4. `SerialIter` reads those bytes and constructs a new `STTx`.
5. `STTx::checkSign()` selects the single-sign path, reconstructs its signing
   payload, and verifies the secp256k1 signature.

Tests require identical bytes from repeated fixture generation, complete input
consumption, identical reserialization and transaction ID, a valid signature,
and rejection after changing the signed sequence without resigning. A separate
case signs an account different from the public key's derived account and shows
that `checkSign()` still succeeds. That establishes signature validity only:
without a ledger, neither authorization nor lack of authorization is known. A
ledger could authorize a different key as a regular key. Account existence,
master-key disablement, signer quorum, sequence eligibility, reserves, fees,
and transaction application are outside this test.

The executable prints the public test passphrase, base branch/commit, OpenSSL
runtime version, wire length and hex, account, transaction ID, signature length,
and explicit
`ledger_authorization=not_checked memory=not_measured` labels. The base commit
does not identify uncommitted changes; preserve the working-tree diff with a
reproduction. No allocation or timing result is published in this milestone.

## Build and run

For the first functional milestone, build the real protocol sources without
the server's protobuf/gRPC dependencies. From the repository root:

```sh
nix develop .#gcc
conan install src/benchmarks/libxrpl/ballast \
  --output-folder build/ballast-protocol --lockfile conan.lock --build missing \
  --settings build_type=Release --settings:build build_type=Release \
  --conf tools.build:jobs=2 --conf:build tools.build:jobs=2
cmake -S src/benchmarks/libxrpl/ballast -B build/ballast-protocol \
  -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_UNITY_BUILD=ON -DCMAKE_UNITY_BUILD_BATCH_SIZE=15
cmake --build build/ballast-protocol --target xrpl.ballast_smoke --parallel 2
build/ballast-protocol/xrpl.ballast_smoke
ctest --test-dir build/ballast-protocol --output-on-failure
```

Stay inside that Nix shell for the commands after `nix develop`; avoid nesting
rootless Nix shells. Dependency versions/revisions come from the repository
lockfile and the local subset recipe. This functional build compiles source
files from the protocol, crypto, JSON, basics, beast, and git directories,
excluding only the unrelated `basics/Archive.cpp`. The compiled source list is
written to `build/ballast-protocol/ballast-sources.txt`. There are no replacement stubs.
It is labeled `build_scope=protocol_source_subset`, not a production node build.

The same test is also registered with the normal repository build: follow
`BUILD.md`, enable `-Dbenchmark=ON -Dballast=ON`, and build the
`xrpl.ballast_smoke` target. That path links the complete existing protocol and
lower-level modules and is labeled `repository_protocol_modules`. Its broader
dependency setup includes protobuf and gRPC. Report which build was actually
run; do not treat the two configurations as interchangeable measurements.

Verified local checkpoint (2026-10-25): the protocol-source subset build passed
all three tests directly and through CTest, using GCC 15.3.0, C++23, Release,
unity batch size 15, and OpenSSL 3.6.3. The fixture has 151 wire bytes, a
33-byte public key, and a 70-byte signature. Its transaction ID is:

```text
39A8B8A605F55C35F928C651FD0547E1D8844117335A2B9C3D1F9ACA6F6A7F2B
```

These are functional fixture results, not allocation, RSS, or throughput
measurements. The full repository integration build has not been run.
Two independent processes reproduced identical wire bytes and transaction IDs.
This local verification saved `fixture.bin`, `fixture.hex`, `fixture.json`, and
`smoke.log` under `build/ballast-protocol/`. The JSON records the wire SHA-256,
build settings, compiler/runtime versions, and harness/lockfile hashes. The
smoke executable itself prints its fixture; it does not emit a memory CSV.

## Real secp256k1 multisigning

The same executable now also generates complete `AccountSet` transactions with
1, 8, and 32 real multisigners. The original single-sign control remains separate.

The public passphrase namespace is `ballast/secp256k1/multisign/v1`. The owner
uses `/account`; signer `i` uses `/signer/i`, with decimal indices starting at 0.
The same signer keys are reused across counts. Every transaction has sequence 1,
fee 10 drops, and the fully canonical signature flag. The fee remains a fixture
constant; this suite does not establish fee eligibility or ledger authorization.

The top-level `SigningPubKey` is empty and there is no top-level `TxnSignature`.
Each `Signer` object contains its own account ID, public key, and signature.
The entries are sorted by binary account ID, rather than generation order or
Base58 text. Signatures are attached inside the `STTx` constructor callback so
its cached transaction ID includes the finished `Signers` array.

For each signer, the real signing helpers produce:

```text
startMultiSigningData(transaction, TxMultiSign) || signer AccountID (20 bytes)
```

The shared portion contains the signing prefix and transaction fields without
signature fields. In particular, the entire `Signers` array is excluded.
These fixtures have a 50-byte shared portion and a 70-byte per-signer payload
at every count. This payload length is distinct from the DER signature length.
Every signer signs the same transaction with its own account suffix; these are
not the independent synthetic records in the ownership microbenchmark.

Observed native serialization results:

| Multisigners | Complete wire bytes | Total DER signature bytes |
| ---: | ---: | ---: |
| 1 | 181 | 71 |
| 8 | 1108 | 564 |
| 32 | 4283 | 2251 |

All 14 tests passed directly and through CTest: the original three controls,
the three valid multisign counts, changed-sequence rejection at every count,
signer-account relabeling rejection at every count, duplicate rejection, and
ordering rejection. Relabeling tests re-sort the entries before checking, so
they exercise the account-suffix signature binding rather than an order error.
The checker still does not read a ledger signer list, weights, or quorum.

Inside the existing Nix shell:

```sh
cmake --build build/ballast-protocol --target xrpl.ballast_smoke --parallel 2
ctest --test-dir build/ballast-protocol --output-on-failure
build/ballast-protocol/xrpl.ballast_smoke \
  --gtest_filter='SignerCounts/BallastMultisign.deterministic_transaction_roundtrips_and_verifies/*'
```

Local reproduction artifacts are stored under
`build/ballast-protocol/multisign/`: `1`, `8`, and `32` each have `.bin`, `.hex`,
and `.json` files; `fixtures.json` contains the combined inventory. These use
the current harness hashes. The earlier `fixture.json` is the first milestone's
single-sign provenance snapshot. `multisign-smoke.log` records the current full
suite. Wire sizes and signature lengths are fixture properties; this milestone
still measures neither heap allocations nor RSS and makes no throughput claim.

## Corrections and staged measurement plan

Repository facts checked on 2026-10-25:

- The current branch is `hackathon/pqc`, base commit
  `d0e8ed5676b726f462d227262b7b8699edc13553`. This is not the PDF's claimed
  `dilithium-full` / `779859c8` / `v3.4.0-rc1` build. The cited commit is
  absent from this root clone, but the subsequent PQ checkpoint located it
  in the **Transia-RnD/rippled** fork, pinned to full revision
  `779859c8afa324d34d16e42028af8d2ceb5fcc8e`. See [the PQ gate](pq/README.md).
- Here `include/xrpl/protocol/PublicKey.h` declares a 33-byte internal buffer;
  `KeyType.h` supports secp256k1 and Ed25519. The PDF's 1312-byte key-storage
  description does not describe this checkout. It does match the pinned
  candidate fork's inline buffer declaration, including for secp256k1 key
  objects; that fork's different object layout must be disclosed in comparisons.
- [XLS-295](https://github.com/XRPLF/XRPL-Standards/discussions/295) is a draft
  proposal, not evidence of activation on a particular network.
  [Ripple's roadmap](https://ripple.com/insights/post-quantum-readiness-on-the-xrp-ledger/)
  describes ML-DSA on AlphaNet as proof-of-concept work. This does not verify
  today's endpoint, operators, validator count, faucet, deployed revision, or
  supported transaction formats. Those operational PDF claims remain
  unverified. This milestone contacts no ledger network and submits nothing.

Completed functional milestones are the single-sign control, actual
secp256k1 multisigning at 1, 8, and 32 signers, the pinned crypto dependency
gate, and the [native single-sign and multisigning matrix](pq/native/README.md).
The matrix now exercises both algorithms in the same candidate build, with
six saved and independently fingerprinted wire fixtures. The root-checkout
secp controls remain separate and byte-identical to the candidate secp cases.
Selected compatibility tests are not certification or a security audit.
The subsequent [allocation calibration](accounting/README.md) now validates
the selected C++ new/delete accounting, lifetime/thread boundaries, and actual
single-field parser interception. The original fixture binaries remain without
the hook; separate counted copies pass the same transaction checks with
attribution active. This does not establish all-allocator or node RSS coverage.
The [local measurement baseline](measurement/README.md) now exercises parse-only,
verify-only, and combined scopes over the saved matrix, with input excluded,
stage warm-up, signature outcome checks, and explicit parsed-object cleanup.
It emits fingerprinted CSV/JSON results and an independently validated summary.
Three fresh runs produced 1620 rows; all 155 tests pass. This completes the
staged local baseline, within the limited allocator coverage described below.
Broader allocator coverage, more workloads, and node concurrency remain future
work; these results do not support node provisioning or fee recommendations.

The memory contract used by this baseline is:

- Scoped tracked heap bytes are not node RSS, allocator residency, queue size,
  open-ledger memory, or a validator provisioning estimate.
- Global C++ `new`/`delete` hooks do not automatically cover C allocation,
  OpenSSL internals, custom pools, direct mappings, stack/inline storage,
  other threads, or allocations outside the scope. Requested sizes do not
  include all allocator bookkeeping or fragmentation. Total/count metrics
  alone do not measure fragmentation.
- Start with fixture generation and retained input storage outside the scope,
  explicitly marked `input_storage=excluded`. Any later included-input variant
  must allocate/retain input within its stated accounting boundary and report
  capacity separately from serialized length. Never silently add wire length
  to a scoped counter and call it measured resident memory.
- Destroy fixture-generating transaction/key objects before opening a measured
  scope. Retain only the stated input corpus and metadata; the current smoke
  test deliberately retains an original transaction for round-trip checks and
  is not itself a memory benchmark.
- Separate parse-only, verify-only, and combined parse/verify stages. Define
  initialization, object lifetimes, and destruction relative to each scope.
  Label any heap-to-wire ratio with its coverage and input-storage policy;
  a value of 1 does not mean that a node holds exactly its wire input.
- Do not call this path a node-wide lower bound. Queues, caches, sharing,
  lifetimes, and concurrent work require separate evidence.

AlphaNet submissions, protocol changes, right-sizing/interning/arena/hash
optimizations, and node-load experiments remain deferred. Resource-limit and
fee recommendations are future proposals informed by validated measurements,
not conclusions of this baseline.
