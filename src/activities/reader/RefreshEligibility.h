#pragma once

// Whether a scheduled maintenance refresh (the periodic anti-ghosting cleanup that would
// otherwise promote to a flashing HALF_REFRESH) may instead stay on the no-flash FAST_REFRESH
// waveform. Pure decision logic -- no I/O -- so it is host-testable in isolation.
//
// All four conditions must hold: the page has no images, no grayscale-AA text pass runs on it, no
// manual forced refresh is pending, and no image-page residue cleanup is pending -- an image page
// leaves gray charge in its image region that a plain fast diff on the immediately following page
// can't clear (see the pagesUntilFullRefresh=1 comment at its call site), so that one next page
// must still flash regardless of how eligible it otherwise looks (bug #2190).
inline bool isEligibleForNoFlashMaintenance(bool pageHasImages, bool needsTextGrayscale, bool manualRefreshPending,
                                            bool imageResidueCleanupPending) {
  return !pageHasImages && !needsTextGrayscale && !manualRefreshPending && !imageResidueCleanupPending;
}

// FAST_REFRESH is the same non-flashing DU waveform used for every ordinary page turn -- it cannot
// reinforce pixels the way the flashing HALF_REFRESH does. Substituting it at every maintenance
// boundary would let an unbroken run of content-eligible pages (e.g. a pure-text chapter) defer
// real cleanup forever, silently turning "flashes every N pages" into "never flashes" on the very
// panel (Uc8253X3Driver) that has no driver-level ghost-clear safety net. Bound it: after this many
// consecutive maintenance boundaries satisfied by the no-flash substitute, force a real flash
// regardless of content eligibility.
constexpr int kMaxConsecutiveNoFlashMaintenance = 3;

inline bool mustForceRealMaintenance(int consecutiveNoFlashMaintenanceCount) {
  return consecutiveNoFlashMaintenanceCount >= kMaxConsecutiveNoFlashMaintenance;
}
