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
  f.write(&page, sizeof(page));
}

void ReaderScreen::onEnter() {
  Screen::onEnter();
  pagesUntilFull = settings.refreshEveryNPages;
  if (xtch.open(bookPath) != xtch::Error::Ok) {
    LOG_ERR("RDR", "Failed to open %s: %s", bookPath, xtch::errorName(xtch.lastError()));
    loaded = false;
    requestUpdate();
    return;
  }
  loaded = true;
  loadProgress();
  if (page >= xtch.pageCount()) {
    LOG_INF("RDR", "Saved page %lu past end (%u), clamping", static_cast<unsigned long>(page), xtch.pageCount());
    page = xtch.pageCount() > 0 ? xtch.pageCount() - 1 : 0;
  }
  snprintf(settings.lastBookPath, sizeof(settings.lastBookPath), "%s", bookPath);
  settings.save();
  LOG_INF("RDR", "Open %s page %lu/%u '%s'", bookPath, static_cast<unsigned long>(page + 1), xtch.pageCount(),
          xtch.title());
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
    if (input.wasReleased(MappedInput::Button::Back) || input.wasReleased(MappedInput::Button::Confirm)) {
      finish();
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

  if (input.wasReleased(MappedInput::Button::Back)) {
    finish();
    return;
  }
  if (input.wasReleased(MappedInput::Button::Confirm)) {
    pagesUntilFull = 1;
    auto screen = makeUniqueNoThrow<ChapterSelectionScreen>(gfx, input, *this, xtch.getChapters(), page, xtch.pageCount());
    if (!screen) {
      LOG_ERR("RDR", "OOM: chapters");
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
    LOG_DBG("RDR", "Page %lu/%u", static_cast<unsigned long>(page + 1), xtch.pageCount());
    saveProgress();
    requestUpdate();
  }
}

void ReaderScreen::showStatus(const char* title, const char* detail) {
  gfx.clear(false);
  gfx.drawCenteredText(FONT_UI_BOLD, gfx.height() / 2 - 20, title);
  if (detail && detail[0] != '\0') {
    gfx.drawCenteredText(FONT_UI, gfx.height() / 2 + 10, detail);
  }
  gfx.present(HalDisplay::HALF_REFRESH);
}

void ReaderScreen::jumpToPage(const uint32_t targetPage) {
  if (!loaded || targetPage >= xtch.pageCount()) {
    return;
  }
  page = targetPage;
  pagesUntilFull = 1;
  LOG_INF("RDR", "Jumped to page %lu/%u", static_cast<unsigned long>(page + 1), xtch.pageCount());
  saveProgress();
  requestUpdate();
}

void ReaderScreen::render() {
  if (!loaded) {
    showStatus(uiText::couldNotOpenBook, uiText::error(xtch::errorName(xtch.lastError())));
    return;
  }

  const bool painted = xtch.drawPage(gfx, page, pagesUntilFull, settings.refreshEveryNPages);
  if (!painted) {
    LOG_ERR("RDR", "Blit page %lu failed: %s", static_cast<unsigned long>(page), xtch::errorName(xtch.lastError()));
    showStatus(uiText::error(xtch::errorName(xtch.lastError())));
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
