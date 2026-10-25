#include <benchmarks/libxrpl/ballast/accounting/AllocationTracker.h>
#include <gtest/gtest.h>

int
main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    // Exercise active attribution during every existing valid/invalid check.
    // This includes framework/fixture allocations and is NOT a memory sample.
    ballast::allocation::Scope regressionSession;
    int const result = RUN_ALL_TESTS();
    static_cast<void>(regressionSession.finish());
    return result;
}
