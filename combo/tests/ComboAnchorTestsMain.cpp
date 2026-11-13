// combo/tests/ComboAnchorTestsMain.cpp — entry point of the `combo_anchor_tests` target: unit tests
// for the launcher's pure co-op logic (combo/net/). No game, no socket, no ROM; runs in CI.
// Exit code 0 = all checks passed.

#include "ComboTestCheck.h"

int main() {
    RunOutboxTests();
    RunDeliveryTests();

    if (ComboTest::gFailures != 0) {
        std::cerr << ComboTest::gFailures << " of " << ComboTest::gChecks << " checks FAILED\n";
        return 1;
    }
    std::cout << "combo_anchor_tests: all " << ComboTest::gChecks << " checks passed\n";
    return 0;
}
