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

CSV goes to stdout if `--output` is omitted. An output path is overwritten.
Diagnostics go to stderr. Run several message sizes separately, retaining each
CSV. Timing runs should use Release builds on an otherwise quiet machine.

## What is being compared?

The fixture wire format is two adjacent XRPL variable-length fields:

```text
VL(public key) || VL(signature)
```

The signed message is a separate synthetic byte buffer, identical for both
policies. Its bytes are signed directly with empty signature context; the
benchmark does not substitute a digest or use HashML-DSA.

- `owned`: decode a field length, allocate an exact-size byte array, and copy
  that field. Repeat for the second field. Release both after the operation.
- `view`: decode the same lengths and return bounds-checked spans into the
  original immutable buffer. The buffer remains alive throughout verification.

Both paths use the same length decoder, record order, message, key, signature,
verification function, and input-buffer lifetime. Key import occurs on every
verification in both policies. Fixture construction, signing, fixture hashing,
CSV output, and warm-up are outside the timed region. Sample order alternates
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

## Read the CSV correctly

Two stages are reported:

- `parse`: framing, field ownership/view creation, observation, and cleanup.
- `parse_verify`: the same work plus OpenSSL key import and verification.

Each sample reports nanoseconds per operation and operations per second. Compare
sample medians and variation for the same algorithm and stage. A faster parser
may have little effect on complete verification. Do not infer ledger TPS from
these single-process, repeated-fixture numbers.

Memory columns have deliberately narrow meanings:

- `parser_payload_allocations_per_op`: explicit field-array allocations in the
  parser, counted in a separate untimed parse of the same record.
- `parser_copied_bytes_per_op`: bytes passed to those parser `memcpy` calls.
- `extra_owned_payload_bytes_per_record`: simultaneously live field-array
  payload bytes, excluding the original input. This equals copied bytes for
  this implementation, which keeps both fields alive until verification ends.
- `input_bytes`: encoded fields plus the separate message, retained equally
  under both policies.

These are **not** measurements of total allocations, allocator overhead, stack
usage, peak RSS, or OpenSSL's internal memory. The view parser avoids its own
field copies; OpenSSL still imports and processes the key and signature. This
does not claim constant total memory or zero-copy cryptography.

Seeds, exact fixture SHA-256, message size, repetitions, compiler, OpenSSL
version, base repository commit, and implementation-source hash appear in each
row. The fixture SHA-256 covers
`encoded fields || message`. Keys and messages derive deterministically from
the seed; ML-DSA signing is deterministic for reproducible test fixtures.
**These are public, synthetic test keys. Never use them to protect assets.**
Fixtures are generated and verified by the same OpenSSL implementation; this
is not independent cross-library interoperability testing. Timing includes
warm-cache effects from repeatedly verifying the same key and signature.

## Understand the code

1. `Layout.cpp`: read length prefixes, check bounds, and copy or borrow fields.
2. `Crypto.cpp`: generate reproducible fixtures and call existing crypto APIs.
3. `Main.cpp`: warm up, alternate policies, time loops, and write CSV.
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
