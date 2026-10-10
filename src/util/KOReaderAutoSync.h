#pragma once
#include <string>

class GfxRenderer;

// Automatic, silent KOSync push-on-sleep / pull-on-open. Reuses the same client code the manual
// "Sync Progress" reader-menu action does (KOReaderSyncClient, KOReaderDocumentId, ProgressMapper,
// KOReaderCredentialStore) -- this is not a second sync client, just headless callers of the
// existing one. Both entry points:
//  - No-op (no radio, no delay) unless a saved WiFi network and KOReader credentials both exist.
//  - Use a short, direct connect (last-connected SSID, no scan) bounded by a few seconds.
//  - Are abortable mid-connect/mid-request via a button press, so they can never strand the
//    caller in a half-finished state.
//  - Never surface UI, never block their caller beyond the bounded timeout, and treat every
//    failure (no WiFi, auth, network, server) as "give up silently, proceed as if sync wasn't
//    attempted."
//  - Store-and-forward the KOReader progress/percentage fields verbatim on the pull side rather
//    than re-deriving them -- CrossPoint only computes its OWN side of a comparison, never the
//    remote's. See lib/KOReaderSync/ProgressMapper.h.
namespace KOReaderAutoSync {

enum class Status { Skipped, UpToDate, Uploaded, Downloaded, Failed };
struct Result {
  Status status;
  float percentage = 0.0f;
  bool matchesRemote = false;
};

// Call from main.cpp's enterDeepSleep(), after the sleep screen is already painted, or from an
// explicit sync wake. This function manages the radio's on/off lifecycle itself.
// Pushes APP_STATE.openEpubPath's current local progress if it's ahead of the server's; no-ops
// entirely unless APP_STATE.lastSleepFromReader is true. An explicit sync wake
// may also push the last open book when sleep was entered from Home.
Result pushOnSleep(bool explicitRequest = false);
// True once if Power + X3 right-side button was observed while the sleep push
// was polling the network. Lets the sleep path honor a gesture released before
// the network call returns.
bool takeSleepComboObserved();

// Call when a book is freshly opened from a browsing screen (Home/Covers/Titles), or during an
// explicit sync wake. Pulls the
// server's progress for epubPath and, if it's further than local, writes it into progress.bin
// before the caller constructs the reader activity, so EpubReaderActivity's own
// EpubReaderUtils::loadProgress() picks it up with no changes on its side. Only applies to EPUB
// files (matching the manual sync feature's own scope); a no-op for any other extension.
Result pullFurthestOnOpen(const std::string& epubPath, GfxRenderer& renderer);

// Explicit sleep-button sync: compare the current book once, then upload or
// download whichever position is further. Uses one WiFi association and one
// remote GET instead of running the separate push and pull paths back-to-back.
Result syncOnDemand(const std::string& epubPath, GfxRenderer& renderer);

}  // namespace KOReaderAutoSync
