#pragma once
#include <cstdint>
#include <string>

// A single bookmark entry — a position in a book.
struct BookmarkEntry {
  std::string xpath;    // XPath-like progress string
  std::string summary;  // First few words of a page to help identify it
  float percentage;     // Progress percentage (0.0 to 1.0)

  uint16_t computedSpineIndex = 0;        // Spine index at the time of bookmarking
  uint16_t computedChapterPageCount = 0;  // Total page count of the chapter at the time of bookmarking
  uint16_t computedChapterProgress = 0;   // Number of pages into the chapter at the time of bookmarking

  // BookOrbit bookmark-sync identity (see src/util/BookOrbitBookmarkSync.h). Empty/false for
  // every bookmark created before this feature existed and for one not yet offered to a server --
  // both read as "not yet part of the synced set," which is exactly correct: neither has ever
  // been reported to BookOrbit under this identity.
  //
  // boDatetime is minted ONCE, at first sync (not at bookmark-creation time, to keep the
  // interactive toggle-bookmark path free of an RTC read), and never changes afterward: the
  // server's identity key is md5(boDatetime | xpath), so changing either field after minting
  // would make this bookmark reappear on the server as a brand-new one.
  std::string boDatetime;
  // True once a server exchange has confirmed this bookmark's identity is known to BookOrbit --
  // either this device uploaded it and got a 2xx, or it was created by applying an incoming
  // server-side change. False means "minted but not yet successfully offered" (still queued for
  // the next sync attempt) when boDatetime is non-empty, and "not applicable" when it is empty.
  bool boSynced = false;
  // Hex BookOrbitOwnerId fingerprint of the server+account boSynced was last confirmed against.
  // Empty means "never confirmed against any owner" (identical to boSynced=false in effect).
  // Scoping matters because switching BookOrbit account/server does NOT clear boDatetime/boSynced
  // (an identity, once minted, never changes -- see boDatetime's own comment) -- without this
  // field, a bookmark already synced to a PREVIOUS account would be silently skipped forever when
  // syncing to a NEW one, since buildOutgoing() would see boSynced=true and never resend it. See
  // BookOrbitBookmarkSync.h's own doc comment for the full reasoning.
  std::string boOwner;
};