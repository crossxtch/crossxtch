#pragma once

#include <XtchTypes.h>

#include <vector>

#include "core/Screen.h"

class ReaderScreen;

class ChapterSelectionScreen final : public Screen {
  ReaderScreen& reader;
  std::vector<xtch::ChapterInfo> chapters;
  uint32_t currentPage;
  uint16_t pageCount;
  int index = 0;
  int window = 0;

  void activate();

 public:
  ChapterSelectionScreen(Gfx& gfx, MappedInput& input, ReaderScreen& reader,
                         const std::vector<xtch::ChapterInfo>& chapterList, uint32_t currentPage,
                         uint16_t pageCount, bool unreadable = false);
  void loop() override;
  void render() override;
};
