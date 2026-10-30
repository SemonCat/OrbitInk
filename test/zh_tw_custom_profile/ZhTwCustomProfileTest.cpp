#include <gtest/gtest.h>

#include "CustomZhTwProfile.h"

TEST(ZhTwCustomProfile, RequiresEveryPackagedFontSizeBeforeMigration) {
  EXPECT_TRUE(CustomZhTwProfile::assetsComplete(CustomZhTwProfile::REQUIRED_SIZE_MASK));
  EXPECT_FALSE(CustomZhTwProfile::assetsComplete(CustomZhTwProfile::REQUIRED_SIZE_MASK & ~(1u << 14)));
  EXPECT_FALSE(CustomZhTwProfile::assetsComplete(CustomZhTwProfile::REQUIRED_SIZE_MASK & ~(1u << 28)));
}

TEST(ZhTwCustomProfile, MigratesAnExistingInstallExactlyOnce) {
  EXPECT_TRUE(CustomZhTwProfile::shouldApplyDefaults(false, CustomZhTwProfile::REQUIRED_SIZE_MASK));
  EXPECT_FALSE(CustomZhTwProfile::shouldApplyDefaults(true, CustomZhTwProfile::REQUIRED_SIZE_MASK));
  EXPECT_FALSE(CustomZhTwProfile::shouldApplyDefaults(false, 0));
}

TEST(ZhTwCustomProfile, AppliesPerformanceDefaultsOnceAfterFontProfile) {
  EXPECT_TRUE(CustomZhTwProfile::shouldApplyPerformanceDefaults(true, false));
  EXPECT_FALSE(CustomZhTwProfile::shouldApplyPerformanceDefaults(false, false));
  EXPECT_FALSE(CustomZhTwProfile::shouldApplyPerformanceDefaults(true, true));
}

TEST(ZhTwCustomProfile, UsesResponsiveX3Defaults) {
  EXPECT_FALSE(CustomZhTwProfile::DEFAULT_TEXT_ANTI_ALIASING);
  EXPECT_FALSE(CustomZhTwProfile::IDLE_PAGE_PREWARM_ENABLED);
}

TEST(ZhTwCustomProfile, JournalMigrationMovesOnlyTheProductDefaultTheme) {
  using CustomZhTwProfile::journalMigration;
  using CustomZhTwProfile::THEME_BOOKORBIT;
  using CustomZhTwProfile::THEME_JOURNAL;
  // The prior product default (set by the Home migration) moves to Journal and is marked.
  const auto fromDefault = journalMigration(true, false, THEME_BOOKORBIT);
  EXPECT_TRUE(fromDefault.markApplied);
  EXPECT_EQ(fromDefault.theme, THEME_JOURNAL);
  // Any other saved theme is preserved, but the marker is still recorded, so a later deliberate
  // BookOrbit choice is never migrated.
  for (uint8_t theme : {0, 1, 2, 3, 4, 5, 7}) {
    const auto other = journalMigration(true, false, theme);
    EXPECT_TRUE(other.markApplied);
    EXPECT_EQ(other.theme, theme);
  }
  // Once marked, nothing changes -- including a later deliberate BookOrbit.
  const auto chosen = journalMigration(true, true, THEME_BOOKORBIT);
  EXPECT_FALSE(chosen.markApplied);
  EXPECT_EQ(chosen.theme, THEME_BOOKORBIT);
  // Never before the font profile is known-good (the device would otherwise render tofu).
  const auto noFonts = journalMigration(false, false, THEME_BOOKORBIT);
  EXPECT_FALSE(noFonts.markApplied);
  EXPECT_EQ(noFonts.theme, THEME_BOOKORBIT);
}

TEST(ZhTwCustomProfile, FreshProfileReachesJournalThroughBothMigrations) {
  // A fresh device: font profile applied, then the Home migration sets BOOKORBIT, then Journal.
  ASSERT_TRUE(CustomZhTwProfile::shouldApplyHomeDefaults(true, false));
  const auto j = CustomZhTwProfile::journalMigration(true, false, CustomZhTwProfile::THEME_BOOKORBIT);
  EXPECT_EQ(j.theme, CustomZhTwProfile::THEME_JOURNAL);
}
