#include "KOReaderAutoSync.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "WifiCredentialStore.h"
#include "activities/reader/EpubReaderUtils.h"

namespace KOReaderAutoSync {

namespace {

// Direct connect to the last-known network, no scan -- a scan alone can cost several seconds,
// which this path can't afford. Bounded and abortable so a sleep transition or a book-open can
// never be stuck waiting on a network that isn't there.
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 3500;
// Ignore sub-page percentage rounding differences when comparing devices.
constexpr float PROGRESS_MATCH_TOLERANCE = 0.0005f;
bool sleepPushActive = false;
bool sleepComboObserved = false;

bool userRequestedAbort() {
  gpio.update();
  if (sleepPushActive && gpio.deviceIsX3() && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.isPressed(HalGPIO::BTN_DOWN)) {
    sleepComboObserved = true;
  }
  return gpio.wasAnyPressed() || sleepComboObserved;
}

// Returns true once actually connected. Callers only reach here after confirming a saved
// credential exists, so a false return here always means "attempted and failed/timed out/aborted"
// -- never "never tried."
bool connectToSavedWifi(const KOReaderSyncClient::AbortCallback& shouldAbort = userRequestedAbort) {
  if (WIFI_STORE.getCredentialCount() == 0) return false;

  std::optional<WifiCredential> cred;
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastSsid.empty()) {
    cred = WIFI_STORE.findCredential(lastSsid);
  }
  if (!cred) {
    cred = WIFI_STORE.getCredentialAt(0);
  }
  if (!cred) return false;

  WiFi.mode(WIFI_STA);
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }

  const unsigned long deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() >= deadline || shouldAbort()) {
      LOG_DBG("KOAuto", "WiFi connect timed out or aborted");
      return false;
    }
    delay(50);
  }
  return true;
}

void disconnectWifi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// buildIfMissing=true: a book being pulled for the first time on this device has no book.bin cache
// yet, and a real spine build is the only way calculateProgress()/ProgressMapper get usable spine
// sizes (loadMetadataOnly() explicitly skips spine entries -- see Epub::loadMetadataOnly()). A book
// being pushed always already has one, from the reading session that just ended.
bool loadEpubForSync(Epub& epub, bool buildIfMissing) {
  if (buildIfMissing) epub.setupCacheDir();
  return epub.load(buildIfMissing, /*skipLoadingCss=*/true);
}

// Match ProgressMapper::toSavedProgress(): the last page has index totalPages - 1.
// Using totalPages here makes our own uploaded percentage appear ahead on wake.
float pageFraction(int pageNumber, int totalPages) {
  return totalPages > 1 ? static_cast<float>(pageNumber) / static_cast<float>(totalPages - 1) : 0.0f;
}

std::string computeDocumentHash(const std::string& path) {
  return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateFromFilename(path)
             : KOReaderDocumentId::calculate(path);
}

}  // namespace

bool takeSleepComboObserved() {
  const bool observed = sleepComboObserved;
  sleepComboObserved = false;
  return observed;
}

Result pushOnSleep(bool explicitRequest) {
  sleepComboObserved = false;
  sleepPushActive = true;
  struct SleepPushScope {
    ~SleepPushScope() { sleepPushActive = false; }
  } sleepPushScope;
  if (!KOREADER_STORE.getAutoSyncEnabled()) {
    LOG_DBG("KOAuto", "Push: gated out -- auto-sync disabled in settings");
    return {Status::Skipped};
  }
  if (!explicitRequest && !APP_STATE.lastSleepFromReader) {
    LOG_DBG("KOAuto", "Push: gated out -- last sleep wasn't from the reader");
    return {Status::Skipped};
  }
  const std::string& epubPath = APP_STATE.openEpubPath;
  if (epubPath.empty() || !FsHelpers::hasEpubExtension(epubPath)) {
    LOG_DBG("KOAuto", "Push: gated out -- no open EPUB (path='%s')", epubPath.c_str());
    return {Status::Skipped};
  }
  if (WIFI_STORE.getCredentialCount() == 0 || !KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOAuto", "Push: gated out -- %zu saved WiFi network(s), KOReader credentials %s",
            WIFI_STORE.getCredentialCount(), KOREADER_STORE.hasCredentials() ? "present" : "MISSING");
    return {Status::Skipped};
  }

  // Logged unconditionally, before any network attempt: this is the value most likely to explain
  // a silent cross-device mismatch (BINARY requires byte-identical files; a different match method
  // or a same-titled-but-not-identical file on each device produces two different hashes, so each
  // device's GET/PUT silently talks to a different server-side document with no error anywhere).
  const auto matchMethod = KOREADER_STORE.getMatchMethod();
  const std::string hash = computeDocumentHash(epubPath);
  LOG_DBG("KOAuto", "Push: doc='%s' match=%s hash=%s", epubPath.c_str(),
          matchMethod == DocumentMatchMethod::FILENAME ? "filename" : "binary",
          hash.empty() ? "(empty)" : hash.c_str());
  if (hash.empty()) {
    LOG_DBG("KOAuto", "Push: gated out -- empty document hash");
    return {Status::Failed};
  }

  if (!connectToSavedWifi()) {
    LOG_DBG("KOAuto", "Push: WiFi connect failed or timed out, giving up");
    disconnectWifi();
    return {Status::Failed};
  }
  LOG_DBG("KOAuto", "Push: WiFi connected (%s)", WiFi.SSID().c_str());

  KOReaderProgress remote;
  const auto getResult = KOReaderSyncClient::getProgress(hash, remote, userRequestedAbort);
  LOG_DBG("KOAuto", "Push: remote fetch -> %s (http=%d)", KOReaderSyncClient::errorString(getResult),
          KOReaderSyncClient::lastHttpCode);
  if (getResult != KOReaderSyncClient::OK && getResult != KOReaderSyncClient::NOT_FOUND) {
    LOG_DBG("KOAuto", "Push: remote fetch failed (%d), giving up", getResult);
    disconnectWifi();
    return {Status::Failed};
  }

  // The book was just being read, so it was already fully loaded this session -- buildIfMissing
  // is false here, not true: if this unexpectedly can't load from cache, something is wrong
  // enough that guessing a position is worse than skipping the push.
  auto epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  if (!loadEpubForSync(*epub, /*buildIfMissing=*/false)) {
    LOG_DBG("KOAuto", "Push: could not load epub from cache, skipping");
    disconnectWifi();
    return {Status::Failed};
  }

  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  const bool hadLocal = EpubReaderUtils::loadProgress(epub->getCachePath(), spineIndex, pageNumber, pageCount);
  const float chapterProgress = hadLocal ? pageFraction(pageNumber, pageCount) : 0.0f;
  const float localPercentage = hadLocal ? epub->calculateProgress(spineIndex, chapterProgress) : 0.0f;

  LOG_DBG("KOAuto", "Push: local=%.4f%% (hadLocal=%d spine=%d page=%d/%d) remote=%s%.4f%%", localPercentage * 100,
          hadLocal, spineIndex, pageNumber, pageCount,
          getResult == KOReaderSyncClient::NOT_FOUND ? "none-found " : "", remote.percentage * 100);

  // Furthest-wins: only push when local is genuinely ahead. getResult == NOT_FOUND means there's
  // nothing to compare against, so any real local progress is worth uploading.
  if (getResult == KOReaderSyncClient::OK && remote.percentage >= localPercentage) {
    LOG_DBG("KOAuto", "Push: remote (%.4f) already >= local (%.4f), nothing to do", remote.percentage,
            localPercentage);
    disconnectWifi();
    return {Status::UpToDate, remote.percentage,
            hadLocal && std::abs(remote.percentage - localPercentage) <= PROGRESS_MATCH_TOLERANCE};
  }
  if (!hadLocal) {
    LOG_DBG("KOAuto", "Push: no local progress saved yet, nothing to push");
    disconnectWifi();
    return {Status::Skipped};
  }
  LOG_DBG("KOAuto", "Push: local is ahead, uploading");

  const SavedProgressPosition saved = ProgressMapper::toSavedProgress(epub, CrossPointPosition{
                                                                                 spineIndex,
                                                                                 pageNumber,
                                                                                 pageCount,
                                                                             });

  KOReaderProgress toPush;
  toPush.document = hash;
  toPush.progress = saved.xpath;
  toPush.percentage = saved.percentage;
  toPush.device = SETTINGS.getEffectiveDeviceName();
  if (KOREADER_STORE.usesCrossPointSyncServer()) {
    KOReaderRichPosition pos;
    pos.pctQ = static_cast<uint32_t>(std::clamp(saved.percentage, 0.0f, 1.0f) * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(spineIndex);
    pos.pageNumber = static_cast<uint16_t>(pageNumber);
    pos.totalPages = static_cast<uint16_t>(pageCount > 0 ? pageCount : 1);
    pos.xpath = saved.xpath;
    toPush.position = std::move(pos);
  }
  if (KOREADER_STORE.getSendMetadata()) {
    KOReaderMetadata meta;
    const auto lastSlash = epubPath.rfind('/');
    meta.filename = lastSlash == std::string::npos ? epubPath : epubPath.substr(lastSlash + 1);
    meta.title = epub->getTitle();
    meta.authors = epub->getAuthor();
    toPush.metadata = std::move(meta);
  }

  LOG_DBG("KOAuto", "Push: sending document=%s device=%s percentage=%.4f xpath=%s", toPush.document.c_str(),
          toPush.device.c_str(), toPush.percentage, toPush.progress.c_str());

  const auto putResult = KOReaderSyncClient::updateProgress(toPush, userRequestedAbort);
  LOG_DBG("KOAuto", "Push result: %s (http=%d, local=%.4f)", KOReaderSyncClient::errorString(putResult),
          KOReaderSyncClient::lastHttpCode, localPercentage);
  disconnectWifi();
  return {putResult == KOReaderSyncClient::OK ? Status::Uploaded : Status::Failed, saved.percentage};
}

Result pullFurthestOnOpen(const std::string& epubPath, GfxRenderer& renderer) {
  if (!KOREADER_STORE.getAutoSyncEnabled()) {
    LOG_DBG("KOAuto", "Pull: gated out -- auto-sync disabled in settings");
    return {Status::Skipped};
  }
  if (epubPath.empty() || !FsHelpers::hasEpubExtension(epubPath)) {
    LOG_DBG("KOAuto", "Pull: gated out -- not an EPUB (path='%s')", epubPath.c_str());
    return {Status::Skipped};
  }
  if (WIFI_STORE.getCredentialCount() == 0 || !KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOAuto", "Pull: gated out -- %zu saved WiFi network(s), KOReader credentials %s",
            WIFI_STORE.getCredentialCount(), KOREADER_STORE.hasCredentials() ? "present" : "MISSING");
    return {Status::Skipped};
  }

  // See pushOnSleep()'s comment: this is the value most likely to explain a silent cross-device
  // mismatch. Compare this line's hash/match against the OTHER device's push-side log line for the
  // same book -- if they differ, that's the whole bug right there.
  const auto matchMethod = KOREADER_STORE.getMatchMethod();
  const std::string hash = computeDocumentHash(epubPath);
  LOG_DBG("KOAuto", "Pull: doc='%s' match=%s hash=%s", epubPath.c_str(),
          matchMethod == DocumentMatchMethod::FILENAME ? "filename" : "binary",
          hash.empty() ? "(empty)" : hash.c_str());
  if (hash.empty()) {
    LOG_DBG("KOAuto", "Pull: gated out -- empty document hash");
    return {Status::Failed};
  }

  if (!connectToSavedWifi()) {
    LOG_DBG("KOAuto", "Pull: WiFi connect failed or timed out, giving up");
    disconnectWifi();
    return {Status::Failed};
  }
  LOG_DBG("KOAuto", "Pull: WiFi connected (%s)", WiFi.SSID().c_str());

  KOReaderProgress remote;
  const auto getResult = KOReaderSyncClient::getProgress(hash, remote, userRequestedAbort);
  disconnectWifi();  // one request either way; done with the radio regardless of outcome
  LOG_DBG("KOAuto", "Pull: remote fetch -> %s (http=%d)", KOReaderSyncClient::errorString(getResult),
          KOReaderSyncClient::lastHttpCode);

  if (getResult != KOReaderSyncClient::OK) {
    // NOT_FOUND here means the server has never seen this hash -- on a cross-device mismatch this
    // is exactly what fires on every pull, silently, forever: check the hash logged above against
    // the other device's.
    LOG_DBG("KOAuto", "Pull: no usable remote progress (%s), leaving local position",
            KOReaderSyncClient::errorString(getResult));
    return {getResult == KOReaderSyncClient::NOT_FOUND ? Status::Skipped : Status::Failed};
  }

  // First local book.bin build for this device happens here if it's never been opened locally --
  // see loadEpubForSync()'s comment. Deferred until after the network round trip so the common
  // "no WiFi" / "no remote progress" cases never pay for it.
  auto epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  if (!loadEpubForSync(*epub, /*buildIfMissing=*/true)) {
    LOG_DBG("KOAuto", "Pull: could not load epub, skipping");
    return {Status::Failed};
  }

  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  const bool hadLocal = EpubReaderUtils::loadProgress(epub->getCachePath(), spineIndex, pageNumber, pageCount);
  const float chapterProgress = hadLocal ? pageFraction(pageNumber, pageCount) : 0.0f;
  const float localPercentage = hadLocal ? epub->calculateProgress(spineIndex, chapterProgress) : 0.0f;

  LOG_DBG("KOAuto", "Pull: remote=%.4f%% (device=%s has_rich_position=%d) local=%.4f%% (hadLocal=%d spine=%d page=%d/%d)",
          remote.percentage * 100, remote.device.c_str(), remote.position.has_value(), localPercentage * 100, hadLocal,
          spineIndex, pageNumber, pageCount);

  if (hadLocal && remote.percentage <= localPercentage + PROGRESS_MATCH_TOLERANCE) {
    const bool matchesRemote = std::abs(remote.percentage - localPercentage) <= PROGRESS_MATCH_TOLERANCE;
    LOG_DBG("KOAuto", "Pull: local (%.4f) %s remote (%.4f), leaving local position", localPercentage,
            matchesRemote ? "matches" : "is ahead of", remote.percentage);
    return {Status::UpToDate, localPercentage, matchesRemote};
  }
  LOG_DBG("KOAuto", "Pull: remote is further, mapping to a local position");

  // Prefer the exact spine/page from a CrossPoint sync server when available (lossless); fall back
  // to xpath/percentage mapping otherwise -- store-and-forward, not re-derived: both paths apply
  // the server's own fields via ProgressMapper as-is.
  std::optional<CrossPointPosition> mapped;
  // Legacy auto-uploads omitted the rich position. The server kept an older
  // position object alongside the newer percentage/XPath, so it can point to
  // a different chapter. Only use it when it describes this same upload.
  const bool richMatches = remote.position.has_value() &&
                           std::abs(static_cast<float>(remote.position->pctQ) / 1000000.0f - remote.percentage) <
                               0.0005f &&
                           (remote.position->xpath.empty() || remote.position->xpath == remote.progress);
  if (richMatches) {
    mapped = ProgressMapper::fromRichPosition(epub, *remote.position, renderer);
    LOG_DBG("KOAuto", "Pull: rich-position mapping %s", mapped.has_value() ? "succeeded" : "failed, falling back");
  } else if (remote.position.has_value()) {
    LOG_DBG("KOAuto", "Pull: stale rich position disagrees with current percentage/XPath; using XPath");
  }
  if (!mapped.has_value()) {
    const SavedProgressPosition koPos{remote.progress, remote.percentage};
    mapped = ProgressMapper::toCrossPoint(epub, koPos, renderer, /*currentSpineIndex=*/-1,
                                          /*totalPagesInCurrentSpine=*/0);
    LOG_DBG("KOAuto", "Pull: xpath/percentage mapping %s", mapped.has_value() ? "succeeded" : "failed");
  }
  if (!mapped.has_value()) {
    LOG_ERR("KOAuto", "Pull: found further remote progress but could not map it to a local position, giving up");
    return {Status::Failed};
  }
  LOG_DBG("KOAuto", "Pull: mapped to spine=%d page=%d/%d", mapped->spineIndex, mapped->pageNumber, mapped->totalPages);

  if (!EpubReaderUtils::saveProgress(*epub, mapped->spineIndex, mapped->pageNumber, mapped->totalPages)) {
    LOG_ERR("KOAuto", "Pull: found further remote progress but failed to save it locally");
    return {Status::Failed};
  }
  LOG_DBG("KOAuto", "Pull: applied remote position (local was %.4f, remote %.4f) -> spine=%d page=%d/%d",
          localPercentage, remote.percentage, mapped->spineIndex, mapped->pageNumber, mapped->totalPages);
  return {Status::Downloaded, remote.percentage};
}

Result syncOnDemand(const std::string& epubPath, GfxRenderer& renderer) {
  if (!KOREADER_STORE.getAutoSyncEnabled() || epubPath.empty() || !FsHelpers::hasEpubExtension(epubPath) ||
      WIFI_STORE.getCredentialCount() == 0 || !KOREADER_STORE.hasCredentials()) {
    return {Status::Skipped};
  }

  const std::string hash = computeDocumentHash(epubPath);
  if (hash.empty()) return {Status::Failed};

  // The wake combo may still be held when the sync starts. Ignore its press
  // edges until both keys have been released; only a later press cancels I/O.
  bool gestureReleased = false;
  const KOReaderSyncClient::AbortCallback shouldAbort = [&gestureReleased]() {
    gpio.update();
    if (!gestureReleased) {
      gestureReleased = !gpio.isPressed(HalGPIO::BTN_POWER) && !gpio.isPressed(HalGPIO::BTN_DOWN);
      return false;
    }
    return gpio.wasAnyPressed();
  };

  if (!connectToSavedWifi(shouldAbort)) {
    disconnectWifi();
    return {Status::Failed};
  }

  KOReaderProgress remote;
  const auto getResult = KOReaderSyncClient::getProgress(hash, remote, shouldAbort);
  if (getResult != KOReaderSyncClient::OK && getResult != KOReaderSyncClient::NOT_FOUND) {
    LOG_DBG("KOAuto", "Demand: remote fetch failed (%d)", getResult);
    disconnectWifi();
    return {Status::Failed};
  }

  // Keep the TLS handshake free of EPUB allocations. Once the remote GET has
  // completed, load the local cache only once and compare both positions.
  auto epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  if (!loadEpubForSync(*epub, /*buildIfMissing=*/true)) {
    disconnectWifi();
    return {Status::Failed};
  }
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  const bool hadLocal = EpubReaderUtils::loadProgress(epub->getCachePath(), spineIndex, pageNumber, pageCount);
  const float localPercentage =
      hadLocal ? epub->calculateProgress(spineIndex, pageFraction(pageNumber, pageCount)) : 0.0f;
  LOG_DBG("KOAuto", "Demand: doc='%s' hash=%s local=%.4f%% remote=%s%.4f%% (saved=%d spine=%d page=%d/%d)",
          epubPath.c_str(), hash.c_str(), localPercentage * 100,
          getResult == KOReaderSyncClient::NOT_FOUND ? "none " : "", remote.percentage * 100, hadLocal, spineIndex,
          pageNumber, pageCount);

  if (!hadLocal && getResult == KOReaderSyncClient::NOT_FOUND) {
    disconnectWifi();
    return {Status::Skipped};
  }

  if (hadLocal &&
      (getResult == KOReaderSyncClient::NOT_FOUND ||
       localPercentage > remote.percentage + PROGRESS_MATCH_TOLERANCE)) {
    const SavedProgressPosition saved =
        ProgressMapper::toSavedProgress(epub, CrossPointPosition{spineIndex, pageNumber, pageCount});
    KOReaderProgress toPush;
    toPush.document = hash;
    toPush.progress = saved.xpath;
    toPush.percentage = saved.percentage;
    toPush.device = SETTINGS.getEffectiveDeviceName();
    if (KOREADER_STORE.usesCrossPointSyncServer()) {
      KOReaderRichPosition pos;
      pos.pctQ = static_cast<uint32_t>(std::clamp(saved.percentage, 0.0f, 1.0f) * 1000000.0f + 0.5f);
      pos.spineIndex = static_cast<uint16_t>(spineIndex);
      pos.pageNumber = static_cast<uint16_t>(pageNumber);
      pos.totalPages = static_cast<uint16_t>(pageCount > 0 ? pageCount : 1);
      pos.xpath = saved.xpath;
      toPush.position = std::move(pos);
    }
    if (KOREADER_STORE.getSendMetadata()) {
      KOReaderMetadata meta;
      const auto lastSlash = epubPath.rfind('/');
      meta.filename = lastSlash == std::string::npos ? epubPath : epubPath.substr(lastSlash + 1);
      meta.title = epub->getTitle();
      meta.authors = epub->getAuthor();
      toPush.metadata = std::move(meta);
    }
    epub.reset();  // leave maximum heap available for the HTTPS upload
    const auto putResult = KOReaderSyncClient::updateProgress(toPush, shouldAbort);
    disconnectWifi();
    LOG_DBG("KOAuto", "Demand: upload %s at %.4f%%", KOReaderSyncClient::errorString(putResult),
            saved.percentage * 100);
    return {putResult == KOReaderSyncClient::OK ? Status::Uploaded : Status::Failed, saved.percentage};
  }

  disconnectWifi();
  if (hadLocal && std::abs(remote.percentage - localPercentage) <= PROGRESS_MATCH_TOLERANCE) {
    return {Status::UpToDate, localPercentage, true};
  }

  // The remote position is further. Prefer the exact CrossPoint position only
  // if it agrees with this upload; old server records may contain stale rich
  // data alongside a newer KOReader percentage and XPath.
  std::optional<CrossPointPosition> mapped;
  const bool richMatches = remote.position.has_value() &&
                           std::abs(static_cast<float>(remote.position->pctQ) / 1000000.0f - remote.percentage) <
                               PROGRESS_MATCH_TOLERANCE &&
                           (remote.position->xpath.empty() || remote.position->xpath == remote.progress);
  if (richMatches) mapped = ProgressMapper::fromRichPosition(epub, *remote.position, renderer);
  if (!mapped.has_value()) {
    mapped = ProgressMapper::toCrossPoint(epub, SavedProgressPosition{remote.progress, remote.percentage}, renderer,
                                          /*currentSpineIndex=*/-1, /*totalPagesInCurrentSpine=*/0);
  }
  if (!mapped.has_value() ||
      !EpubReaderUtils::saveProgress(*epub, mapped->spineIndex, mapped->pageNumber, mapped->totalPages)) {
    LOG_ERR("KOAuto", "Demand: failed to apply remote position at %.4f%%", remote.percentage * 100);
    return {Status::Failed};
  }
  LOG_DBG("KOAuto", "Demand: pulled %.4f%% -> spine=%d page=%d/%d", remote.percentage * 100,
          mapped->spineIndex, mapped->pageNumber, mapped->totalPages);
  return {Status::Downloaded, remote.percentage};
}

}  // namespace KOReaderAutoSync
