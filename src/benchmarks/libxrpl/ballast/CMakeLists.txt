cmake_minimum_required(VERSION 3.24)

get_filename_component(BALLAST_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ballast_protocol_smoke LANGUAGES CXX)
    set(BALLAST_BUILD_SCOPE "protocol_source_subset")
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    include("${BALLAST_ROOT}/cmake/XrplVersion.cmake")
    find_package(
        Boost REQUIRED COMPONENTS
        chrono container context date_time filesystem json program_options regex thread
    )
    find_package(OpenSSL REQUIRED)
    find_package(date REQUIRED)
    find_package(ed25519 REQUIRED)
    find_package(mpt-crypto REQUIRED)
    find_package(secp256k1 REQUIRED)
    find_package(FastFloat REQUIRED)
    find_package(xxHash REQUIRED)
    find_package(Threads REQUIRED)

    add_library(ballast.dependencies INTERFACE)
    target_include_directories(
        ballast.dependencies INTERFACE "${BALLAST_ROOT}/include" "${BALLAST_ROOT}/src"
    )
    target_compile_definitions(ballast.dependencies INTERFACE OPENSSL_SUPPRESS_DEPRECATED)
    target_link_libraries(
        ballast.dependencies
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
    )
    file(WRITE "${CMAKE_BINARY_DIR}/ballast-sources.txt" "")
    foreach(module beast basics json crypto git protocol)
        file(
            GLOB_RECURSE sources CONFIGURE_DEPENDS
            "${BALLAST_ROOT}/src/libxrpl/${module}/*.cpp"
        )
        # Archive extraction is outside this functional parser/signature baseline.
        # The real protocol sources, including all supported field types, are intact.
        list(FILTER sources EXCLUDE REGEX "/Archive\\.cpp$")
        add_library(xrpl.libxrpl.${module} STATIC ${sources})
        target_link_libraries(xrpl.libxrpl.${module} PUBLIC ballast.dependencies)
        foreach(source IN LISTS sources)
            file(APPEND "${CMAKE_BINARY_DIR}/ballast-sources.txt" "${source}\n")
        endforeach()
    endforeach()
    target_link_libraries(xrpl.libxrpl.basics PUBLIC xrpl.libxrpl.beast)
    target_link_libraries(xrpl.libxrpl.json PUBLIC xrpl.libxrpl.basics)
    target_link_libraries(xrpl.libxrpl.crypto PUBLIC xrpl.libxrpl.basics)
    target_link_libraries(
        xrpl.libxrpl.protocol PUBLIC xrpl.libxrpl.crypto xrpl.libxrpl.git xrpl.libxrpl.json
    )
    target_compile_definitions(
        xrpl.libxrpl.git PRIVATE
        GIT_COMMIT_HASH="${GIT_COMMIT_HASH}" GIT_BUILD_BRANCH="${GIT_BUILD_BRANCH}"
    )
    set_source_files_properties(
        "${BALLAST_ROOT}/src/libxrpl/protocol/BuildInfo.cpp"
        PROPERTIES COMPILE_DEFINITIONS XRPLD_VERSION="${XRPLD_VERSION}"
    )
else()
    set(BALLAST_BUILD_SCOPE "repository_protocol_modules")
endif()

find_package(GTest REQUIRED)

add_executable(
    xrpl.ballast_smoke
    ../../../tests/libxrpl/ballast/Transaction.cpp
    ../../../tests/libxrpl/ballast/Multisign.cpp
)
# Link the repository's unmodified protocol objects and their lower-level
# modules directly. No server, transaction application, or synthetic codec.
target_link_libraries(
    xrpl.ballast_smoke
    PRIVATE
        xrpl.libxrpl.protocol
        xrpl.libxrpl.crypto
        xrpl.libxrpl.git
        xrpl.libxrpl.json
        xrpl.libxrpl.basics
        xrpl.libxrpl.beast
        GTest::gtest_main
)
set_target_properties(
    xrpl.ballast_smoke
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(COMMAND patch_nix_binary)
    patch_nix_binary(xrpl.ballast_smoke)
endif()
target_compile_definitions(
    xrpl.ballast_smoke
    PRIVATE
        BALLAST_SOURCE_COMMIT="${GIT_COMMIT_HASH}"
        BALLAST_SOURCE_BRANCH="${GIT_BUILD_BRANCH}"
        BALLAST_BUILD_SCOPE="${BALLAST_BUILD_SCOPE}"
)

# This focused suite is available even when the much larger tests=ON build is
# disabled. Run ctest in this subdirectory, or run the executable directly.
enable_testing()
include(GoogleTest)
gtest_discover_tests(xrpl.ballast_smoke)
