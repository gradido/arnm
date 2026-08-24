#include "arnm/result.h"

#include <gtest/gtest.h>
#include <set>
#include <string>

// arnm_result_to_string() answers "ARNM_ERROR_UNKNOWN" for anything it has no entry for, which
// is the right answer for a host's own code and the wrong one for arnm's. The table is filled
// with designated initializers, so a value added to the enum without a string beside it leaves
// a hole rather than a compile error -- exactly how ARNM_ERROR_RESOURCE_SIZE_EXCEED came to
// report itself as unknown. These tests walk the range instead of listing the values, so a
// value added in the middle is covered the day it is added, with nothing here to update.

namespace {

/** Last value that belongs to arnm; everything from ARNM_ERROR_USER_BASE up is the host's. */
constexpr int kLastCoreValue = ARNM_ERROR_ENUM_UNKNOWN;

const char *NameOf(int value) {
  return arnm_result_to_string(static_cast<arnm_result>(value));
}

} // namespace

TEST(Result, EveryCoreValueHasAName) {
  // the whole range, not a list of the values that happened to be there when this was written
  for (int value = 0; value <= kLastCoreValue; ++value) {
    const char *name = NameOf(value);
    ASSERT_NE(name, nullptr) << "value " << value;
    EXPECT_STRNE(name, "ARNM_ERROR_UNKNOWN")
        << "arnm_result " << value << " has no entry in the message table of result.c";
  }
}

TEST(Result, NamesAreDistinctAndCarryThePrefix) {
  // A hole is one way the table drifts; a copy-pasted line that names its neighbour is the
  // other, and it cannot be seen by looking for "unknown". Two values answering the same
  // string is always a mistake, so uniqueness catches it without naming a single value here.
  std::set<std::string> seen;
  for (int value = 0; value <= kLastCoreValue; ++value) {
    const std::string name = NameOf(value);
    EXPECT_EQ(name.rfind("ARNM_", 0), 0u) << "value " << value << " answered " << name;
    EXPECT_TRUE(seen.insert(name).second) << "value " << value << " reuses the name " << name;
  }
  EXPECT_EQ(seen.size(), static_cast<size_t>(kLastCoreValue) + 1);
}

TEST(Result, ANameIsTheIdentifierItself) {
  // The convention the table follows, pinned on a handful of values: what comes back is the
  // spelling a reader greps for, not prose. Includes the one the table used to be missing and
  // both of its neighbours, since an off-by-one in a designated initializer would show there.
#define EXPECT_NAMES_ITSELF(value) EXPECT_STREQ(arnm_result_to_string(value), #value)
  EXPECT_NAMES_ITSELF(ARNM_SUCCESS);
  EXPECT_NAMES_ITSELF(ARNM_WARNING_ARENA_MEMORY_NOT_RECLAIMED);
  EXPECT_NAMES_ITSELF(ARNM_ERROR_RESOURCE_EXHAUSTED);
  EXPECT_NAMES_ITSELF(ARNM_ERROR_RESOURCE_SIZE_EXCEED);
  EXPECT_NAMES_ITSELF(ARNM_ERROR_RESOURCE_IN_USE);
  EXPECT_NAMES_ITSELF(ARNM_ERROR_ENUM_UNKNOWN);
#undef EXPECT_NAMES_ITSELF
}

TEST(Result, OutsideTheCoreRangeItSaysUnknown) {
  // The host's own codes are not arnm's to name, and neither is anything past them. Reported
  // rather than read out of bounds -- the range check is what makes the call safe on a value
  // that arrived from somewhere else.
  EXPECT_STREQ(NameOf(ARNM_ERROR_USER_BASE), "ARNM_ERROR_UNKNOWN");
  EXPECT_STREQ(NameOf(ARNM_ERROR_USER_BASE + 7), "ARNM_ERROR_UNKNOWN");
  EXPECT_STREQ(NameOf(ARNM_ERROR_USER_BASE + 500), "ARNM_ERROR_UNKNOWN");
  // A negative value takes the same branch, but arnm_result has no negative enumerator, so
  // casting one is not representable and the test would be buying coverage with UB.
}

TEST(Result, TheRangeTheseTestsWalkIsStillTheWholeRange) {
  // kLastCoreValue is written down, so a value appended after ARNM_ERROR_ENUM_UNKNOWN would
  // fall outside every loop above. This is the tripwire: the moment such a value gets its
  // string, the table grows and this stops answering unknown. Extend kLastCoreValue then --
  // the failure message is the instruction.
  EXPECT_STREQ(NameOf(kLastCoreValue + 1), "ARNM_ERROR_UNKNOWN")
      << "arnm_result gained a value past ARNM_ERROR_ENUM_UNKNOWN; raise kLastCoreValue so the "
         "tests above cover it";
}
