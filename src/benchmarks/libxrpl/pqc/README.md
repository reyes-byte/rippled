# Post-quantum signature buffer benchmark

An offline C++ experiment measuring the extra allocations and copies involved
in owning cryptographic fields instead of borrowing them from an input buffer.
Uses real OpenSSL Ed25519 and pure ML-DSA-44 signatures. No server build,
network access at runtime, funds, hardware wallet, or consensus changes.

## Build and run

From the repository root, enter the provided Nix shell:

```sh
nix develop .#gcc-plain
cmake -S src/benchmarks/libxrpl/pqc -B build/pqc-plain -DCMAKE_BUILD_TYPE=Release
cmake --build build/pqc-plain --parallel 2
ctest --test-dir build/pqc-plain --output-on-failure
build/pqc-plain/pqc_bench --output build/pqc-plain/results.csv
```

Run the build and test commands inside that shell. Avoid nesting another
`nix develop` inside a rootless Nix shell, which can fail to create namespaces.

`gcc-plain` is a documented Nix devshell variant. This standalone build uses
Nix's OpenSSL, whose libc must match the compiler. The default deployment
shell targets an older libc and is intended to use matching Conan libraries.
OpenSSL 3.5 or newer is required. Tests use the repository's GoogleTest 1.17.0
version; CMake downloads a pinned commit if no installed GTest is found. To
build the benchmark without that test dependency, pass `-DPQC_BUILD_TESTS=OFF`.

Configuration example:

```sh
build/pqc-plain/pqc_bench --seed 42 --samples 7 \
  --parse-iterations 100000 --verify-iterations 1000 \
  --message-bytes 1024 --output build/pqc-plain/results.csv
```

To exercise 32 signer records per operation, use smaller iteration counts:

```sh
build/pqc-plain/pqc_bench --signers 32 --samples 5 \
  --parse-iterations 10000 --verify-iterations 30 \
  --output build/pqc-plain/signers32.csv
```

`--signers` defaults to 1 and accepts 1 through 32. One operation parses the
entire group, then optionally verifies every signature. All parsed fields stay
alive until the group finishes, so the owned policy holds 64 field payloads
simultaneously at 32 signers. Message size is per signer; the CLI limits the
combined messages to 16 MiB.

CSV goes to stdout if `--output` is omitted. An output path is overwritten.
Diagnostics go to stderr. Run several message sizes separately, retaining each
CSV. Timing runs should use Release builds on an otherwise quiet machine.

## What is being compared?

Each signer record contains two adjacent XRPL variable-length fields. The
records are concatenated into one immutable wire buffer:

```text
[VL(public key) || VL(signature)] repeated --signers times
```

Each signer has a separate synthetic message, identical across policies. Signer
`i` uses `seed + i` modulo 2^32 to generate its key and message. These are
independently signed records, not XRPL multisigning over a common transaction.
Messages are signed directly with empty signature context; the benchmark does
not substitute a digest or use HashML-DSA.

- `owned`: decode a field length, allocate an exact-size byte array, and copy
  that field. Repeat for every field. Release them all after the operation.
- `view`: decode the same lengths and return bounds-checked spans into the
  original immutable buffer. The buffer remains alive throughout verification.
- `shared_view`: decode with the same view parser, retaining a shared owner of
  the immutable buffer in a `SharedFields` wrapper. Copies of the wrapper share
  the buffer; the last owner releases it. Byte edits materialize only the changed
  field, leaving the input and other wrappers unchanged.

All paths use the same length decoder, record order, message, key, signature,
verification function, and input-buffer lifetime. Key import occurs on every
verification in all policies. Fixture construction, signing, fixture hashing,
CSV output, and warm-up are outside the timed region. Setup concatenates the
records, records their offsets, then moves the combined vector into one shared
immutable owner. Every policy uses the same buffer and record boundaries;
length decoding is still performed on every timed parse. Wrapper containers
are allocated once outside timing and reused for every policy. Field allocation,
copying, and release remain timed. The `shared_view` timing includes retaining
and releasing the common wire owner for each signer, but excludes creation of
the initial owner. Sample order rotates
between policies. The program observes parsed fields out of line so parse-only
results cannot simply be discarded; link-time optimization is not enabled.

This is a **small reproduction of a real XRPL ownership pattern**, not an
execution of the complete production parser. In the inspected source:

- `src/libxrpl/protocol/STBlob.cpp`: `STBlob(SerialIter&, ...)` calls
  `getVLBuffer()` to own a field.
- `src/libxrpl/protocol/Serializer.cpp`: `getVLBuffer()` uses
  `getRawHelper<Buffer>()`, which allocates and calls `memcpy`.
- That same file's `getSlice()` returns a bounds-checked borrowed slice.
- `include/xrpl/basics/Slice.h` explains that a slice does not own its memory.

The standalone codec follows the existing 1/2/3-byte VL rules and the 918744-byte
field limit. Tests include independently written known prefix examples at all
boundaries. Ed25519 public-key fields include XRPL's `0xED` prefix, giving 33
bytes; OpenSSL receives the underlying 32-byte key. ML-DSA-44 uses a 1312-byte
public key and a 2420-byte signature.

These records omit STObject field IDs, complete transactions, signing prefixes,
account authorization, and network framing. Successful verification here does
not mean current XRPL nodes accept ML-DSA transactions. Replacing STBlob
ownership globally would require a separate lifetime and mutation audit.
`parseShared()` requires an owner without mutable aliases; the benchmark
constructs a genuinely const buffer. Tests release the caller's owner, copy and
move wrappers, verify signatures, and check final release.
Its optional offset and byte-count arguments select a signer record within the
same owner. Invalid ranges are rejected before creating a view.

## Try lazy materialization

`SharedFields::setSignatureByte()` and `setPublicKeyByte()` demonstrate
bounds-checked, fixed-size edits. For example, given a parsed `original`:

```cpp
auto edited = original; // shares the wire buffer, no field payload copies
edited.setSignatureByte(0, original.views().signature[0] ^ 1);
// Only edited's signature is now materialized; original still verifies.
```

The first edit copies that field into a byte vector. Further edits reuse it
while it has one owner. Copying an edited wrapper shares its materialized
fields too; the next write detaches the changed field again. All exposed views
are read-only. Reacquire views after editing a wrapper, and do not retain them
beyond the lifetime of the storage they reference. Concurrent mutation or
reading and writing of the same wrapper is unsupported.

The original wire buffer remains retained even after fields are materialized.
Edits change the wrapper's views, not the encoded input. No field resizing or
edited-record serialization is implemented. The timed benchmark remains
read-only; mutation behavior is checked by the `pqc_tests` executable:

```sh
build/pqc-plain/pqc_tests \
  --gtest_filter='*signature_edits*:*public_key_edits*:PqcFraming.invalid_edits*'
```

## Read the CSV correctly

Two stages are reported:

- `parse`: framing, field ownership/view creation, observation, and cleanup for
  every signer in the group.
- `parse_verify`: the same work plus OpenSSL key import and verification for
  every signer.

Each sample reports nanoseconds per operation and operations per second. Compare
sample medians and variation for the same algorithm, stage, and signer count.
`ns_per_op` and operations per second describe entire groups, not individual
signatures. A faster parser may have little effect on complete verification.
Do not infer ledger TPS from
these single-process, repeated-fixture numbers.

Memory columns have deliberately narrow meanings:

- `parser_payload_allocations_per_op`: explicit field-array allocations in the
  parser, counted in a separate untimed parse of the same group.
- `parser_copied_bytes_per_op`: bytes passed to those parser `memcpy` calls.
- `extra_owned_payload_bytes_per_workload`: simultaneously live field-array
  payload bytes for the whole group, excluding the original input. This equals
  copied bytes here, because every signer's fields stay live until the group
  finishes. This replaces the earlier `extra_owned_payload_bytes_per_record`
  column.
- `input_bytes`: the combined encoded fields plus all messages, retained equally
  under all policies.

These are **not** measurements of total allocations, allocator overhead, stack
usage, peak RSS, or OpenSSL's internal memory. The view parser avoids its own
field copies; OpenSSL still imports and processes the key and signature. This
does not claim constant total memory or zero-copy cryptography.
Both view policies report zero extra owned field payload bytes. `shared_view`
still retains the entire encoded input, plus wrapper and reference-counting
storage; those costs are not represented by the field-payload columns.
At 32 ML-DSA-44 signers, the owned parser reports 64 payload allocations and
119424 copied bytes (32 times (1312 + 2420)); both view policies report zero.
The immutable encoded input itself occupies 119552 bytes, plus the messages.

Seeds, exact fixture SHA-256, message size, repetitions, compiler, OpenSSL
version, base repository commit, and implementation-source hash appear in each
row. The CSV includes `signer_count`; key, signature, and message sizes remain
per signer, while counters and input bytes describe the whole group. The
fixture SHA-256 covers `encoded fields || message` for each signer in order.
For one signer this preserves the previous fixture fingerprint. Keys and
messages derive deterministically from the seed; ML-DSA signing is deterministic
for reproducible test fixtures.
**These are public, synthetic test keys. Never use them to protect assets.**
Fixtures are generated and verified by the same OpenSSL implementation; this
is not independent cross-library interoperability testing. Timing includes
warm-cache effects from repeatedly verifying the same group of keys and
signatures.

## Understand the code

1. `Layout.cpp`: read length prefixes, check bounds, and copy or borrow fields.
2. `Crypto.cpp`: generate reproducible fixtures and call existing crypto APIs.
3. `Main.cpp`: warm up, rotate policies, time groups, and write CSV.
4. `src/tests/libxrpl/pqc/BufferBench.cpp`: verify framing boundaries, every
   truncation of real fixtures, ownership, reproducibility, and tampering.

No cryptographic primitive is implemented here. Zero-copy parsing is an
established technique; this experiment does not establish novelty or improved
quantum resistance. Its purpose is to quantify a specific migration cost.

## Relevance to the hackathon

This is general Protocol Innovation tooling. It does not integrate a Ledger
device or claim eligibility for Ledger's separate sponsor bounty. Desktop
measurements do not establish performance on a Ledger secure element.

Ledger already documents low-memory ML-DSA and streaming hashing in its SDK.
The proposed contribution here is measurement of XRPL-shaped field ingestion,
not invention of those techniques. Before presenting, report the actual
end-to-end result, even if avoiding copies produces no clear timing gain.

Sources:

- [NIST FIPS 204](https://csrc.nist.gov/pubs/fips/204/final)
- [OpenSSL ML-DSA API](https://docs.openssl.org/3.5/man7/EVP_SIGNATURE-ML-DSA/)
- [Ledger's PQC SDK work](https://www.ledger.com/blog-post-quantum-cryptography-ledger-sdk)
- [XRPL post-quantum proposal, Draft](https://github.com/XRPLF/XRPL-Standards/discussions/295)
- [Prior XRPL readiness tests](https://github.com/XRPLF/rippled/pull/6971)
