# PQ dependency interoperability checkpoint

This executable checks the candidate XRPL fork's pinned crypto dependency
against the OpenSSL 3.6.3 default provider. It does **not** build that fork's
XRPL wrappers, parse a PQ transaction, call its `STTx::checkSign()`, measure
memory, or connect to a ledger network. The existing secp256k1 transaction
target and standalone ownership benchmark are unchanged.

## Provenance

[pins.json](pins.json) is consumed by CMake, rather than being descriptive
metadata only. It selects:

- [Transia-RnD/rippled](https://github.com/Transia-RnD/rippled/tree/779859c8afa324d34d16e42028af8d2ceb5fcc8e),
  commit `779859c8afa324d34d16e42028af8d2ceb5fcc8e`. Its `dilithium-full`
  branch resolved to this full revision when fetched on 2026-10-25. This is
  the PDF's short commit, in a different repository from this working tree.
- That revision's `cmake/deps/dilithium.cmake` pins
  [Transia-RnD/dilithium](https://github.com/Transia-RnD/dilithium/tree/3032292cfd4d94e0df9bd49a0098669ca9166aa1)
  at `3032292cfd4d94e0df9bd49a0098669ca9166aa1`.
- Its entire tracked `ref/` tree differs from upstream
  `pq-crystals/dilithium` commit `444cdcc84eb36b66fe27b3a2529ee48f6d8150c2`
  only in `ref/Makefile` (archive build targets). CMake verifies the changed
  file list, Git revisions, clean tracked files, and the candidate dependency
  pin at configure time. It saves SHA-256 hashes of every tracked `ref/` file
  in `source-sha256.txt`. Reconfigure before each recorded run; do not edit
  these isolated source checkouts between configuration and execution.

Source inspection confirms that the candidate has `KeyType::Dilithium`, a
1312-byte inline `PublicKey` buffer, a 2560-byte expanded secret key, and
ML signing/verification calls using empty context. `enabledPublicKeyType(slice, rules)`
gates Dilithium on `featureQuantum`. These are source findings, not successful
runtime integration checks. The current root checkout still supports only
secp256k1/Ed25519 and has its original 33-byte inline public-key buffer.
The candidate's `BuildInfo.cpp` declares version string `3.4.0-rc1`; this
identifies source text, not a verified deployed server version.

## Build and run

Use the existing Nix shell and the dependency toolchain generated for the
[secp256k1 checkpoint](../README.md). Enter Nix once; do not nest Nix shells.
The initial clones require network access; all subsequent checks run offline.

```sh
nix develop .#gcc
git clone --no-checkout https://github.com/Transia-RnD/rippled.git build/ballast-pq/candidate
git -C build/ballast-pq/candidate checkout --detach 779859c8afa324d34d16e42028af8d2ceb5fcc8e
git clone --no-checkout https://github.com/Transia-RnD/dilithium.git build/ballast-pq/crypto
git -C build/ballast-pq/crypto checkout --detach 3032292cfd4d94e0df9bd49a0098669ca9166aa1
```

Skip those clone/checkout commands when the pinned checkouts already exist.
From the repository root, inside Nix:

```sh
cmake -S src/benchmarks/libxrpl/ballast/pq -B build/ballast-pq/check \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/ballast-protocol/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/ballast-pq/check --parallel 2
ctest --test-dir build/ballast-pq/check --output-on-failure
build/ballast-pq/check/xrpl.ballast_pq_interop > build/ballast-pq/check/interop.log
```

This compiles the unmodified mode-2 reference C sources listed explicitly in
CMake, with randomized signing enabled by the pinned `config.h`. It uses a
test-only `randombytes` adapter, rather than XRPL's adapter. That adapter
consumes explicit test entropy once for key generation or deterministic
signing; randomized-signing tests fall back to OpenSSL `RAND_bytes`.

## What one input does

In [MlDsaInterop.cpp](../../../../tests/libxrpl/ballast/MlDsaInterop.cpp):

1. `testSeed()` returns the 32 bytes `00 01 ... 1f`. These are public test
   material; never use these keys for funds.
2. `referenceKey()` gives that seed to the pinned reference key generator.
   `opensslKey()` independently imports the same seed. The test compares
   all 1312 public-key bytes and all 2560 expanded private-key bytes.
3. For the 70-byte case, `testMessage(70)` creates `00 01 ... 45`. This is
   a **synthetic message**, not an XRPL multisigning payload. Matching a real
   fixture's length does not make it a transaction.
4. `signReference()` supplies 32 zero bytes as signing randomness.
   `signOpenSSL()` selects deterministic signing. Both retain normal pure
   ML-DSA message encoding and must produce identical 2420-byte signatures.
5. `verifyOpenSSL()` checks the reference signature; `verifyReference()`
   checks the OpenSSL signature. Separate tests change signed bytes, keys,
   signatures, lengths, or context and require rejection by both implementations.

`std::vector<unsigned char>` owns the byte storage. `View` is a `std::span`
that borrows it for a function call; the owning vector stays alive throughout.
The `unique_ptr` aliases release OpenSSL handles automatically when the
function returns. Neither mechanism is a change to XRPL blob ownership.

There are seven tests: deterministic interoperation for messages of 0, 1, 70,
and 4096 bytes; nonempty-context agreement and mismatch rejection; mutation
and truncation rejection; and randomized interoperation. Deterministic cases
emit `BALLAST_PQ_FIXTURE` JSON records with message/key/signature hashes,
lengths, runtime OpenSSL identity, and both source commits. These records
should repeat across fresh runs. Randomized signature bytes are not fixed.

Local validation on 2026-10-25 used GCC 15.3.0, Release, C++23, and OpenSSL
3.6.3. All seven CTest cases passed (0.17 seconds); the existing 14 real
secp256k1 transaction cases also passed (0.25 seconds). Supplying the wrong
reference checkout caused configuration to fail with `Wrong source revision`.
The four deterministic fixture records matched across two fresh processes,
and every saved source hash was checked against disk. The final reference
build had no duplicate-macro warnings. Formatting passed with Nix
clang-format 23.1.0; this is not a claim that all pre-commit hooks ran.

The ignored `build/ballast-pq/check/` directory contains `interop.log`,
`repeat.log`, `fixtures.json` (local fixture/provenance inventory), `pins.json`,
`source-sha256.txt`, `build-config.txt`, and `rejected-pin.log`. The 70-byte
case's deterministic signature SHA-256 is
`f7f45934ffc5a9509bfb989dc764086793e7462afbc118b001a978a1b0749d60`.
This is a cross-implementation regression artifact, not an official NIST
test vector. Running the commands above regenerates the logs and configure
metadata; `fixtures.json` is a saved inventory of this validated local run.

[FIPS 204](https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.204.pdf), Table 2,
defines ML-DSA-44's 1312-byte public key, 2560-byte secret key, and fixed
2420-byte signature. Algorithms 2/3 include the mode byte and context length
even for an empty context. The
[OpenSSL signing documentation](https://docs.openssl.org/3.6/man7/EVP_SIGNATURE-ML-DSA/)
describes normal message encoding and deterministic signing with zero
randomness. This gate keeps that encoding enabled; it does not bypass it to
make older Dilithium formats agree.

Passing these selected cross-implementation checks supplies evidence of
compatibility for the dependency and cases exercised. It is not a NIST
known-answer vector suite, FIPS module certification, a production security
claim, verification of XRPL's RNG/seed wrappers, or proof of ledger
authorization. The subsequent [native transaction checkpoint](native/README.md)
builds the candidate's protocol sources and tests these wrappers on an actual
single-signed transaction; this standalone gate retains its narrower scope.
