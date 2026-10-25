cmake_minimum_required(VERSION 3.24)
project(ballast_pq_interop LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(BALLAST_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(BALLAST_PQ_CANDIDATE "${BALLAST_ROOT}/build/ballast-pq/candidate" CACHE PATH "Pinned XRPL fork")
set(BALLAST_PQ_REFERENCE "${BALLAST_ROOT}/build/ballast-pq/crypto" CACHE PATH "Pinned crypto dependency")
file(READ "${CMAKE_CURRENT_LIST_DIR}/pins.json" pins)
string(JSON candidate_commit GET "${pins}" candidate commit)
string(JSON reference_commit GET "${pins}" reference commit)
string(JSON upstream_commit GET "${pins}" reference upstream_commit)
string(JSON openssl_version GET "${pins}" openssl_version)
find_package(Git REQUIRED)

function(check_checkout directory expected)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${directory}" rev-parse HEAD
        OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
    )
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Wrong source revision: ${directory}: ${actual}; expected ${expected}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${directory}" status --porcelain --untracked-files=no
        OUTPUT_VARIABLE changes COMMAND_ERROR_IS_FATAL ANY
    )
    if(NOT changes STREQUAL "")
        message(FATAL_ERROR "Tracked source edits in ${directory}: ${changes}")
    endif()
endfunction()
check_checkout("${BALLAST_PQ_CANDIDATE}" "${candidate_commit}")
check_checkout("${BALLAST_PQ_REFERENCE}" "${reference_commit}")
file(READ "${BALLAST_PQ_CANDIDATE}/cmake/deps/dilithium.cmake" dependency)
string(FIND "${dependency}" "${reference_commit}" pin_position)
if(pin_position EQUAL -1)
    message(FATAL_ERROR "Candidate no longer pins the selected crypto revision")
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${BALLAST_PQ_REFERENCE}" diff
            "${upstream_commit}" "${reference_commit}" --name-only -- ref
    OUTPUT_VARIABLE ref_changes OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
)
if(NOT ref_changes STREQUAL "ref/Makefile")
    message(FATAL_ERROR "Unexpected reference changes from upstream: ${ref_changes}")
endif()

find_package(OpenSSL ${openssl_version} EXACT REQUIRED)
find_package(GTest REQUIRED)
set(reference_sources)
foreach(source sign packing polyvec poly ntt reduce rounding symmetric-shake fips202)
    list(APPEND reference_sources "${BALLAST_PQ_REFERENCE}/ref/${source}.c")
endforeach()
add_library(ballast.pq_reference STATIC ${reference_sources})
target_include_directories(ballast.pq_reference PUBLIC "${BALLAST_PQ_REFERENCE}/ref")
# The pinned config.h already enables DILITHIUM_RANDOMIZED_SIGNING.
target_compile_definitions(ballast.pq_reference PRIVATE DILITHIUM_MODE=2)
target_compile_options(ballast.pq_reference PRIVATE -fstack-protector -fPIC)

add_executable(xrpl.ballast_pq_interop ../../../../tests/libxrpl/ballast/MlDsaInterop.cpp)
target_link_libraries(xrpl.ballast_pq_interop PRIVATE ballast.pq_reference OpenSSL::Crypto GTest::gtest_main)
target_compile_definitions(
    xrpl.ballast_pq_interop PRIVATE
    BALLAST_PQ_CANDIDATE_COMMIT="${candidate_commit}"
    BALLAST_PQ_REFERENCE_COMMIT="${reference_commit}"
)

# Save the bytes actually selected at configure time, in addition to Git IDs.
file(WRITE "${CMAKE_BINARY_DIR}/source-sha256.txt" "")
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${BALLAST_PQ_REFERENCE}" ls-files ref
    OUTPUT_VARIABLE ref_files OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
)
string(REPLACE "\n" ";" ref_files "${ref_files}")
foreach(source IN LISTS ref_files)
    file(SHA256 "${BALLAST_PQ_REFERENCE}/${source}" hash)
    file(APPEND "${CMAKE_BINARY_DIR}/source-sha256.txt" "${hash}  reference/${source}\n")
endforeach()
foreach(source
    cmake/deps/dilithium.cmake
    include/xrpl/protocol/KeyType.h
    include/xrpl/protocol/PublicKey.h
    include/xrpl/protocol/SecretKey.h
    src/libxrpl/protocol/PublicKey.cpp
    src/libxrpl/protocol/SecretKey.cpp
    src/libxrpl/protocol/STTx.cpp
)
    file(SHA256 "${BALLAST_PQ_CANDIDATE}/${source}" hash)
    file(APPEND "${CMAKE_BINARY_DIR}/source-sha256.txt" "${hash}  candidate/${source}\n")
endforeach()
foreach(source
    src/benchmarks/libxrpl/ballast/pq/CMakeLists.txt
    src/benchmarks/libxrpl/ballast/pq/pins.json
    src/tests/libxrpl/ballast/MlDsaInterop.cpp
)
    file(SHA256 "${BALLAST_ROOT}/${source}" hash)
    file(APPEND "${CMAKE_BINARY_DIR}/source-sha256.txt" "${hash}  harness/${source}\n")
endforeach()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/build-config.txt" CONTENT
"scope=crypto_dependency_interop
configuration=$<CONFIG>
c_compiler=${CMAKE_C_COMPILER}
c_compiler_version=${CMAKE_C_COMPILER_VERSION}
cxx_compiler=${CMAKE_CXX_COMPILER}
cxx_compiler_version=${CMAKE_CXX_COMPILER_VERSION}
c_flags=${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_RELEASE}
cxx_flags=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}
reference_definitions=DILITHIUM_MODE=2;DILITHIUM_RANDOMIZED_SIGNING (config.h)
reference_extra_flags=-fstack-protector -fPIC
openssl_version=${openssl_version}
candidate_compiled=false
memory_measurement=none
")
configure_file(pins.json pins.json COPYONLY)
enable_testing()
include(GoogleTest)
gtest_discover_tests(xrpl.ballast_pq_interop)
