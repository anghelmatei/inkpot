#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Xtc.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ScreenComponents.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
int clampPercent(const int value) {
  if (value < 0) return 0;
  if (value > 100) return 100;
  return value;
}

int getEpubProgressPercent(const std::string& path) {
  Epub epub(path, "/.crosspoint");
  if (!epub.load(false)) {
    return -1;
  }

  FsFile f;
  if (!SdMan.openFileForRead("HME", epub.getCachePath() + "/progress.bin", f)) {
    return -1;
  }

  uint8_t data[6];
  const int size = f.read(data, 6);
  f.close();
  if (size < 4) {
    return -1;
  }

  const int spineIndex = data[0] + (data[1] << 8);
  const int page = data[2] + (data[3] << 8);
  const int pageCount = (size >= 6) ? (data[4] + (data[5] << 8)) : 0;
  if (pageCount <= 0) {
    return -1;
  }

  const float chapterProg = (static_cast<float>(page) + 0.5f) / static_cast<float>(pageCount);
  const float progress = epub.calculateProgress(spineIndex, chapterProg) * 100.0f;
  return clampPercent(static_cast<int>(std::round(progress)));
}

int getXtcProgressPercent(const std::string& path) {
  Xtc xtc(path, "/.crosspoint");
  if (!xtc.load()) {
    return -1;
  }

  FsFile f;
  if (!SdMan.openFileForRead("HMX", xtc.getCachePath() + "/progress.bin", f)) {
    return -1;
  }

  uint8_t data[4];
  if (f.read(data, 4) != 4) {
    f.close();
    return -1;
  }
  f.close();

  const uint32_t page = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
  const uint32_t pageCount = xtc.getPageCount();
  if (pageCount == 0) {
    return -1;
  }

  const float progress = (static_cast<float>(page + 1) * 100.0f) / static_cast<float>(pageCount);
  return clampPercent(static_cast<int>(std::round(progress)));
}

int getBookProgressPercent(const std::string& path) {
  if (StringUtils::checkFileExtension(path, ".epub")) {
    return getEpubProgressPercent(path);
  }
  if (StringUtils::checkFileExtension(path, ".xtc") || StringUtils::checkFileExtension(path, ".xtch")) {
    return getXtcProgressPercent(path);
  }
  return -1;
}
}  // namespace

void HomeActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HomeActivity*>(param);
  self->displayTaskLoop();
}

int HomeActivity::getMenuItemCount() const {
  int count = 3;  // Bookshelf, File transfer, Settings
  if (hasContinueReading) count++;
  if (hasOpdsUrl) count++;
  return count;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Check if we have a book to continue reading
  hasContinueReading = !APP_STATE.openEpubPath.empty() && SdMan.exists(APP_STATE.openEpubPath.c_str());

  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;

  if (hasContinueReading) {
    // Extract filename from path for display
    lastBookTitle = APP_STATE.openEpubPath;
    const size_t lastSlash = lastBookTitle.find_last_of('/');
    if (lastSlash != std::string::npos) {
      lastBookTitle = lastBookTitle.substr(lastSlash + 1);
    }

    // If epub, try to load the metadata for title/author and cover
    if (StringUtils::checkFileExtension(lastBookTitle, ".epub")) {
      Epub epub(APP_STATE.openEpubPath, "/.crosspoint");
      epub.load(false);
      if (!epub.getTitle().empty()) {
        lastBookTitle = std::string(epub.getTitle());
      }
      if (!epub.getAuthor().empty()) {
        lastBookAuthor = std::string(epub.getAuthor());
      }
      // Try to generate thumbnail image for Continue Reading card
      if (epub.generateThumbBmp()) {
        coverBmpPath = epub.getThumbBmpPath();
        hasCoverImage = true;
      }
    } else if (StringUtils::checkFileExtension(lastBookTitle, ".xtch") ||
               StringUtils::checkFileExtension(lastBookTitle, ".xtc")) {
      // Handle XTC file
      Xtc xtc(APP_STATE.openEpubPath, "/.crosspoint");
      if (xtc.load()) {
        if (!xtc.getTitle().empty()) {
          lastBookTitle = std::string(xtc.getTitle());
        }
        if (!xtc.getAuthor().empty()) {
          lastBookAuthor = std::string(xtc.getAuthor());
        }
        // Try to generate thumbnail image for Continue Reading card
        if (xtc.generateThumbBmp()) {
          coverBmpPath = xtc.getThumbBmpPath();
          hasCoverImage = true;
        }
      }
      // Remove extension from title if we don't have metadata
      if (StringUtils::checkFileExtension(lastBookTitle, ".xtch")) {
        lastBookTitle.resize(lastBookTitle.length() - 5);
      } else if (StringUtils::checkFileExtension(lastBookTitle, ".xtc")) {
        lastBookTitle.resize(lastBookTitle.length() - 4);
      }
    }
  }

  continueReadingLabel = "Continue Reading";
  if (hasContinueReading) {
    const int progressPercent = getBookProgressPercent(APP_STATE.openEpubPath);
    if (progressPercent >= 0) {
      continueReadingLabel += " (" + std::to_string(progressPercent) + "%)";
    }
  }

  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&HomeActivity::taskTrampoline, "HomeActivityTask",
              4096,               // Stack size (increased for cover image rendering)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  // Free any existing buffer first
  freeCoverBuffer();

  const size_t bufferSize = GfxRenderer::getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) {
    return false;
  }

  memcpy(coverBuffer, frameBuffer, bufferSize);
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) {
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  const size_t bufferSize = GfxRenderer::getBufferSize();
  memcpy(frameBuffer, coverBuffer, bufferSize);
  return true;
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Right);

  const int menuCount = getMenuItemCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Calculate dynamic indices based on which options are available
    int idx = 0;
    const int continueIdx = hasContinueReading ? idx++ : -1;
    const int myLibraryIdx = idx++;
    const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
    const int fileTransferIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex == continueIdx) {
      onContinueReading();
    } else if (selectorIndex == myLibraryIdx) {
      onMyLibraryOpen();
    } else if (selectorIndex == opdsLibraryIdx) {
      onOpdsBrowserOpen();
    } else if (selectorIndex == fileTransferIdx) {
      onFileTransferOpen();
    } else if (selectorIndex == settingsIdx) {
      onSettingsOpen();
    }
  } else if (prevPressed) {
    selectorIndex = (selectorIndex + menuCount - 1) % menuCount;
    updateRequired = true;
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % menuCount;
    updateRequired = true;
  }
}

void HomeActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void HomeActivity::render() {
  // If we have a stored cover buffer, restore it instead of clearing
  const bool bufferRestored = coverBufferStored && restoreCoverBuffer();
  if (!bufferRestored) {
    const bool darkMode = SETTINGS.readerDarkMode;
    renderer.clearScreen(darkMode ? 0x00 : 0xFF);
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const bool darkMode = SETTINGS.readerDarkMode;

  constexpr int margin = 20;
  constexpr int bottomMargin = 60;
  constexpr int menuTileHeight = 45;
  constexpr int menuSpacing = 8;

  // --- Cover image area (no selection highlight, just displays the cover) ---
  const int coverWidth = pageWidth - 2 * margin;
  const int coverHeight = pageHeight * 9 / 10;  // 90% of page height
  const int coverX = (pageWidth - coverWidth) / 2;
  constexpr int coverY = 6;  // Minimal top padding

  // For compatibility with existing cover rendering logic
  const int bookWidth = coverWidth;
  const int bookHeight = coverHeight;
  const int bookX = coverX;
  const int bookY = coverY;
  const bool bookSelected = false;  // Cover itself is never "selected" visually

  // Bookmark dimensions (used in multiple places)
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 10;
  const int bookmarkY = bookY + 5;

  // Draw cover image area (no outline, no selection highlight on the cover itself)
  if (hasContinueReading) {
    if (hasCoverImage && !coverBmpPath.empty() && !coverRendered) {
      // First time: load cover from SD and render
      FsFile file;
      if (SdMan.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          // Calculate position to center image within the cover area
          int imgX, imgY;

          if (bitmap.getWidth() > bookWidth || bitmap.getHeight() > bookHeight) {
            const float imgRatio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
            const float boxRatio = static_cast<float>(bookWidth) / static_cast<float>(bookHeight);

            if (imgRatio > boxRatio) {
              imgX = bookX;
              imgY = bookY + (bookHeight - static_cast<int>(bookWidth / imgRatio)) / 2;
            } else {
              imgX = bookX + (bookWidth - static_cast<int>(bookHeight * imgRatio)) / 2;
              imgY = bookY;
            }
          } else {
            imgX = bookX + (bookWidth - bitmap.getWidth()) / 2;
            imgY = bookY + (bookHeight - bitmap.getHeight()) / 2;
          }

          // Draw the cover image centered (no border)
          renderer.drawBitmap(bitmap, imgX, imgY, bookWidth, bookHeight);

          // Store the buffer with cover image for fast navigation
          coverBufferStored = storeCoverBuffer();
          coverRendered = true;
        }
        file.close();
      }
    }

    if (!coverRendered && !bufferRestored) {
      // No cover image: draw title and author text in plain area (no border)
      std::vector<std::string> words;
      words.reserve(8);
      size_t pos = 0;
      while (pos < lastBookTitle.size()) {
        while (pos < lastBookTitle.size() && lastBookTitle[pos] == ' ') {
          ++pos;
        }
        if (pos >= lastBookTitle.size()) {
          break;
        }
        const size_t start = pos;
        while (pos < lastBookTitle.size() && lastBookTitle[pos] != ' ') {
          ++pos;
        }
        words.emplace_back(lastBookTitle.substr(start, pos - start));
      }

      std::vector<std::string> lines;
      std::string currentLine;
      const int maxLineWidth = bookWidth - 20;
      const int spaceWidth = renderer.getSpaceWidth(UI_12_FONT_ID);

      for (auto& word : words) {
        if (lines.size() >= 3) {
          lines.back().append("...");
          while (!lines.back().empty() && renderer.getTextWidth(UI_12_FONT_ID, lines.back().c_str()) > maxLineWidth) {
            lines.back().resize(lines.back().size() - 3);
            StringUtils::utf8RemoveLastChar(lines.back());
            lines.back().append("...");
          }
          break;
        }

        int wordWidth = renderer.getTextWidth(UI_12_FONT_ID, word.c_str());
        while (wordWidth > maxLineWidth && !word.empty()) {
          StringUtils::utf8RemoveLastChar(word);
          std::string withEllipsis = word + "...";
          wordWidth = renderer.getTextWidth(UI_12_FONT_ID, withEllipsis.c_str());
          if (wordWidth <= maxLineWidth) {
            word = withEllipsis;
            break;
          }
        }

        int newLineWidth = renderer.getTextWidth(UI_12_FONT_ID, currentLine.c_str());
        if (newLineWidth > 0) {
          newLineWidth += spaceWidth;
        }
        newLineWidth += wordWidth;

        if (newLineWidth > maxLineWidth && !currentLine.empty()) {
          lines.push_back(currentLine);
          currentLine = word;
        } else {
          currentLine.append(" ").append(word);
        }
      }

      if (!currentLine.empty() && lines.size() < 3) {
        lines.push_back(currentLine);
      }

      int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * static_cast<int>(lines.size());
      if (!lastBookAuthor.empty()) {
        totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
      }

      int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;

      for (const auto& line : lines) {
        renderer.drawCenteredText(UI_12_FONT_ID, titleYStart, line.c_str(), !darkMode);
        titleYStart += renderer.getLineHeight(UI_12_FONT_ID);
      }

      if (!lastBookAuthor.empty()) {
        titleYStart += renderer.getLineHeight(UI_10_FONT_ID) / 2;
        std::string trimmedAuthor = lastBookAuthor;
        while (renderer.getTextWidth(UI_10_FONT_ID, trimmedAuthor.c_str()) > maxLineWidth && !trimmedAuthor.empty()) {
          StringUtils::utf8RemoveLastChar(trimmedAuthor);
        }
        renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, trimmedAuthor.c_str(), !darkMode);
      }
    }
  } else {
    // No book to continue reading - show placeholder text
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, "No open book", !darkMode);
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), "Start reading below",
                              !darkMode);
  }

  // --- Bottom menu tiles (including Continue Reading as first button if available) ---
  std::vector<std::string> menuItems;
  if (hasContinueReading) {
    menuItems.push_back(continueReadingLabel);
  }
  menuItems.emplace_back("Bookshelf");
  if (hasOpdsUrl) {
    menuItems.emplace_back("OPDS Browser");
  }
  menuItems.emplace_back("File Transfer");
  menuItems.emplace_back("Settings");

  const int menuTileWidth = pageWidth - 2 * margin;
    const int totalMenuHeight =
      static_cast<int>(menuItems.size()) * menuTileHeight + (static_cast<int>(menuItems.size()) - 1) * menuSpacing;

  // Position menu below the cover
  int menuStartY = coverY + coverHeight + 15;
  // Ensure we don't collide with the bottom button legend
  const int maxMenuStartY = pageHeight - bottomMargin - totalMenuHeight - margin;
  if (menuStartY > maxMenuStartY) {
    menuStartY = maxMenuStartY;
  }

  for (size_t i = 0; i < menuItems.size(); ++i) {
    constexpr int tileX = margin;
    const int tileY = menuStartY + static_cast<int>(i) * (menuTileHeight + menuSpacing);
    const bool selected = selectorIndex == static_cast<int>(i);

    if (selected) {
      renderer.fillRect(tileX, tileY, menuTileWidth, menuTileHeight, !darkMode);
    } else {
      renderer.drawRect(tileX, tileY, menuTileWidth, menuTileHeight, !darkMode);
    }

    const auto label = renderer.truncatedText(UI_10_FONT_ID, menuItems[i].c_str(), menuTileWidth - 20);
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label.c_str());
    const int textX = tileX + (menuTileWidth - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY = tileY + (menuTileHeight - lineHeight) / 2;

    const bool textColor = selected ? darkMode : !darkMode;
    renderer.drawText(UI_10_FONT_ID, textX, textY, label.c_str(), textColor);
  }

    const auto labels = mappedInput.mapLabels("", "Select", "Up", "Down");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4, !darkMode);

    // Battery indicator - positioned same as reader status bar (above button hints)
    const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
    // Match reader status bar: orientedMarginBottom is ~19 for status bar, 4px from bottom margin
    // Button hints area is about 40px, so position at 35px from bottom
    constexpr int batteryMarginBottom = 30;
    ScreenComponents::drawBattery(renderer, margin + 1, pageHeight - batteryMarginBottom, showBatteryPercentage, !darkMode);

  renderer.displayBuffer();
}
