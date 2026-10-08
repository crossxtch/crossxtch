#include "ReaderScreen.h"

#include <Gfx.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include <algorithm>

#include <HalTiltSensor.h>

#include "core/BookCache.h"
#include "core/Power.h"
#include "core/Settings.h"
#include "core/UiText.h"
#include "core/fontIds.h"
#include "screens/ChapterSelectionScreen.h"

namespace {
void progressPath(char* out, size_t outSize, const char* bookPath) {
  snprintf(out, outSize, "%s/p_%08lx.bin", Settings::kDir, static_cast<unsigned long>(BookCache::key(bookPath)));
}
}  // namespace

ReaderScreen::ReaderScreen(Gfx& gfx, MappedInput& input, const char* path) : Screen("Reader", gfx, input) {
  snprintf(bookPath, sizeof(bookPath), "%s", path ? path : "");
}

void ReaderScreen::loadProgress() {
  char p[64];
  progressPath(p, sizeof(p), bookPath);
  HalFile f;
  if (!Storage.openFileForRead("PRG", p, f)) {
    page = 0;
    LOG_INF("RDR", "No progress file, start at 0");
    return;
  }
  uint32_t saved = 0;
  if (f.read(&saved, sizeof(saved)) == static_cast<int>(sizeof(saved))) {
    page = saved;
    LOG_INF("RDR", "Progress %s -> page %lu", p, static_cast<unsigned long>(page));
  } else {
    page = 0;
    LOG_ERR("RDR", "Progress file %s unreadable", p);
  }
}

void ReaderScreen::saveProgress() const {
  Storage.ensureDirectoryExists(Settings::kDir);
  char p[64];
  progressPath(p, sizeof(p), bookPath);
  HalFile f;
  if (!Storage.openFileForWrite("PRG", p, f)) {
    LOG_ERR("PRG", "Could not write %s", p);
    return;
  }
  const size_t n = f.write(&page, sizeof(page));
  if (n != sizeof(page)) {
    LOG_ERR("PRG", "Short progress write (%u of %u)", static_cast<unsigned>(n), static_cast<unsigned>(sizeof(page)));
  }
}

bool ReaderScreen::tryOpen() {
  if (xtch.open(bookPath) != xtch::Error::Ok) {
    LOG_ERR("RDR", "Failed to open %s: %s", bookPath, xtch::errorName(xtch.lastError()));
    loaded = false;
    return false;
  }
  loaded = true;
  pageFailed = false;
  memFailed = false;
  loadProgress();
  if (page >= xtch.pageCount()) {
    LOG_INF("RDR", "Saved page %lu past end (%u), clamping", static_cast<unsigned long>(page), xtch.pageCount());
    page = xtch.pageCount() > 0 ? xtch.pageCount() - 1 : 0;
  }
  snprintf(settings.lastBookPath, sizeof(settings.lastBookPath), "%s", bookPath);
  settings.save();
  LOG_INF("RDR", "Open %s page %lu/%u '%s'", bookPath, static_cast<unsigned long>(page + 1), xtch.pageCount(),
          xtch.title());
  return true;
}

void ReaderScreen::onEnter() {
  Screen::onEnter();
  pagesUntilFull = settings.refreshEveryNPages;
  tryOpen();
  requestUpdate();
}

void ReaderScreen::onExit() {
  if (loaded) {
    saveProgress();
  }
  xtch.close();
  halTiltSensor.clearPendingEvents();
  Screen::onExit();
}

void ReaderScreen::onResume() { pagesUntilFull = 1; }

void ReaderScreen::loop() {
  if (!loaded) {
    if (input.wasReleased(MappedInput::Button::Back)) {
      finish();
    } else if (input.wasReleased(MappedInput::Button::Confirm) && tryOpen()) {
      requestUpdate();
    }
    return;
  }

  // Two full-frame SPI copies. Deferred so it is not inside the page the user
  // just waited on, but a tap queued during that refresh is handled below, so
  // this still sits on the next turn. At the 10 MHz idle clock the CPU cannot
  // fill the SPI FIFO and the same copies take ~800 ms instead of ~90 ms.
  // Lock only when there is work: the lock forces 160 MHz, and taking it on
  // every idle tick fights idleDelay and toggles the clock every 50 ms.
  if (xtch.pendingCleanup()) {
    HalPowerManager::Lock powerLock;
    xtch.flushPendingCleanup(gfx);
  }

  // Confirm retries the page that failed. Back still leaves the book.
  // A page turn falls through and clears the hold.
  if (pageFailed || memFailed) {
    if (input.wasReleased(MappedInput::Button::Back)) {
      finish();
      return;
    }
    if (input.wasReleased(MappedInput::Button::Confirm)) {
      pageFailed = false;
      memFailed = false;
      requestUpdate();
      return;
    }
  } else if (input.wasReleased(MappedInput::Button::Back)) {
    finish();
    return;
  } else if (input.wasReleased(MappedInput::Button::Confirm)) {
    pagesUntilFull = 1;
    const auto& chapterList = xtch.getChapters();
    auto screen = makeUniqueNoThrow<ChapterSelectionScreen>(gfx, input, *this, chapterList, page, xtch.pageCount(),
                                                            xtch.chapterReadFailed());
    if (!screen) {
      LOG_ERR("RDR", "OOM: chapters");
      memFailed = true;
      requestUpdate();
      return;
    }
    push(std::move(screen));
    return;
  }

  bool moved = false;
  const int delta = input.consumeReaderPageDelta();

  if (delta > 0) {
    const uint32_t maxForward = xtch.pageCount() > 0 ? xtch.pageCount() - 1 - page : 0;
    const uint32_t step = std::min(static_cast<uint32_t>(delta), maxForward);
    if (step > 0) {
      page += step;
      moved = true;
    } else {
      LOG_DBG("RDR", "Already last page");
    }
  } else if (delta < 0) {
    const uint32_t step = std::min(static_cast<uint32_t>(-delta), page);
    if (step > 0) {
      page -= step;
      moved = true;
    } else {
      LOG_DBG("RDR", "Already first page");
    }
  }
  if (moved) {
    pageFailed = false;
    memFailed = false;
    LOG_DBG("RDR", "Page %lu/%u", static_cast<unsigned long>(page + 1), xtch.pageCount());
    saveProgress();
    requestUpdate();
  }
}

void ReaderScreen::showStatus(const char* title, const char* detail, const char* hint) {
  gfx.clear(false);
  const int mid = gfx.height() / 2;
  if (hint && hint[0] != '\0') {
    gfx.drawCenteredText(FONT_UI_BOLD, mid - 36, title);
    if (detail && detail[0] != '\0') {
      gfx.drawCenteredText(FONT_UI, mid, detail);
    }
    gfx.drawCenteredText(FONT_UI, mid + 36, hint);
  } else {
    gfx.drawCenteredText(FONT_UI_BOLD, mid - 20, title);
    if (detail && detail[0] != '\0') {
      gfx.drawCenteredText(FONT_UI, mid + 10, detail);
    }
  }
  gfx.present(HalDisplay::HALF_REFRESH);
  // The next real page has to rebuild the gray base. A fast refresh over this
  // text leaves it ghosted on the panel.
  pagesUntilFull = 1;
}

void ReaderScreen::jumpToPage(const uint32_t targetPage) {
  if (!loaded || targetPage >= xtch.pageCount()) {
    return;
  }
  page = targetPage;
  pageFailed = false;
  memFailed = false;
  pagesUntilFull = 1;
  LOG_INF("RDR", "Jumped to page %lu/%u", static_cast<unsigned long>(page + 1), xtch.pageCount());
  saveProgress();
  requestUpdate();
}

void ReaderScreen::showPageError() {
  const char* detail = uiText::xtchError(xtch.lastError());
  const char* title = uiText::couldNotReadFile;
  // ReadError and out-of-memory already say the whole thing. A second copy of
  // the same line is just noise on the panel.
  if (detail == title || detail == uiText::outOfMemory) {
    title = detail;
    detail = nullptr;
  }
  showStatus(title, detail, uiText::confirmRetry);
}

void ReaderScreen::render() {
  if (!loaded) {
    showStatus(uiText::couldNotOpenBook, uiText::xtchError(xtch.lastError()), uiText::confirmRetry);
    return;
  }
  if (memFailed) {
    showStatus(uiText::outOfMemory, nullptr, uiText::confirmRetry);
    return;
  }
  if (pageFailed) {
    showPageError();
    return;
  }

  const bool painted = xtch.drawPage(gfx, page, pagesUntilFull, settings.refreshEveryNPages);
  if (!painted) {
    LOG_ERR("RDR", "Blit page %lu failed: %s", static_cast<unsigned long>(page), xtch::errorName(xtch.lastError()));
    pageFailed = true;
    showPageError();
    return;
  }
  if (power::tiltLocked()) {
    power::paintGyroOffMarker();
  }

  // Skip prefetch when the user already queued a skip-ahead or back-turn
  // during the blit; loading N+1 would delay that jump by ~146 ms.
  const int queued = input.queuedPageDelta();
  if (queued >= 0 && queued <= 1) {
    xtch.prefetchForward(page);
  } else {
    LOG_DBG("RDR", "Skip prefetch (queued delta %d)", queued);
  }
}
