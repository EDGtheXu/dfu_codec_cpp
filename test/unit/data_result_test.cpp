// Tests for DataResult (success / failure / partial values) and Lifecycle.
#include "test_support.hpp"

namespace {

using codec::DataResult;
using codec::ErrorHandler;
using codec::Lifecycle;

std::string messageOf(const DataResult<int>& result) { return result.message(); }

TEST(LifecycleTest, CombinationRules) {
  const Lifecycle stable = Lifecycle::stable();
  const Lifecycle experimental = Lifecycle::experimental();
  const Lifecycle deprecated5 = Lifecycle::deprecated(5);
  const Lifecycle deprecated3 = Lifecycle::deprecated(3);

  EXPECT_TRUE(stable.add(stable).isStable());
  EXPECT_TRUE(stable.add(experimental).isExperimental());
  EXPECT_TRUE(experimental.add(stable).isExperimental());
  EXPECT_TRUE(experimental.add(deprecated5).isExperimental());
  EXPECT_EQ(stable.add(deprecated5), deprecated5);
  EXPECT_EQ(deprecated5.add(stable), deprecated5);
  // The lowest "since" wins.
  EXPECT_EQ(deprecated5.add(deprecated3), deprecated3);
  EXPECT_EQ(deprecated3.add(deprecated5), deprecated3);
}

TEST(LifecycleTest, ToString) {
  EXPECT_EQ(Lifecycle::stable().toString(), "Stable");
  EXPECT_EQ(Lifecycle::experimental().toString(), "Experimental");
  EXPECT_EQ(Lifecycle::deprecated(7).toString(), "Deprecated[7]");
}

TEST(DataResultTest, SuccessCarriesAValueAndExperimentalLifecycleByDefault) {
  const DataResult<int> result = DataResult<int>::success(42);
  EXPECT_TRUE(result.isSuccess());
  EXPECT_FALSE(result.isError());
  EXPECT_TRUE(result.result().has_value());
  EXPECT_EQ(*result.result(), 42);
  EXPECT_GE(result.valueOrPartial().value(), 0);
  EXPECT_TRUE(result.lifecycle().isExperimental());
  EXPECT_FALSE(result.hasPartial());
}

TEST(DataResultTest, ErrorFactories) {
  const DataResult<int> plain = DataResult<int>::error("boom");
  EXPECT_TRUE(plain.isError());
  EXPECT_EQ(plain.message(), "boom");
  EXPECT_FALSE(plain.result().has_value());
  EXPECT_FALSE(plain.hasValue());

  const DataResult<int> withPartial = DataResult<int>::error("boom", 7);
  EXPECT_TRUE(withPartial.isError());
  EXPECT_EQ(withPartial.message(), "boom");
  EXPECT_FALSE(withPartial.result().has_value());
  EXPECT_TRUE(withPartial.hasPartial());
  EXPECT_EQ(*withPartial.valueOrPartial(), 7);
}

TEST(DataResultTest, MapTransformsTheValueAndThePartial) {
  const DataResult<int> success = DataResult<int>::success(2);
  const DataResult<int> doubled = success.map([](const int& value) { return value * 2; });
  EXPECT_EQ(*doubled.result(), 4);

  const DataResult<int> failure = DataResult<int>::error("bad", 3);
  const DataResult<int> mapped = failure.map([](const int& value) { return value + 10; });
  EXPECT_TRUE(mapped.isError());
  EXPECT_EQ(mapped.message(), "bad");
  EXPECT_EQ(*mapped.valueOrPartial(), 13);
}

TEST(DataResultTest, FlatMapSuccessPath) {
  const DataResult<int> success = DataResult<int>::success(2, Lifecycle::stable());
  const DataResult<int> chained = success.flatMap([](const int& value) {
    return DataResult<int>::success(value * 3, Lifecycle::stable());
  });
  EXPECT_TRUE(chained.isSuccess());
  EXPECT_EQ(*chained.result(), 6);
  EXPECT_TRUE(chained.lifecycle().isStable());

  const DataResult<int> becomesError = success.flatMap([](const int&) {
    return DataResult<int>::error("second failed");
  });
  EXPECT_TRUE(becomesError.isError());
  EXPECT_EQ(becomesError.message(), "second failed");
}

TEST(DataResultTest, FlatMapOnFailureWithPartialFeedsTheFunction) {
  const DataResult<int> failure = DataResult<int>::error("first failed", 4);
  const DataResult<int> recovers =
      failure.flatMap([](const int& value) { return DataResult<int>::success(value * 5); });
  EXPECT_TRUE(recovers.isError());
  EXPECT_EQ(recovers.message(), "first failed");
  EXPECT_EQ(*recovers.valueOrPartial(), 20);  // partial survives

  const DataResult<int> compounds = failure.flatMap([](const int&) {
    return DataResult<int>::error("second failed");
  });
  EXPECT_TRUE(compounds.isError());
  EXPECT_EQ(compounds.message(), "first failed; second failed");
}

TEST(DataResultTest, FlatMapOnFailureWithoutPartialIsUnchanged) {
  const DataResult<int> failure =
      DataResult<int>::errorNoPartial("only message", Lifecycle::stable());
  const DataResult<int> chained =
      failure.flatMap([](const int& value) { return DataResult<int>::success(value); });
  EXPECT_TRUE(chained.isError());
  EXPECT_EQ(chained.message(), "only message");
  EXPECT_TRUE(chained.lifecycle().isStable());
  EXPECT_FALSE(chained.hasValue());
}

TEST(DataResultTest, Apply2FollowsTheApplicativeRules) {
  const DataResult<int> good = DataResult<int>::success(1, Lifecycle::stable());
  const DataResult<int> alsoGood = DataResult<int>::success(2, Lifecycle::stable());
  const auto add = [](const int& a, const int& b) { return a + b; };

  const DataResult<int> sum = good.apply2(add, alsoGood);
  EXPECT_TRUE(sum.isSuccess());
  EXPECT_EQ(*sum.result(), 3);
  // apply2 points the function at an experimental lifecycle, so the result is
  // never stable (DFU behaviour).
  EXPECT_TRUE(sum.lifecycle().isExperimental());

  const DataResult<int> stableSum = good.apply2stable(add, alsoGood);
  EXPECT_TRUE(stableSum.lifecycle().isStable());
  EXPECT_EQ(*stableSum.result(), 3);
}

TEST(DataResultTest, Apply2JoinsMessagesAndCombinesPartials) {
  const auto add = [](const int& a, const int& b) { return a + b; };

  const DataResult<int> left = DataResult<int>::error("left", 1);
  const DataResult<int> right = DataResult<int>::error("right", 2);
  const DataResult<int> both = left.apply2(add, right);
  EXPECT_TRUE(both.isError());
  EXPECT_EQ(both.message(), "left; right");
  EXPECT_EQ(*both.valueOrPartial(), 3);

  const DataResult<int> onlyMessage = DataResult<int>::error("no partial");
  const DataResult<int> noPartial = onlyMessage.apply2(add, right);
  EXPECT_EQ(noPartial.message(), "no partial; right");
  EXPECT_FALSE(noPartial.hasValue());

  const DataResult<int> good = DataResult<int>::success(1);
  const DataResult<int> onePartial = good.apply2(add, right);
  EXPECT_EQ(onePartial.message(), "right");
  EXPECT_EQ(*onePartial.valueOrPartial(), 3);
}

TEST(DataResultTest, Apply3CombinesThreeResults) {
  const DataResult<int> a = DataResult<int>::success(1);
  const DataResult<int> b = DataResult<int>::success(2);
  const DataResult<int> c = DataResult<int>::success(3);
  const DataResult<int> sum = a.apply3(
      [](const int& x, const int& y, const int& z) { return x + y + z; }, b, c);
  EXPECT_EQ(*sum.result(), 6);

  const DataResult<int> broken = DataResult<int>::error("c failed");
  const DataResult<int> partial = a.apply3(
      [](const int& x, const int& y, const int& z) { return x + y + z; }, b, broken);
  EXPECT_EQ(partial.message(), "c failed");
  // A failure without a partial value leaves nothing to apply the function to,
  // so the combined failure has no partial either (DFU's Applicative chain).
  EXPECT_FALSE(partial.hasValue());
}

TEST(DataResultTest, SetPartialOnlyAffectsFailures) {
  const DataResult<int> failure = DataResult<int>::error("nope");
  EXPECT_EQ(*failure.setPartial(9).valueOrPartial(), 9);
  const DataResult<int> success = DataResult<int>::success(1);
  EXPECT_EQ(*success.setPartial(9).valueOrPartial(), 1);
}

TEST(DataResultTest, MapErrorAndAddLifecycle) {
  const DataResult<int> failure = DataResult<int>::error("nope", 5);
  const DataResult<int> mapped =
      failure.mapError([](const std::string& message) { return "wrapped: " + message; });
  EXPECT_EQ(mapped.message(), "wrapped: nope");
  EXPECT_EQ(*mapped.valueOrPartial(), 5);

  // Lifecycle.add: experimental always wins, deprecated only survives when the
  // other side is stable or a newer deprecation.
  EXPECT_TRUE(failure.lifecycle().isExperimental());
  EXPECT_TRUE(failure.addLifecycle(Lifecycle::deprecated(4)).lifecycle().isExperimental());

  const DataResult<int> stable = DataResult<int>::errorNoPartial("nope", Lifecycle::stable());
  const DataResult<int> deprecated = stable.addLifecycle(Lifecycle::deprecated(4));
  EXPECT_TRUE(deprecated.lifecycle().isDeprecated());
  EXPECT_EQ(deprecated.lifecycle().since(), 4);
  EXPECT_TRUE(deprecated.setLifecycle(Lifecycle::stable()).lifecycle().isStable());
}

TEST(DataResultTest, ResultOrPartialAndGetOrThrow) {
  const DataResult<int> failure = DataResult<int>::error("nope", 5);
  std::string reported;
  const std::optional<int> partial =
      failure.resultOrPartial([&](const std::string& message) { reported = message; });
  EXPECT_EQ(reported, "nope");
  ASSERT_TRUE(partial.has_value());
  EXPECT_EQ(*partial, 5);

  EXPECT_EQ(failure.getOrThrow(true, [](const std::string&) {}), 5);
  EXPECT_THROW(failure.getOrThrow(false, [](const std::string&) {}), std::runtime_error);

  const DataResult<int> withoutPartial = DataResult<int>::error("nope");
  EXPECT_THROW(withoutPartial.getOrThrow(true, [](const std::string&) {}), std::runtime_error);
  EXPECT_EQ(DataResult<int>::success(3).getOrThrow(false, [](const std::string&) {}), 3);
}

TEST(DataResultTest, PromotePartialTurnsPartialFailuresIntoSuccesses) {
  const DataResult<int> failure = DataResult<int>::error("nope", 5, Lifecycle::stable());
  std::string reported;
  const DataResult<int> promoted =
      failure.promotePartial([&](const std::string& message) { reported = message; });
  EXPECT_EQ(reported, "nope");
  EXPECT_TRUE(promoted.isSuccess());
  EXPECT_EQ(*promoted.result(), 5);
  EXPECT_TRUE(promoted.lifecycle().isStable());

  const DataResult<int> withoutPartial = DataResult<int>::error("nope");
  EXPECT_TRUE(withoutPartial.promotePartial([](const std::string&) {}).isError());
}

TEST(DataResultTest, BaseViewSeesThroughTheTypeParameter) {
  const DataResult<int> failure = DataResult<int>::error("nope", 5);
  const codec::DataResultBase& base = failure;
  EXPECT_TRUE(base.isError());
  EXPECT_TRUE(base.hasValue());
  EXPECT_EQ(base.message(), "nope");

  const DataResult<std::string> success = DataResult<std::string>::success("x");
  const codec::DataResultBase& successBase = success;
  EXPECT_TRUE(successBase.isSuccess());
  EXPECT_EQ(messageOf(failure), "nope");
}

}  // namespace
