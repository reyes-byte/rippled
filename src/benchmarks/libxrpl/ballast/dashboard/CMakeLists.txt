cmake_minimum_required(VERSION 3.24)
project(ballast_dashboard LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(
    BALLAST_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../.."
    ABSOLUTE
)
find_package(Boost REQUIRED COMPONENTS json)
find_package(GTest REQUIRED)
set(dashboard_data "${BALLAST_ROOT}/artifacts/ballast/dashboard.json")
add_library(ballast.dashboard Dashboard.cpp)
target_include_directories(ballast.dashboard PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(ballast.dashboard PUBLIC Boost::json)
target_compile_options(ballast.dashboard PRIVATE -Wall -Wextra -Wpedantic)
add_executable(ballast_dashboard Main.cpp Terminal.cpp)
target_link_libraries(ballast_dashboard PRIVATE ballast.dashboard)
target_compile_definitions(
    ballast_dashboard
    PRIVATE BALLAST_DASHBOARD_DATA="${dashboard_data}"
)
add_executable(
    ballast_dashboard_tests
    ../../../../tests/libxrpl/ballast/Dashboard.cpp
)
target_link_libraries(
    ballast_dashboard_tests
    PRIVATE ballast.dashboard GTest::gtest_main
)
target_compile_definitions(
    ballast_dashboard_tests
    PRIVATE BALLAST_DASHBOARD_DATA="${dashboard_data}"
)
enable_testing()
include(GoogleTest)
gtest_discover_tests(ballast_dashboard_tests)
