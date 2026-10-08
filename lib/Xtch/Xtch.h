#pragma once

#include <HalStorage.h>

#include <vector>

#include "XtchTypes.h"

class Gfx;

// Path ends in ".xtch", case-insensitive.
bool isXtchPath(const char* path);

// Streaming XTCH reader. The source file stays open while the book is open so
// page turns do not re-walk FAT; close() releases it.
class XtchBook {
 public:
  XtchBook() = default;
  ~XtchBook();

  // Allocate the decoded-page scratch buffer from a still-contiguous heap.
  // Call from setup() before display.begin() so the framebuffer cannot split
  // the heap underneath it. loadPageData() grows it lazily if this is skipped.
  static bool reserveScratchBuffers(uint16_t maxWidth, uint16_t maxHeight);

  // Give the decoded-page scratch back so Wi-Fi/mDNS can start. Pair with
  // WifiSession::end() — this heap cannot be unfragmented in place.
  static void releaseScratchBuffers();

  xtch::Error open(const char* path);
  void close();
  bool isOpen() const { return opened; }

  uint16_t pageCount() const { return header.pageCount; }
  uint16_t pageWidth() const { return defaultWidth; }
  uint16_t pageHeight() const { return defaultHeight; }
  const char* title() const { return bookTitle; }
  const char* author() const { return bookAuthor; }
  const char* path() const { return filepath; }
  xtch::Error lastError() const { return error; }

  bool pageInfo(uint32_t pageIndex, xtch::PageInfo& info);

  // Header flag only; getChapters() parses once and caches.
  bool hasChapters() const { return chaptersAvailable; }
  // True after getChapters() if the table was present but could not be read.
  bool chapterReadFailed() const { return chaptersBroken; }
  const std::vector<xtch::ChapterInfo>& getChapters();

  // Paints a 2-bit page and runs the display sequence; caller must not present() on success.
  // pagesUntilFullRefresh is decremented each call and reset to refreshFrequency at the periodic full refresh.
  bool drawPage(Gfx& gfx, uint32_t pageIndex, int& pagesUntilFullRefresh, int refreshFrequency);

  // Best-effort load of fromPageIndex+1 into pageBuffer. No-op if already loaded
  // or past the end. Call after drawPage when the next turn is likely +1.
  void prefetchForward(uint32_t fromPageIndex);

  // Runs the deferred post-gray-refresh RAM cleanup if drawPage() left one
  // pending. No-op otherwise. Call opportunistically on idle ticks so the
  // SPI housekeeping happens before the user's next page turn instead of
  // blocking the page that just rendered.
  bool pendingCleanup() const { return cleanupPending; }
  void flushPendingCleanup(Gfx& gfx);

 private:
  char filepath[256]{};
  char bookTitle[128]{};
  char bookAuthor[64]{};
  HalFile file;
  xtch::Header header{};
  uint16_t defaultWidth = 0;
  uint16_t defaultHeight = 0;
  bool opened = false;
  xtch::Error error = xtch::Error::Ok;
  std::vector<xtch::ChapterInfo> chapters;
  bool chaptersAvailable = false;
  bool chaptersLoaded = false;
  bool chaptersBroken = false;
  // Shared decoded-page scratch, sized for the panel. Kept across book close()
  // so the next open does not have to find a ~100 KB hole in a fragmented heap.
  static uint8_t* pageBuffer;
  static size_t pageBufferCapacity;
  uint32_t loadedPageIndex = 0xFFFFFFFFu;

  // Spatial page-table window: 4 behind / 59 ahead of the miss, 1 KB, O(1) in
  // book length. Refilled on miss (draw, prefetch N+1, or jump).
  static constexpr uint16_t kPageTableWindowSize = 64;
  static constexpr uint16_t kPageTableLookbehind = 4;
  xtch::PageTableEntry pageTableWindow[kPageTableWindowSize]{};
  uint32_t pageTableWindowStart = 0;
  uint16_t pageTableWindowCount = 0;

  // FAT cluster LRU of pages actually visited. Clusters are not in the file,
  // so this is not a spatial window. 0 means "unknown / not cached".
  static constexpr uint16_t kClusterLruSize = 32;
  struct ClusterLruSlot {
    uint32_t pageIndex;
    uint32_t cluster;
  };
  ClusterLruSlot clusterLru[kClusterLruSize]{};
  uint8_t clusterLruCount = 0;

  bool cleanupPending = false;

  bool ensureOpen();
  void closeFile();
  xtch::Error readHeader();
  xtch::Error readMetadata();
  xtch::Error loadPageTable();
  bool ensurePageTableWindow(uint32_t pageIndex);
  // PageOutOfRange when the index is past pageCount. ReadError / FileNotFound
  // when the row cannot be read. CorruptedHeader when the table offset is 0.
  xtch::Error readPageTableEntry(uint32_t pageIndex, xtch::PageInfo& info);
  uint32_t clusterLruLookup(uint32_t pageIndex);
  void clusterLruRemember(uint32_t pageIndex, uint32_t cluster);
  xtch::Error loadPageData(uint32_t pageIndex);
  void readChapters();
};
