cmake_minimum_required(VERSION 3.24)
# cspell:ignore DILITHIUM
project(ballast_pq_native LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()
option(
    BALLAST_STBLOB_MOVE_EXPERIMENT
    "Compile the local STBlob move constructor instead of the pinned header"
    OFF
)

# Reuse the existing pin checks, reference archive, and independent crypto gate.
# This also rejects tracked edits in either isolated source checkout.
add_subdirectory(.. dependency-gate)
set_target_properties(ballast.pq_reference PROPERTIES UNITY_BUILD OFF)
get_filename_component(
    BALLAST_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../.."
    ABSOLUTE
)
set(BALLAST_PQ_CANDIDATE
    "${BALLAST_ROOT}/build/ballast-pq/candidate"
    CACHE PATH
    "Pinned XRPL fork"
)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../pins.json" pins)
string(JSON candidate_commit GET "${pins}" candidate commit)
string(JSON reference_commit GET "${pins}" reference commit)

find_package(
    Boost
    REQUIRED
    COMPONENTS
        chrono
        container
        context
        date_time
        filesystem
        json
        program_options
        regex
        thread
)
find_package(date REQUIRED)
find_package(ed25519 REQUIRED)
find_package(mpt-crypto REQUIRED)
find_package(secp256k1 REQUIRED)
find_package(FastFloat REQUIRED)
find_package(xxHash REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(GTest REQUIRED)
add_subdirectory(
    "${BALLAST_ROOT}/src/benchmarks/libxrpl/ballast/accounting"
    accounting
)

add_library(ballast.native.dependencies INTERFACE)
target_include_directories(
    ballast.native.dependencies
    INTERFACE "${BALLAST_PQ_CANDIDATE}/include" "${BALLAST_PQ_CANDIDATE}/src"
)
target_compile_definitions(
    ballast.native.dependencies
    INTERFACE OPENSSL_SUPPRESS_DEPRECATED DILITHIUM_MODE=2
)
target_link_libraries(
    ballast.native.dependencies
    INTERFACE
        Boost::headers
        Boost::chrono
        Boost::container
        Boost::context
        Boost::date_time
        Boost::filesystem
        Boost::json
        Boost::process
        Boost::program_options
        Boost::regex
        Boost::thread
        OpenSSL::SSL
        OpenSSL::Crypto
        date::date
        ed25519::ed25519
        mpt-crypto::mpt-crypto
        secp256k1::secp256k1
        FastFloat::fast_float
        xxHash::xxhash
        Threads::Threads
        ballast.pq_reference
)
file(WRITE "${CMAKE_BINARY_DIR}/native-sources.txt" "")
# Record the original header in either variant. The opt-in overlay changes only
# STBlob; the pinned dependency checkouts remain clean and keep their gates.
set(stblob_header "${BALLAST_PQ_CANDIDATE}/include/xrpl/protocol/STBlob.h")
file(SHA256 "${stblob_header}" stblob_hash)
file(
    APPEND "${CMAKE_BINARY_DIR}/source-sha256.txt"
    "${stblob_hash}  candidate/include/xrpl/protocol/STBlob.h\n"
)
if(BALLAST_STBLOB_MOVE_EXPERIMENT)
    set(stblob_overlay "${CMAKE_BINARY_DIR}/stblob-overlay")
    configure_file(
        "${BALLAST_ROOT}/include/xrpl/protocol/STBlob.h"
        "${stblob_overlay}/xrpl/protocol/STBlob.h"
        COPYONLY
    )
    target_include_directories(
        ballast.native.dependencies
        BEFORE
        INTERFACE "${stblob_overlay}"
    )
    file(
        SHA256 "${BALLAST_ROOT}/include/xrpl/protocol/STBlob.h"
        stblob_move_hash
    )
    file(
        APPEND "${CMAKE_BINARY_DIR}/source-sha256.txt"
        "${stblob_move_hash}  overlay/include/xrpl/protocol/STBlob.h\n"
    )
endif()
foreach(
    module
    beast
    basics
    json
    crypto
    git
    protocol
)
    file(
        GLOB_RECURSE sources
        CONFIGURE_DEPENDS
        "${BALLAST_PQ_CANDIDATE}/src/libxrpl/${module}/*.cpp"
    )
    # Same functional subset as the existing secp smoke build: archive extraction
    # is excluded; every protocol source and supported field type is retained.
    list(FILTER sources EXCLUDE REGEX "/Archive\\.cpp$")
    add_library(ballast.native.${module} STATIC ${sources})
    target_link_libraries(
        ballast.native.${module}
        PUBLIC ballast.native.dependencies
    )
    foreach(source IN LISTS sources)
        file(APPEND "${CMAKE_BINARY_DIR}/native-sources.txt" "${source}\n")
        file(SHA256 "${source}" hash)
        file(RELATIVE_PATH relative "${BALLAST_PQ_CANDIDATE}" "${source}")
        file(
            APPEND "${CMAKE_BINARY_DIR}/source-sha256.txt"
            "${hash}  candidate/${relative}\n"
        )
    endforeach()
endforeach()
target_link_libraries(ballast.native.basics PUBLIC ballast.native.beast)
target_link_libraries(ballast.native.json PUBLIC ballast.native.basics)
target_link_libraries(ballast.native.crypto PUBLIC ballast.native.basics)
target_link_libraries(
    ballast.native.protocol
    PUBLIC ballast.native.crypto ballast.native.git ballast.native.json
)
target_compile_definitions(
    ballast.native.git
    PRIVATE
        GIT_COMMIT_HASH="${candidate_commit}"
        GIT_BUILD_BRANCH="pinned-detached-candidate"
)

add_executable(
    xrpl.ballast_pq_native
    ../../../../../tests/libxrpl/ballast/PqTransaction.cpp
)
target_link_libraries(
    xrpl.ballast_pq_native
    PRIVATE
        ballast.native.protocol
        ballast.native.crypto
        ballast.native.git
        ballast.native.json
        ballast.native.basics
        ballast.native.beast
        GTest::gtest_main
)
add_executable(
    xrpl.ballast_pq_multisign
    ../../../../../tests/libxrpl/ballast/PqMultisign.cpp
)
target_link_libraries(
    xrpl.ballast_pq_multisign
    PRIVATE
        ballast.native.protocol
        ballast.native.crypto
        ballast.native.git
        ballast.native.json
        ballast.native.basics
        ballast.native.beast
        GTest::gtest_main
)
target_compile_definitions(
    xrpl.ballast_pq_multisign
    PRIVATE
        BALLAST_PQ_CANDIDATE_COMMIT="${candidate_commit}"
        BALLAST_PQ_REFERENCE_COMMIT="${reference_commit}"
        BALLAST_MULTISIGN_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures/multisign"
)
foreach(algorithm secp256k1 ml-dsa-44)
    foreach(count 1 8 32)
        set(matrix_base
            "${CMAKE_CURRENT_LIST_DIR}/fixtures/multisign/${algorithm}-${count}"
        )
        file(READ "${matrix_base}.json" matrix_metadata)
        string(
            JSON expected_matrix_hash
            GET "${matrix_metadata}"
            hex_file_sha256
        )
        file(SHA256 "${matrix_base}.hex" actual_matrix_hash)
        if(NOT actual_matrix_hash STREQUAL expected_matrix_hash)
            message(
                FATAL_ERROR
                "Multisign fixture differs from metadata: ${matrix_base}"
            )
        endif()
        set_property(
            DIRECTORY
            APPEND
            PROPERTY
                CMAKE_CONFIGURE_DEPENDS
                    "${matrix_base}.hex"
                    "${matrix_base}.json"
        )
        foreach(extension hex json)
            file(SHA256 "${matrix_base}.${extension}" hash)
            file(
                RELATIVE_PATH
                relative
                "${BALLAST_ROOT}"
                "${matrix_base}.${extension}"
            )
            file(
                APPEND "${CMAKE_BINARY_DIR}/source-sha256.txt"
                "${hash}  harness/${relative}\n"
            )
        endforeach()
    endforeach()
endforeach()
set(fixture_base "${CMAKE_CURRENT_LIST_DIR}/fixtures/ml-dsa-44-accountset-v1")
file(READ "${fixture_base}.json" fixture_metadata)
string(JSON fixture_id GET "${fixture_metadata}" transaction_id)
string(JSON fixture_wire_hash GET "${fixture_metadata}" wire_sha256)
string(TOUPPER "${fixture_wire_hash}" fixture_wire_hash)
string(JSON fixture_hex_hash GET "${fixture_metadata}" hex_file_sha256)
file(SHA256 "${fixture_base}.hex" actual_hex_hash)
if(NOT actual_hex_hash STREQUAL fixture_hex_hash)
    message(FATAL_ERROR "Saved native fixture hex does not match its metadata")
endif()
set_property(
    DIRECTORY
    APPEND
    PROPERTY
        CMAKE_CONFIGURE_DEPENDS "${fixture_base}.hex" "${fixture_base}.json"
)
target_compile_definitions(
    xrpl.ballast_pq_native
    PRIVATE
        BALLAST_PQ_CANDIDATE_COMMIT="${candidate_commit}"
        BALLAST_PQ_REFERENCE_COMMIT="${reference_commit}"
        BALLAST_PQ_FIXTURE_HEX_PATH="${fixture_base}.hex"
        BALLAST_PQ_FIXTURE_TXID="${fixture_id}"
        BALLAST_PQ_FIXTURE_SHA256="${fixture_wire_hash}"
)
foreach(
    source
    src/benchmarks/libxrpl/ballast/pq/native/CMakeLists.txt
    src/benchmarks/libxrpl/ballast/pq/native/fixtures/ml-dsa-44-accountset-v1.hex
    src/benchmarks/libxrpl/ballast/pq/native/fixtures/ml-dsa-44-accountset-v1.json
    src/tests/libxrpl/ballast/PqTransaction.cpp
    src/tests/libxrpl/ballast/PqMultisign.cpp
    src/benchmarks/libxrpl/ballast/conanfile.txt
    conan.lock
)
    file(SHA256 "${BALLAST_ROOT}/${source}" hash)
    file(
        APPEND "${CMAKE_BINARY_DIR}/source-sha256.txt"
        "${hash}  harness/${source}\n"
    )
endforeach()
file(
    GENERATE OUTPUT "${CMAKE_BINARY_DIR}/native-build-config.txt"
    CONTENT
        "scope=candidate_protocol_source_subset
candidate_commit=${candidate_commit}
reference_commit=${reference_commit}
configuration=$<CONFIG>
cxx_compiler=${CMAKE_CXX_COMPILER}
cxx_compiler_version=${CMAKE_CXX_COMPILER_VERSION}
cxx_flags=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}
cxx_standard=23
unity=${CMAKE_UNITY_BUILD}
unity_batch_size=${CMAKE_UNITY_BUILD_BATCH_SIZE}
candidate_rng=xrpl_cryptoPrng
amendment_presets=featureQuantum_only
ledger_authorization=not_checked
fee_validation=not_checked
memory_measurement=none
"
)
include(GoogleTest)
gtest_discover_tests(xrpl.ballast_pq_native)
gtest_discover_tests(xrpl.ballast_pq_multisign)

# Re-run the same native tests with the replacement allocation functions linked
# into separate executables. Existing plain binaries and protocol objects stay
# intact. TEST_PREFIX avoids duplicate CTest names.
foreach(original xrpl.ballast_pq_native xrpl.ballast_pq_multisign)
    get_target_property(counted_sources ${original} SOURCES)
    get_target_property(counted_definitions ${original} COMPILE_DEFINITIONS)
    add_executable(
        ${original}_counted
        ${counted_sources}
        "${BALLAST_ROOT}/src/benchmarks/libxrpl/ballast/accounting/CountedMain.cpp"
    )
    target_compile_definitions(
        ${original}_counted
        PRIVATE ${counted_definitions}
    )
    target_link_libraries(
        ${original}_counted
        PRIVATE
            ballast.allocation_hooks
            ballast.native.protocol
            ballast.native.crypto
            ballast.native.git
            ballast.native.json
            ballast.native.basics
            ballast.native.beast
            GTest::gtest
    )
    gtest_discover_tests(${original}_counted TEST_PREFIX "Counted.")
endforeach()

add_executable(
    xrpl.ballast_allocation_protocol
    ../../../../../tests/libxrpl/ballast/AllocationProtocol.cpp
)
target_link_libraries(
    xrpl.ballast_allocation_protocol
    PRIVATE
        ballast.allocation_hooks
        ballast.native.protocol
        ballast.native.crypto
        ballast.native.git
        ballast.native.json
        ballast.native.basics
        ballast.native.beast
        GTest::gtest_main
)
gtest_discover_tests(xrpl.ballast_allocation_protocol)
file(
    SHA256 "${BALLAST_ROOT}/src/tests/libxrpl/ballast/AllocationProtocol.cpp"
    allocation_probe_hash
)
file(
    APPEND "${CMAKE_BINARY_DIR}/source-sha256.txt"
    "${allocation_probe_hash}  harness/src/tests/libxrpl/ballast/AllocationProtocol.cpp\n"
)
file(
    SHA256
        "${BALLAST_ROOT}/src/benchmarks/libxrpl/ballast/accounting/CountedMain.cpp"
    counted_main_hash
)
file(
    APPEND "${CMAKE_BINARY_DIR}/source-sha256.txt"
    "${counted_main_hash}  harness/src/benchmarks/libxrpl/ballast/accounting/CountedMain.cpp\n"
)
add_executable(
    xrpl.ballast_blob_move
    ../../../../../tests/libxrpl/ballast/BlobMove.cpp
)
target_compile_definitions(
    xrpl.ballast_blob_move
    PRIVATE BALLAST_EXPECT_STBLOB_MOVE=$<BOOL:${BALLAST_STBLOB_MOVE_EXPERIMENT}>
)
target_link_libraries(
    xrpl.ballast_blob_move
    PRIVATE ballast.allocation_hooks ballast.native.protocol GTest::gtest_main
)
gtest_discover_tests(xrpl.ballast_blob_move)
file(
    SHA256 "${BALLAST_ROOT}/src/tests/libxrpl/ballast/BlobMove.cpp"
    blob_move_test_hash
)
file(
    APPEND "${CMAKE_BINARY_DIR}/source-sha256.txt"
    "${blob_move_test_hash}  harness/src/tests/libxrpl/ballast/BlobMove.cpp\n"
)

add_subdirectory(
    "${BALLAST_ROOT}/src/benchmarks/libxrpl/ballast/measurement"
    measurement
)
add_subdirectory(
    "${BALLAST_ROOT}/src/benchmarks/libxrpl/ballast/retention"
    retention
)
