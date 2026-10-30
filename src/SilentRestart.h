#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open EPUB (APP_STATE.openEpubPath)
// BookOrbit library exits while WiFi is up (docs/bookorbit-home-library.md §9 finding 1): the
// heap-defrag reboot must still land where the user was going, not on a generic Home.
void silentRestartToReaderWithSync();  // goToReader(openEpubPath, checkRemoteProgress=true)
void silentRestartToHomeLibrary();     // BookOrbit Home with the 書庫 tile focused
