#include "core/UiText.h"

#include <cstddef>
#include <cstring>

#include "core/Settings.h"

namespace uiText {

#define UI_PTR(name, en, ja, zh) const char* name = en;
UI_STRINGS(UI_PTR)
#undef UI_PTR

const char* languageName = "English";

void apply() {
#define UI_EN(name, en, ja, zh) en,
#define UI_JA(name, en, ja, zh) ja,
#define UI_ZH(name, en, ja, zh) zh,
#define UI_SLOT(name, en, ja, zh) &name,
  static const char* const kEn[] = {UI_STRINGS(UI_EN)};
  static const char* const kJa[] = {UI_STRINGS(UI_JA)};
  static const char* const kZh[] = {UI_STRINGS(UI_ZH)};
  static const char** const kSlot[] = {UI_STRINGS(UI_SLOT)};
#undef UI_EN
#undef UI_JA
#undef UI_ZH
#undef UI_SLOT

  const char* const* row = kEn;
  if (::settings.language == Settings::kLanguageJapanese) {
    row = kJa;
  } else if (::settings.language == Settings::kLanguageChinese) {
    row = kZh;
  }
  for (size_t i = 0; i < sizeof(kSlot) / sizeof(kSlot[0]); ++i) {
    *kSlot[i] = row[i];
  }
  if (row == kJa) {
    languageName = "日本語";
  } else if (row == kZh) {
    languageName = "中文";
  } else {
    languageName = "English";
  }
}

const char* error(const char* en) {
  if (!en || en[0] == '\0') {
    return "";
  }
  static const struct {
    const char* key;
    const char* const* text;
  } kMap[] = {
      {"file not found", &fileNotFound},
      {"out of memory", &outOfMemory},
      {"invalid firmware", &invalidFirmware},
      {"write failed", &writeFailed},
      {"could not open file", &couldNotOpenFile},
      {"could not read file", &couldNotReadFile},
      {"read error", &couldNotReadFile},
      {"invalid magic", &invalidFormat},
      {"not an ESP32 image", &invalidFormat},
      {"unsupported version", &unsupportedVersion},
      {"corrupted header", &corrupted},
      {"page out of range", &pageOutOfRange},
      {"page larger than screen", &pageTooLarge},
      {"page decompression failed", &decodeFailed},
      {"file too small", &fileTooSmall},
      {"file too large", &fileTooLarge},
      {"wrong device", &wrongDevice},
      {"SD card error", &sdCardError},
  };
  for (const auto& e : kMap) {
    if (strcmp(en, e.key) == 0) {
      return *e.text;
    }
  }
  return unknownError;
}

const char* xtchError(xtch::Error err) {
  switch (err) {
    case xtch::Error::FileNotFound:
      return fileNotFound;
    case xtch::Error::InvalidMagic:
      return invalidFormat;
    case xtch::Error::InvalidVersion:
      return unsupportedVersion;
    case xtch::Error::CorruptedHeader:
      return corrupted;
    case xtch::Error::PageOutOfRange:
      return pageOutOfRange;
    case xtch::Error::ReadError:
      return couldNotReadFile;
    case xtch::Error::TooLarge:
      return pageTooLarge;
    case xtch::Error::OutOfMemory:
      return outOfMemory;
    case xtch::Error::DecodeFailed:
      return decodeFailed;
    case xtch::Error::Ok:
    default:
      return unknownError;
  }
}

}  // namespace uiText
