// combo/tests/ComboTestCheck.h — the whole test "framework" of combo_anchor_tests: one CHECK macro and
// two counters shared by every test file. Deliberately tiny so the tests add no dependency.
#pragma once

#include <iostream>

namespace ComboTest {
inline int gFailures = 0;
inline int gChecks = 0;
} // namespace ComboTest

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        ComboTest::gChecks++;                                                               \
        if (!(cond)) {                                                                      \
            ComboTest::gFailures++;                                                         \
            std::cerr << "FAILED " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n"; \
        }                                                                                   \
    } while (0)

// One entry point per test file, all run by ComboAnchorTestsMain.cpp.
void RunOutboxTests();
void RunDeliveryTests();
void RunMmFlagTests();
