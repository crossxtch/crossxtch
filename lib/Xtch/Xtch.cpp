#include "Xtch.h"

#include <Gfx.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "puff.h"

uint8_t* XtchBook::pageBuffer = nullptr;
size_t XtchBook::pageBufferCapacity = 0;

bool isXtchPath(const char* path) {
  if (!path) {
    return false;
  }
  const size_t n = std::strlen(path);
  return n >= 5 && strcasecmp(path + (n - 5), ".xtch") == 0;
}

namespace {
// Round buffer growth up to a fixed granularity so repeated regrowth (compressed
// page sizes vary page-to-page) converges on a small set of block sizes instead
// of a new exact byte count every time a slightly-bigger page is seen. Most
// "new max" pages then already fit the current (rounded-up) capacity and need
// no malloc/free at all, which is what was fragmenting the heap: distinctly
// sized free()/malloc() cycles scattered variously-sized holes through the same
// region, so a later ~18-20 KB request could fail even with more than that much
// free heap in aggregate (see XTCH OOM logs with freeHeap >> largestFreeBlock).
constexpr size_t kBufferGrowthBlock = 4096;
size_t roundUpToBlock(const size_t size) {
  return (size + (kBufferGrowthBlock - 1)) & ~(kBufferGrowthBlock - 1);
}

void logHeap(const char* what) {
  LOG_INF("XTCH", "%s: freeHeap=%u largestFreeBlock=%u", what, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

// SD window for puff_stream(): small enough to malloc from a fragmented heap
// (the previous path needed a 20-30 KB contiguous rawBuffer and failed with
// freeHeap=27 KB / largestFreeBlock=13 KB), large enough for a few FAT sectors.
constexpr size_t kInflateChunk = 4096;

struct DeflateIn {
  HalFile* file;
  size_t remaining;
};

int refillDeflate(unsigned char* buf, unsigned long cap, void* user) {
  auto* in = static_cast<DeflateIn*>(user);
  if (in->remaining == 0) {
    return 0;
  }
  const size_t n = cap < in->remaining ? static_cast<size_t>(cap) : in->remaining;
  const int got = in->file->read(buf, n);
  if (got <= 0) {
    return -1;
  }
  in->remaining -= static_cast<size_t>(got);
  return got;
}

// malloc the new block first so a failure keeps the existing buffer instead of
// dropping it on the floor (free-then-malloc lost the old page on OOM and then
// still had nothing to decode into).
bool growBuffer(uint8_t*& buf, size_t& cap, const size_t needed) {
  if (needed == 0) {
    return false;
  }
  if (buf != nullptr && cap >= needed) {
    return true;
  }
  const size_t newCap = roundUpToBlock(needed);
  uint8_t* next = static_cast<uint8_t*>(malloc(newCap));
  if (!next) {
    return false;
  }
  free(buf);
  buf = next;
  cap = newCap;
  return true;
}
}  // namespace

bool XtchBook::reserveScratchBuffers(const uint16_t maxWidth, const uint16_t maxHeight) {
  const size_t colBytes = (static_cast<size_t>(maxHeight) + 7) / 8;
  const size_t bitmapSize = colBytes * static_cast<size_t>(maxWidth) * 2;
  const size_t totalDecoded = sizeof(xtch::PageHeader) + bitmapSize;
  if (growBuffer(pageBuffer, pageBufferCapacity, totalDecoded)) {
    logHeap("Reserved page buffer");
    return true;
  }
  LOG_ERR("XTCH", "Failed to reserve page buffer (%lu bytes): freeHeap=%u largestFreeBlock=%u",
          static_cast<unsigned long>(totalDecoded), static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  return false;
}

void XtchBook::releaseScratchBuffers() {
  free(pageBuffer);
  pageBuffer = nullptr;
  pageBufferCapacity = 0;
  logHeap("Released scratch buffers");
}

XtchBook::~XtchBook() { close(); }

void XtchBook::closeFile() {
  if (file.isOpen()) {
    file.close();
  }
}

void XtchBook::close() {
  cleanupPending = false;
  closeFile();
  // pageBuffer is intentionally NOT freed here: it's a static scratch buffer
  // sized for the panel's worst-case page, kept alive for the app's lifetime
  // rather than reacquired (and possibly failing to find a large-enough
  // contiguous block) every time a book is reopened. Only the logical
  // "nothing decoded yet" state resets.
  loadedPageIndex = 0xFFFFFFFFu;
  pageTableWindowStart = 0;
  pageTableWindowCount = 0;
  clusterLruCount = 0;
  opened = false;
  defaultWidth = 0;
  defaultHeight = 0;
  bookTitle[0] = '\0';
  bookAuthor[0] = '\0';
  memset(&header, 0, sizeof(header));
  chapters.clear();
  chaptersAvailable = false;
  chaptersLoaded = false;
  chaptersBroken = false;
}

bool XtchBook::ensureOpen() {
  if (file.isOpen()) {
    return true;
  }
  return Storage.openFileForRead("XTCH", filepath, file);
}

xtch::Error XtchBook::open(const char* path) {
  close();
  if (!path || path[0] == '\0') {
    LOG_ERR("XTCH", "Empty path");
    error = xtch::Error::FileNotFound;
    return error;
  }
  snprintf(filepath, sizeof(filepath), "%s", path);

  if (!Storage.openFileForRead("XTCH", filepath, file)) {
    LOG_ERR("XTCH", "Not found %s", filepath);
    error = xtch::Error::FileNotFound;
    return error;
  }

  error = readHeader();
  if (error != xtch::Error::Ok) {
    closeFile();
    return error;
  }
  chaptersAvailable = (header.hasChapters == 1 && header.pageTableOffset >= sizeof(header));
  if (header.hasMetadata) {
    error = readMetadata();
    if (error != xtch::Error::Ok) {
      closeFile();
      return error;
    }
  }
  if (bookTitle[0] == '\0') {
    const char* slash = strrchr(filepath, '/');
    const char* name = slash ? slash + 1 : filepath;
    snprintf(bookTitle, sizeof(bookTitle), "%s", name);
  }

  error = loadPageTable();
  if (error != xtch::Error::Ok) {
    closeFile();
    LOG_ERR("XTCH", "Page table: %s", xtch::errorName(error));
    return error;
  }

  if (file.probeContiguous()) {
    LOG_DBG("XTCH", "File is contiguous");
  } else {
    LOG_DBG("XTCH", "File is not contiguous");
  }

  opened = true;
  LOG_INF("XTCH", "Opened %s (%u pages, %dx%d, title='%s') freeHeap=%u largestFreeBlock=%u", filepath,
          header.pageCount, defaultWidth, defaultHeight, bookTitle, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  return xtch::Error::Ok;
}

xtch::Error XtchBook::readHeader() {
  uint8_t raw[sizeof(xtch::Header)];
  const size_t n = static_cast<size_t>(file.read(raw, sizeof(raw)));
  if (n != sizeof(raw)) {
    LOG_ERR("XTCH", "Short header read (%u of %u)", static_cast<unsigned>(n), static_cast<unsigned>(sizeof(raw)));
    return xtch::Error::ReadError;
  }
  memcpy(&header, raw, sizeof(header));

  if (header.magic != xtch::XTCH_MAGIC) {
    LOG_ERR("XTCH", "Bad magic 0x%08lX (need XTCH)", static_cast<unsigned long>(header.magic));
    return xtch::Error::InvalidMagic;
  }

  const bool validVersion =
      (header.versionMajor == 1 && header.versionMinor == 0) || (header.versionMajor == 0 && header.versionMinor == 1);
  if (!validVersion) {
    LOG_ERR("XTCH", "Unsupported version %u.%u", header.versionMajor, header.versionMinor);
    return xtch::Error::InvalidVersion;
  }
  if (header.pageCount == 0) {
    LOG_ERR("XTCH", "Header pageCount is 0");
    return xtch::Error::CorruptedHeader;
  }
  return xtch::Error::Ok;
}

xtch::Error XtchBook::readMetadata() {
  char titleBuf[sizeof(bookTitle)] = {};
  if (!file.seek(0x38)) {
    LOG_ERR("XTCH", "Seek title failed");
    return xtch::Error::ReadError;
  }
  file.read(titleBuf, sizeof(titleBuf) - 1);
  snprintf(bookTitle, sizeof(bookTitle), "%s", titleBuf);

  char authorBuf[sizeof(bookAuthor)] = {};
  if (!file.seek(0xB8)) {
    LOG_ERR("XTCH", "Seek author failed");
    return xtch::Error::ReadError;
  }
  file.read(authorBuf, sizeof(authorBuf) - 1);
  snprintf(bookAuthor, sizeof(bookAuthor), "%s", authorBuf);
  return xtch::Error::Ok;
}

xtch::Error XtchBook::loadPageTable() {
  pageTableWindowStart = 0;
  pageTableWindowCount = 0;
  clusterLruCount = 0;
  if (header.pageCount == 0 || header.pageTableOffset == 0) {
    LOG_ERR("XTCH", "Page table missing (count=%u offset=%llu)", header.pageCount,
            static_cast<unsigned long long>(header.pageTableOffset));
    return xtch::Error::CorruptedHeader;
  }

  // First row only: default pixel size. The window is left empty so the first
  // real lookup (often a resumed page, not 0) recenters around that page.
  if (!file.seek64(header.pageTableOffset)) {
    LOG_ERR("XTCH", "Page table seek failed");
    return xtch::Error::ReadError;
  }
  xtch::PageTableEntry first{};
  if (static_cast<size_t>(file.read(reinterpret_cast<uint8_t*>(&first), sizeof(first))) != sizeof(first)) {
    LOG_ERR("XTCH", "First page table entry unreadable");
    return xtch::Error::ReadError;
  }
  defaultWidth = first.width;
  defaultHeight = first.height;
  return xtch::Error::Ok;
}

bool XtchBook::ensurePageTableWindow(const uint32_t pageIndex) {
  if (pageTableWindowCount > 0 && pageIndex >= pageTableWindowStart &&
      pageIndex < pageTableWindowStart + pageTableWindowCount) {
    return true;
  }
  if (header.pageCount == 0 || header.pageTableOffset == 0 || !ensureOpen()) {
    return false;
  }

  uint32_t start = pageIndex >= kPageTableLookbehind ? pageIndex - kPageTableLookbehind : 0;
  if (start + kPageTableWindowSize > header.pageCount) {
    start = header.pageCount > kPageTableWindowSize
                ? static_cast<uint32_t>(header.pageCount - kPageTableWindowSize)
                : 0;
  }
  const uint32_t remaining = static_cast<uint32_t>(header.pageCount) - start;
  const uint16_t count =
      remaining < kPageTableWindowSize ? static_cast<uint16_t>(remaining) : kPageTableWindowSize;
  if (count == 0) {
    return false;
  }

  const uint64_t offset =
      header.pageTableOffset + static_cast<uint64_t>(start) * sizeof(xtch::PageTableEntry);
  const size_t bytes = static_cast<size_t>(count) * sizeof(xtch::PageTableEntry);
  if (!file.seek64(offset) ||
      static_cast<size_t>(file.read(reinterpret_cast<uint8_t*>(pageTableWindow), bytes)) != bytes) {
    pageTableWindowCount = 0;
    LOG_ERR("XTCH", "Page table window read failed (start=%lu count=%u)", static_cast<unsigned long>(start),
            count);
    return false;
  }
  pageTableWindowStart = start;
  pageTableWindowCount = count;
  LOG_DBG("XTCH", "Page table window [%lu, %lu)", static_cast<unsigned long>(start),
          static_cast<unsigned long>(start + count));
  return true;
}

xtch::Error XtchBook::readPageTableEntry(const uint32_t pageIndex, xtch::PageInfo& info) {
  if (pageIndex >= header.pageCount) {
    return xtch::Error::PageOutOfRange;
  }
  if (header.pageTableOffset == 0) {
    return xtch::Error::CorruptedHeader;
  }
  if (ensurePageTableWindow(pageIndex)) {
    const xtch::PageTableEntry& entry = pageTableWindow[pageIndex - pageTableWindowStart];
    info.offset = entry.dataOffset;
    info.size = entry.dataSize;
    info.width = entry.width;
    info.height = entry.height;
    return xtch::Error::Ok;
  }
  // Window refill failed; still try a single row so the page can load.
  if (!ensureOpen()) {
    return xtch::Error::FileNotFound;
  }
  const uint64_t entryOffset =
      header.pageTableOffset + static_cast<uint64_t>(pageIndex) * sizeof(xtch::PageTableEntry);
  if (!file.seek64(entryOffset)) {
    LOG_ERR("XTCH", "Page table entry %lu seek failed", static_cast<unsigned long>(pageIndex));
    return xtch::Error::ReadError;
  }
  xtch::PageTableEntry entry{};
  if (static_cast<size_t>(file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(entry))) != sizeof(entry)) {
    LOG_ERR("XTCH", "Page table entry %lu unreadable", static_cast<unsigned long>(pageIndex));
    return xtch::Error::ReadError;
  }
  info.offset = entry.dataOffset;
  info.size = entry.dataSize;
  info.width = entry.width;
  info.height = entry.height;
  return xtch::Error::Ok;
}

uint32_t XtchBook::clusterLruLookup(const uint32_t pageIndex) {
  for (uint8_t i = 0; i < clusterLruCount; ++i) {
    if (clusterLru[i].pageIndex != pageIndex) {
      continue;
    }
    const uint32_t cluster = clusterLru[i].cluster;
    if (i != 0) {
      const ClusterLruSlot hit = clusterLru[i];
      memmove(&clusterLru[1], &clusterLru[0], static_cast<size_t>(i) * sizeof(ClusterLruSlot));
      clusterLru[0] = hit;
    }
    return cluster;
  }
  return 0;
}

void XtchBook::clusterLruRemember(const uint32_t pageIndex, const uint32_t cluster) {
  if (cluster == 0) {
    return;
  }
  uint8_t found = clusterLruCount;
  for (uint8_t i = 0; i < clusterLruCount; ++i) {
    if (clusterLru[i].pageIndex == pageIndex) {
      found = i;
      break;
    }
  }
  if (found == 0 && clusterLruCount > 0 && clusterLru[0].pageIndex == pageIndex) {
    clusterLru[0].cluster = cluster;
    return;
  }
  const ClusterLruSlot slot{pageIndex, cluster};
  if (found < clusterLruCount) {
    memmove(&clusterLru[1], &clusterLru[0], static_cast<size_t>(found) * sizeof(ClusterLruSlot));
    clusterLru[0] = slot;
    return;
  }
  const uint8_t shift = clusterLruCount < kClusterLruSize ? clusterLruCount : static_cast<uint8_t>(kClusterLruSize - 1);
  if (shift > 0) {
    memmove(&clusterLru[1], &clusterLru[0], static_cast<size_t>(shift) * sizeof(ClusterLruSlot));
  }
  clusterLru[0] = slot;
  if (clusterLruCount < kClusterLruSize) {
    ++clusterLruCount;
  }
}

bool XtchBook::pageInfo(const uint32_t pageIndex, xtch::PageInfo& info) {
  return readPageTableEntry(pageIndex, info) == xtch::Error::Ok;
}

xtch::Error XtchBook::loadPageData(const uint32_t pageIndex) {
  loadedPageIndex = 0xFFFFFFFFu;
  const uint32_t tTable = millis();
  xtch::PageInfo page{};
  const xtch::Error tableErr = readPageTableEntry(pageIndex, page);
  if (tableErr != xtch::Error::Ok) {
    return tableErr;
  }
  const uint32_t tableMs = millis() - tTable;
  if (!ensureOpen()) {
    return xtch::Error::FileNotFound;
  }

  const size_t colBytes = (static_cast<size_t>(page.height) + 7) / 8;
  const size_t planeSize = colBytes * static_cast<size_t>(page.width);
  const size_t bitmapSize = planeSize * 2;
  // Decoded size is always derived from width/height (never from what's on disk),
  // so pageBuffer's layout/size is unaffected by whether the page is compressed.
  const size_t totalDecoded = sizeof(xtch::PageHeader) + bitmapSize;
  // page.size is the actual on-disk block length for this page (header + body);
  // the body is raw bitplanes when uncompressed or a compressed blob otherwise.
  const size_t totalOnDisk = page.size;

  if (totalOnDisk < sizeof(xtch::PageHeader)) {
    LOG_ERR("XTCH", "Page %lu on-disk size %lu too small for header", static_cast<unsigned long>(pageIndex),
            static_cast<unsigned long>(totalOnDisk));
    return xtch::Error::CorruptedHeader;
  }

  if (!growBuffer(pageBuffer, pageBufferCapacity, totalDecoded)) {
    LOG_ERR("XTCH", "Failed to allocate page buffer (%lu bytes): freeHeap=%u largestFreeBlock=%u",
            static_cast<unsigned long>(totalDecoded), static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return xtch::Error::OutOfMemory;
  }

  const uint32_t tSeek = millis();
  bool clusterHit = false;
  const uint32_t cachedCluster = clusterLruLookup(pageIndex);
  if (cachedCluster != 0) {
    file.setPos(page.offset, cachedCluster);
    clusterHit = true;
  } else if (!file.seek64(page.offset)) {
    LOG_ERR("XTCH", "Seek page %lu offset %llu failed", static_cast<unsigned long>(pageIndex),
            static_cast<unsigned long long>(page.offset));
    return xtch::Error::ReadError;
  } else {
    uint64_t pos = 0;
    uint32_t cluster = 0;
    if (file.getPos(&pos, &cluster)) {
      clusterLruRemember(pageIndex, cluster);
    }
  }
  const uint32_t seekMs = millis() - tSeek;

  // Read the 22-byte page header first so uncompressed pages can stream the
  // body straight into pageBuffer. Compressed pages inflate through a 4 KB
  // window (puff_stream) instead of malloc'ing a second 20-30 KB rawBuffer —
  // that second block is what OOMed after the framebuffer and page table had
  // fragmented the heap, even when freeHeap still looked comfortable.
  xtch::PageHeader pageHeader{};
  if (static_cast<size_t>(file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(pageHeader))) !=
      sizeof(pageHeader)) {
    LOG_ERR("XTCH", "Short page header read for page %lu", static_cast<unsigned long>(pageIndex));
    return xtch::Error::ReadError;
  }
  if (pageHeader.magic != xtch::XTH_MAGIC) {
    LOG_ERR("XTCH", "Bad page magic 0x%08lX (need XTH)", static_cast<unsigned long>(pageHeader.magic));
    return xtch::Error::InvalidMagic;
  }
  if (pageHeader.width != page.width || pageHeader.height != page.height) {
    LOG_ERR("XTCH", "Page %lu size mismatch: header=%ux%u table=%ux%u", static_cast<unsigned long>(pageIndex),
            pageHeader.width, pageHeader.height, page.width, page.height);
    return xtch::Error::CorruptedHeader;
  }

  memcpy(pageBuffer, &pageHeader, sizeof(pageHeader));
  uint8_t* decodedBody = pageBuffer + sizeof(xtch::PageHeader);
  const uint32_t hdrMs = millis() - tSeek - seekMs;
  const uint32_t tBody = millis();

  if (pageHeader.compression == 0) {
    // Raw bitplanes stored as-is: body length must match the decoded size exactly.
    if (pageHeader.dataSize != bitmapSize || totalOnDisk != totalDecoded) {
      LOG_ERR("XTCH", "Unsupported page %lu encoding: compression=0 dataSize=%lu expected=%lu",
              static_cast<unsigned long>(pageIndex), static_cast<unsigned long>(pageHeader.dataSize),
              static_cast<unsigned long>(bitmapSize));
      return xtch::Error::CorruptedHeader;
    }
    if (static_cast<size_t>(file.read(decodedBody, bitmapSize)) != bitmapSize) {
      LOG_ERR("XTCH", "Short page body read for page %lu", static_cast<unsigned long>(pageIndex));
      return xtch::Error::ReadError;
    }
  } else if (pageHeader.compression == 1) {
    // Raw-DEFLATE (no zlib/gzip wrapper): dataSize is the on-disk compressed body
    // length; the decompressed length is always bitmapSize (derived from width/height).
    const size_t compressedSize = static_cast<size_t>(pageHeader.dataSize);
    if (compressedSize + sizeof(xtch::PageHeader) != totalOnDisk) {
      LOG_ERR("XTCH", "Page %lu compressed size mismatch: dataSize=%lu on-disk=%lu",
              static_cast<unsigned long>(pageIndex), static_cast<unsigned long>(pageHeader.dataSize),
              static_cast<unsigned long>(totalOnDisk));
      return xtch::Error::CorruptedHeader;
    }
    static uint8_t inflateChunk[kInflateChunk];
    DeflateIn in{&file, compressedSize};
    unsigned long destLen = static_cast<unsigned long>(bitmapSize);
    const int puffErr =
        puff_stream(decodedBody, &destLen, refillDeflate, &in, inflateChunk, sizeof(inflateChunk));
    if (puffErr != 0 || destLen != bitmapSize) {
      LOG_ERR("XTCH", "Page %lu decompression failed: err=%d decoded=%lu expected=%lu remaining=%u",
              static_cast<unsigned long>(pageIndex), puffErr, destLen, static_cast<unsigned long>(bitmapSize),
              static_cast<unsigned>(in.remaining));
      return puffErr > 0 && in.remaining > 0 ? xtch::Error::ReadError : xtch::Error::DecodeFailed;
    }
  } else {
    LOG_ERR("XTCH", "Page %lu has unsupported compression id %u", static_cast<unsigned long>(pageIndex),
            pageHeader.compression);
    return xtch::Error::CorruptedHeader;
  }

  loadedPageIndex = pageIndex;
  LOG_INF("PERF", "load p=%lu comp=%u cluster=%d table=%lums seek=%lums hdr=%lums body=%lums disk=%lu dec=%lu",
          static_cast<unsigned long>(pageIndex + 1), pageHeader.compression, clusterHit ? 1 : 0,
          static_cast<unsigned long>(tableMs), static_cast<unsigned long>(seekMs), static_cast<unsigned long>(hdrMs),
          static_cast<unsigned long>(millis() - tBody), static_cast<unsigned long>(totalOnDisk),
          static_cast<unsigned long>(totalDecoded));
  return xtch::Error::Ok;
}

// 96-byte entries at chapterOffset|padding<<32: 80-byte name, startPage, endPage, 12 reserved.
void XtchBook::readChapters() {
  chapters.clear();
  chaptersLoaded = true;
  chaptersBroken = false;
  if (!chaptersAvailable) {
    return;
  }
  const uint64_t chapterOffset =
      static_cast<uint64_t>(header.chapterOffset) | (static_cast<uint64_t>(header.padding) << 32);
  if (chapterOffset == 0) {
    chaptersAvailable = false;
    return;
  }
  if (!ensureOpen()) {
    LOG_ERR("XTCH", "Chapter table reopen failed");
    chaptersAvailable = false;
    chaptersBroken = true;
    return;
  }
  constexpr uint64_t kChapterEntrySize = 96;
  const uint64_t fileSize = file.fileSize64();
  if (chapterOffset < sizeof(header) || chapterOffset >= fileSize || chapterOffset + kChapterEntrySize > fileSize) {
    LOG_ERR("XTCH", "Chapter table offset %llu outside file (%llu)", static_cast<unsigned long long>(chapterOffset),
            static_cast<unsigned long long>(fileSize));
    chaptersAvailable = false;
    chaptersBroken = true;
    closeFile();
    return;
  }

  // Clamp to whichever known table follows the chapter table so a bogus
  // header can't inflate the derived chapter count.
  uint64_t maxOffset = fileSize;
  if (header.pageTableOffset > chapterOffset && header.pageTableOffset <= fileSize) {
    maxOffset = header.pageTableOffset;
  } else if (header.dataOffset > chapterOffset && header.dataOffset <= fileSize) {
    maxOffset = header.dataOffset;
  }
  if (maxOffset <= chapterOffset || !file.seek64(chapterOffset)) {
    LOG_ERR("XTCH", "Chapter table seek failed");
    chaptersAvailable = false;
    chaptersBroken = true;
    closeFile();
    return;
  }

  const size_t chapterCount = static_cast<size_t>((maxOffset - chapterOffset) / kChapterEntrySize);
  if (chapterCount == 0 || chapterCount > header.pageCount) {
    if (chapterCount > header.pageCount) {
      LOG_ERR("XTCH", "Chapter count %u exceeds pageCount %u", static_cast<unsigned>(chapterCount), header.pageCount);
      chaptersBroken = true;
    }
    chaptersAvailable = false;
    closeFile();
    return;
  }

  chapters.reserve(chapterCount);
  uint8_t buf[kChapterEntrySize];
  for (size_t i = 0; i < chapterCount; ++i) {
    if (static_cast<size_t>(file.read(buf, sizeof(buf))) != sizeof(buf)) {
      LOG_ERR("XTCH", "Short chapter read at %u of %u", static_cast<unsigned>(i), static_cast<unsigned>(chapterCount));
      chaptersBroken = true;
      break;
    }
    char nameBuf[81];
    memcpy(nameBuf, buf, 80);
    nameBuf[80] = '\0';
    std::string name(nameBuf, strnlen(nameBuf, 80));

    uint16_t startPage = 0;
    uint16_t endPage = 0;
    memcpy(&startPage, buf + 0x50, sizeof(startPage));
    memcpy(&endPage, buf + 0x52, sizeof(endPage));
    if (name.empty() && startPage == 0 && endPage == 0) {
      break;  // trailing unused entries
    }

    // On-disk pages are 1-based; PageInfo/drawPage use 0-based indices.
    if (startPage > 0) {
      --startPage;
    }
    if (endPage > 0) {
      --endPage;
    }
    if (startPage >= header.pageCount) {
      continue;
    }
    if (endPage >= header.pageCount) {
      endPage = header.pageCount - 1;
    }
    if (startPage > endPage) {
      continue;
    }
    chapters.push_back(xtch::ChapterInfo{std::move(name), startPage, endPage});
  }
  chaptersAvailable = !chapters.empty();
  closeFile();
  LOG_DBG("XTCH", "Chapters: %u", static_cast<unsigned>(chapters.size()));
}

const std::vector<xtch::ChapterInfo>& XtchBook::getChapters() {
  if (!chaptersLoaded) {
    readChapters();
  }
  return chapters;
}

namespace {
enum class PlaneOp : uint8_t { Ink, Lsb, Msb };

// Full-frame pages match logical size (X3 528×792, X4 480×800). XTCH columns
// are stored in the same order as panel rows after the 90° map, so each source
// byte is already one framebuffer byte.
void blitFullFrame(uint8_t* fb, const uint8_t* plane1, const uint8_t* plane2, const size_t colBytes,
                   const uint16_t rows, const PlaneOp op) {
  for (uint16_t row = 0; row < rows; ++row) {
    const uint8_t* p1 = plane1 + static_cast<size_t>(row) * colBytes;
    const uint8_t* p2 = plane2 + static_cast<size_t>(row) * colBytes;
    uint8_t* dst = fb + static_cast<size_t>(row) * colBytes;
    switch (op) {
      case PlaneOp::Ink:
        for (size_t k = 0; k < colBytes; ++k) {
          dst[k] = static_cast<uint8_t>(~(p1[k] | p2[k]));
        }
        break;
      case PlaneOp::Lsb:
        for (size_t k = 0; k < colBytes; ++k) {
          dst[k] = static_cast<uint8_t>(static_cast<uint8_t>(~p1[k]) & p2[k]);
        }
        break;
      case PlaneOp::Msb:
        for (size_t k = 0; k < colBytes; ++k) {
          dst[k] = static_cast<uint8_t>(p1[k] ^ p2[k]);
        }
        break;
    }
  }
}
}  // namespace

bool XtchBook::drawPage(Gfx& gfx, const uint32_t pageIndex, int& pagesUntilFullRefresh, const int refreshFrequency) {
  if (!opened) {
    LOG_ERR("XTCH", "drawPage but book is closed");
    error = xtch::Error::FileNotFound;
    return false;
  }

  auto fail = [this](const xtch::Error err) {
    loadedPageIndex = 0xFFFFFFFFu;
    closeFile();
    error = err;
    return false;
  };

  // A prior page's cleanup may still be pending. The new page and an error
  // screen both present, and both need the DTM banks already resynced. Its
  // PERF cleanup line is not part of this page's total.
  flushPendingCleanup(gfx);

  xtch::PageInfo page{};
  const xtch::Error tableErr = readPageTableEntry(pageIndex, page);
  if (tableErr != xtch::Error::Ok) {
    LOG_ERR("XTCH", "Page %lu %s", static_cast<unsigned long>(pageIndex), xtch::errorName(tableErr));
    return fail(tableErr);
  }
  if (static_cast<int>(page.width) > gfx.width() || static_cast<int>(page.height) > gfx.height()) {
    LOG_ERR("XTCH", "Page %lu is %ux%u, screen %dx%d", static_cast<unsigned long>(pageIndex), page.width, page.height,
            gfx.width(), gfx.height());
    return fail(xtch::Error::TooLarge);
  }
  if (!ensureOpen()) {
    LOG_ERR("XTCH", "Reopen failed for page %lu", static_cast<unsigned long>(pageIndex));
    return fail(xtch::Error::FileNotFound);
  }

  const uint16_t pageWidth = page.width;
  const uint16_t pageHeight = page.height;
  const size_t colBytes = (static_cast<size_t>(pageHeight) + 7) / 8;
  const size_t planeSize = colBytes * static_cast<size_t>(pageWidth);
  const bool prefetched = loadedPageIndex == pageIndex && pageBuffer != nullptr;

  const uint32_t tPage = millis();

  if (!prefetched) {
    xtch::Error loadErr = loadPageData(pageIndex);
    // One reopen. A wedged file handle or a stale FAT cluster otherwise fails
    // the same way on the next attempt, and the cluster cache is what setPos
    // would reuse.
    if (loadErr == xtch::Error::ReadError || loadErr == xtch::Error::FileNotFound) {
      LOG_INF("XTCH", "Retry page %lu after %s", static_cast<unsigned long>(pageIndex), xtch::errorName(loadErr));
      closeFile();
      clusterLruCount = 0;
      loadErr = loadPageData(pageIndex);
    }
    if (loadErr != xtch::Error::Ok) {
      return fail(loadErr);
    }
  }

  const uint8_t* plane1 = pageBuffer + sizeof(xtch::PageHeader);
  const uint8_t* plane2 = plane1 + planeSize;
  const int ox = (gfx.width() - static_cast<int>(pageWidth)) / 2;
  const int oy = (gfx.height() - static_cast<int>(pageHeight)) / 2;
  uint8_t* fb = gfx.frameBuffer();
  const bool fullFrame = fb != nullptr && ox == 0 && oy == 0 && static_cast<int>(pageWidth) == gfx.width() &&
                         static_cast<int>(pageHeight) == gfx.height() && (pageHeight % 8) == 0 &&
                         gfx.stride() == static_cast<uint16_t>(colBytes);

  auto paint = [&](const PlaneOp op) {
    if (fullFrame) {
      blitFullFrame(fb, plane1, plane2, colBytes, pageWidth, op);
      return;
    }
    const bool ink = op == PlaneOp::Ink;
    gfx.clear(!ink);
    for (uint16_t y = 0; y < pageHeight; ++y) {
      const size_t byteInCol = y / 8;
      const uint8_t bitInByte = static_cast<uint8_t>(7 - (y % 8));
      size_t byteOffset = static_cast<size_t>(pageWidth - 1) * colBytes + byteInCol;
      for (uint16_t x = 0; x < pageWidth; ++x, byteOffset -= colBytes) {
        const uint8_t pv = static_cast<uint8_t>(((plane1[byteOffset] >> bitInByte) & 1) << 1 |
                                               ((plane2[byteOffset] >> bitInByte) & 1));
        const bool keep = op == PlaneOp::Ink ? pv >= 1 : op == PlaneOp::Lsb ? pv == 1 : (pv == 1 || pv == 2);
        if (keep) {
          gfx.drawPixel(ox + x, oy + y, ink);
        }
      }
    }
  };

  auto cpuMs = [](auto&& fn) {
    const uint32_t t0 = millis();
    fn();
    return static_cast<unsigned long>(millis() - t0);
  };

  const unsigned long inkMs = cpuMs([&] { paint(PlaneOp::Ink); });

  const char* mode = "fast";
  // UC8253 returns from start once BUSY is low and waits in finish. The
  // blocking base rewrites DTM1 after that wait; the LSB copy replaces it, so
  // the split skips the extra plane (~45 ms). Other panels block in start and
  // finish is empty. CPU paint between the two is fine. SPI is not: the
  // controller is mid-refresh, and this bus is shared with the SD card.
  bool baseOpen = false;
  if (pagesUntilFullRefresh <= 1) {
    // Clean base before gray planes so ghosting doesn't accumulate.
    mode = "half";
    if (gfx.combinesGrayscaleBase()) {
      gfx.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
    } else {
      gfx.present(HalDisplay::HALF_REFRESH);
      gfx.preconditionGrayscale();
    }
    pagesUntilFullRefresh = refreshFrequency;
  } else {
    gfx.startGrayscaleBase(HalDisplay::FAST_REFRESH);
    baseOpen = true;
    --pagesUntilFullRefresh;
  }

  const unsigned long lsbMs = cpuMs([&] { paint(PlaneOp::Lsb); });
  if (baseOpen) {
    gfx.finishGrayscaleBase();
  }
  gfx.copyGrayscaleLsbBuffers();

  const unsigned long msbMs = cpuMs([&] { paint(PlaneOp::Msb); });
  gfx.copyGrayscaleMsbBuffers();

  gfx.startGrayBuffer();
  const unsigned long rebuildMs = cpuMs([&] { paint(PlaneOp::Ink); });
  gfx.finishGrayBuffer();

  // Deferred: run on the next idle tick (flushPendingCleanup) instead of here,
  // so its SPI housekeeping doesn't block the page the user is waiting on.
  // drawPage() itself flushes it defensively before the next page.
  cleanupPending = true;

  error = xtch::Error::Ok;

  // ink/lsb/msb/rebuild are CPU paint only. Panel and SPI times are the PERF
  // lines printed by those calls (base, panel, precondition, spi-*, gray).
  LOG_INF("PERF", "page %lu/%u %s hit=%d full=%d mhz=%u ink=%lums lsb=%lums msb=%lums rebuild=%lums total=%lums",
          static_cast<unsigned long>(pageIndex + 1), header.pageCount, mode, prefetched ? 1 : 0, fullFrame ? 1 : 0,
          static_cast<unsigned>(getCpuFrequencyMhz()), inkMs, lsbMs, msbMs, rebuildMs,
          static_cast<unsigned long>(millis() - tPage));
  return true;
}

void XtchBook::flushPendingCleanup(Gfx& gfx) {
  if (!cleanupPending) {
    return;
  }
  gfx.cleanupGrayscaleBuffers();
  cleanupPending = false;
}

void XtchBook::prefetchForward(const uint32_t fromPageIndex) {
  if (!opened || fromPageIndex + 1 >= header.pageCount) {
    return;
  }
  if (loadedPageIndex == fromPageIndex + 1 && pageBuffer != nullptr) {
    return;
  }
  const xtch::Error err = loadPageData(fromPageIndex + 1);
  if (err != xtch::Error::Ok) {
    LOG_DBG("XTCH", "Prefetch page %lu failed: %s", static_cast<unsigned long>(fromPageIndex + 1),
            xtch::errorName(err));
    // Drop a handle that died mid-read so the next draw opens clean. The draw
    // path retries once; doing it here as well would read the page four times.
    if (err == xtch::Error::ReadError || err == xtch::Error::FileNotFound) {
      closeFile();
    }
  }
}
