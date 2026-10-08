#include "ChapterSelectionScreen.h"

#include <Gfx.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/UiList.h"
#include "core/UiText.h"
#include "screens/PageJumpScreen.h"
#include "screens/ReaderScreen.h"

ChapterSelectionScreen::ChapterSelectionScreen(Gfx& gfx, MappedInput& input, ReaderScreen& reader,
                                               const std::vector<xtch::ChapterInfo>& chapterList,
                                               const uint32_t currentPage, const uint16_t pageCount,
                                               const bool unreadable)
    : Screen("Chapters", gfx, input),
      reader(reader),
      chapters(chapterList),
      currentPage(currentPage),
      pageCount(pageCount) {
  if (unreadable) {
    notice = uiText::couldNotReadFile;
  }
  // Row 0 is the synthetic "Go to page" item; chapters start at row 1.
  for (size_t i = 0; i < chapters.size(); ++i) {
    if (currentPage >= chapters[i].startPage && currentPage <= chapters[i].endPage) {
      index = static_cast<int>(i) + 1;
      break;
    }
  }
}

void ChapterSelectionScreen::activate() {
  if (index == 0) {
    auto screen = makeUniqueNoThrow<PageJumpScreen>(gfx, input, reader, currentPage, pageCount);
    if (!screen) {
      LOG_ERR("SCR", "OOM: page jump");
      setNotice(uiText::outOfMemory);
      return;
    }
    notice = nullptr;
    push(std::move(screen));
    return;
  }
  const size_t chapterIdx = static_cast<size_t>(index - 1);
  if (chapterIdx >= chapters.size()) {
    return;
  }
  reader.jumpToPage(chapters[chapterIdx].startPage);
  finish();
}

void ChapterSelectionScreen::loop() {
  if (input.wasReleased(MappedInput::Button::Back)) {
    finish();
    return;
  }
  const int count = static_cast<int>(chapters.size()) + 1;
  if (ui::applyDelta(index, input.consumeNavigationDelta(), count)) {
    requestUpdate();
  } else if (input.wasReleased(MappedInput::Button::Confirm)) {
    activate();
  }
}

void ChapterSelectionScreen::render() {
  gfx.clear(false);
  gfx.drawCenteredText(FONT_UI_BOLD, 8, uiText::chapters);

  const int rowH = gfx.lineHeight(FONT_UI) + 8;
  const int top = 40;
  const int rows = (gfx.height() - top - (notice ? 48 : 24)) / rowH;
  const int count = static_cast<int>(chapters.size()) + 1;
  ui::followWindow(window, index, rows);
  const int last = std::min(window + rows, count);
  char label[192];
  auto fillLabel = [&](const int i) {
    if (i == 0) {
      snprintf(label, sizeof(label), uiText::goToPageRange, static_cast<unsigned long>(currentPage + 1), pageCount);
    } else {
      const xtch::ChapterInfo& chapter = chapters[static_cast<size_t>(i - 1)];
      if (chapter.name.empty()) {
        snprintf(label, sizeof(label), uiText::chapterN, i, chapter.startPage + 1, chapter.endPage + 1);
      } else {
        snprintf(label, sizeof(label), uiText::chapterNamed, chapter.name.c_str(), chapter.startPage + 1,
                 chapter.endPage + 1);
      }
    }
    label[utf8SafeTruncateBuffer(label, static_cast<int>(strlen(label)))] = '\0';
  };
  for (int i = window; i < last; ++i) {
    fillLabel(i);
    ui::drawRow(gfx, top + (i - window) * rowH, rowH, label, i == index);
  }
  drawNotice();
  presentUi();
}
