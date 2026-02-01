#include "MyLibraryActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "ScreenComponents.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
// Layout constants
constexpr int CONTENT_START_Y = 60;
constexpr int LINE_HEIGHT = 30;
constexpr int LEFT_MARGIN = 20;
constexpr int RIGHT_MARGIN = 40;  // Extra space for scroll indicator

// Timing thresholds
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    if (str1.back() == '/' && str2.back() != '/') return true;
    if (str1.back() != '/' && str2.back() == '/') return false;
    return lexicographical_compare(
        begin(str1), end(str1), begin(str2), end(str2),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}
}  // namespace

int MyLibraryActivity::getPageItems() const {
  const int screenHeight = renderer.getScreenHeight();
  const int bottomBarHeight = 60;  // Space for button hints
  const int availableHeight = screenHeight - CONTENT_START_Y - bottomBarHeight;
  int items = availableHeight / LINE_HEIGHT;
  if (items < 1) {
    items = 1;
  }
  return items;
}

int MyLibraryActivity::getCurrentItemCount() const {
  return static_cast<int>(items.size());
}

int MyLibraryActivity::getTotalPages() const {
  const int itemCount = getCurrentItemCount();
  const int pageItems = getPageItems();
  if (itemCount == 0) return 1;
  return (itemCount + pageItems - 1) / pageItems;
}

int MyLibraryActivity::getCurrentPage() const {
  const int pageItems = getPageItems();
  return selectorIndex / pageItems + 1;
}

int MyLibraryActivity::getFirstSelectableIndex() const {
  for (size_t i = 0; i < items.size(); i++) {
    if (items[i].type == LibraryItemType::Recent || items[i].type == LibraryItemType::File) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

bool MyLibraryActivity::isSelectableIndex(const int index) const {
  if (index < 0 || index >= static_cast<int>(items.size())) {
    return false;
  }
  return items[index].type == LibraryItemType::Recent || items[index].type == LibraryItemType::File;
}

int MyLibraryActivity::findNextSelectableIndex(const int startIndex, const int direction) const {
  const int itemCount = static_cast<int>(items.size());
  if (itemCount == 0) {
    return startIndex;
  }
  int index = startIndex;
  for (int i = 0; i < itemCount; i++) {
    index = (index + direction + itemCount) % itemCount;
    if (isSelectableIndex(index)) {
      return index;
    }
  }
  return startIndex;
}

int MyLibraryActivity::findItemIndexForFileIndex(const size_t fileIndex) const {
  for (size_t i = 0; i < items.size(); i++) {
    if (items[i].type == LibraryItemType::File && static_cast<size_t>(items[i].index) == fileIndex) {
      return static_cast<int>(i);
    }
  }
  return getFirstSelectableIndex();
}

void MyLibraryActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(books.size());

  for (const auto& book : books) {
    // Skip if file no longer exists
    if (!SdMan.exists(book.path.c_str())) {
      continue;
    }
    recentBooks.push_back(book);
  }
}

void MyLibraryActivity::loadFiles() {
  files.clear();

  auto root = SdMan.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    // Ignore folders and non-book files
    if (!file.isDirectory()) {
      auto filename = std::string(name);
      if (StringUtils::checkFileExtension(filename, ".epub") || StringUtils::checkFileExtension(filename, ".xtch") ||
          StringUtils::checkFileExtension(filename, ".xtc") || StringUtils::checkFileExtension(filename, ".txt") ||
          StringUtils::checkFileExtension(filename, ".md")) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

void MyLibraryActivity::rebuildItemList() {
  items.clear();
  items.reserve(recentBooks.size() + files.size() + 4);

  items.push_back({LibraryItemType::Header, -1, "Recent"});
  if (recentBooks.empty()) {
    items.push_back({LibraryItemType::Placeholder, -1, "No recent books"});
  } else {
    for (size_t i = 0; i < recentBooks.size(); i++) {
      items.push_back({LibraryItemType::Recent, static_cast<int>(i), nullptr});
    }
  }

  items.push_back({LibraryItemType::Header, -1, "All Books"});
  if (files.empty()) {
    items.push_back({LibraryItemType::Placeholder, -1, "No books found"});
  } else {
    for (size_t i = 0; i < files.size(); i++) {
      items.push_back({LibraryItemType::File, static_cast<int>(i), nullptr});
    }
  }
}

size_t MyLibraryActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++) {
    if (files[i] == name) return i;
  }
  return 0;
}

void MyLibraryActivity::taskTrampoline(void* param) {
  auto* self = static_cast<MyLibraryActivity*>(param);
  self->displayTaskLoop();
}

void MyLibraryActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Load data for both sections
  loadRecentBooks();
  loadFiles();

  rebuildItemList();

  if (currentTab == Tab::Files) {
    selectorIndex = findItemIndexForFileIndex(0);
  } else {
    selectorIndex = getFirstSelectableIndex();
  }
  updateRequired = true;

  xTaskCreate(&MyLibraryActivity::taskTrampoline, "MyLibraryActivityTask",
              4096,               // Stack size (increased for epub metadata loading)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void MyLibraryActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to
  // EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  files.clear();
}

void MyLibraryActivity::loop() {
  const int itemCount = getCurrentItemCount();
  const int pageItems = getPageItems();

  // Long press BACK (1s+) goes to root folder
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS) {
    if (basepath != "/") {
      basepath = "/";
      loadFiles();
      rebuildItemList();
      selectorIndex = getFirstSelectableIndex();
      updateRequired = true;
    }
    return;
  }

  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                          mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

  // Confirm button - open selected item
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (isSelectableIndex(selectorIndex)) {
      const auto& item = items[selectorIndex];
      if (item.type == LibraryItemType::Recent) {
        const auto& book = recentBooks[static_cast<size_t>(item.index)];
        onSelectBook(book.path, Tab::Recent);
      } else if (item.type == LibraryItemType::File) {
        const auto& fileName = files[static_cast<size_t>(item.index)];
        if (basepath.back() != '/') basepath += "/";
        if (!fileName.empty() && fileName.back() == '/') {
          basepath += fileName.substr(0, fileName.length() - 1);
          loadFiles();
          rebuildItemList();
          selectorIndex = getFirstSelectableIndex();
          updateRequired = true;
        } else {
          onSelectBook(basepath + fileName, Tab::Files);
        }
      }
    }
    return;
  }

  // Back button
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        // Go up one directory, remembering the directory we came from
        const std::string oldPath = basepath;
        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();
        rebuildItemList();

        // Select the directory we just came from
        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findItemIndexForFileIndex(findEntry(dirName));

        updateRequired = true;
      } else {
        // Go home
        onGoHome();
      }
    }
    return;
  }

  // Navigation: Up/Down moves through items only
  const bool prevReleased = upReleased;
  const bool nextReleased = downReleased;

  if ((prevReleased || nextReleased) && itemCount > 0) {
    int candidateIndex = selectorIndex;
    if (skipPage) {
      const int pageDelta = prevReleased ? -1 : 1;
      candidateIndex = ((selectorIndex / pageItems + pageDelta) * pageItems + itemCount) % itemCount;
      candidateIndex = findNextSelectableIndex(candidateIndex, prevReleased ? -1 : 1);
    } else {
      candidateIndex = findNextSelectableIndex(selectorIndex, prevReleased ? -1 : 1);
    }

    if (candidateIndex != selectorIndex) {
      selectorIndex = candidateIndex;
      updateRequired = true;
    }
  }
}

void MyLibraryActivity::displayTaskLoop() {
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

void MyLibraryActivity::render() const {
  const bool darkMode = SETTINGS.readerDarkMode;
  renderer.clearScreen(darkMode ? 0x00 : 0xFF);

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Bookshelf", !darkMode, EpdFontFamily::BOLD);
  renderCombinedList();

  // Draw scroll indicator
  const int screenHeight = renderer.getScreenHeight();
  const int contentHeight = screenHeight - CONTENT_START_Y - 60;  // 60 for bottom bar
  ScreenComponents::drawScrollIndicator(renderer, getCurrentPage(), getTotalPages(), CONTENT_START_Y, contentHeight,
                                        !darkMode);

  // Draw side button hints (up/down navigation on right side)
  renderer.drawSideButtonHints(UI_10_FONT_ID, ">", "<", !darkMode);

  // Draw bottom button hints
  const auto labels = mappedInput.mapLabels("« Back", "Open", "Up", "Down");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4, !darkMode);

  renderer.displayBuffer();
}

void MyLibraryActivity::renderCombinedList() const {
  const auto pageWidth = renderer.getScreenWidth();
  const int pageItems = getPageItems();
  const int itemCount = static_cast<int>(items.size());
  const bool darkMode = SETTINGS.readerDarkMode;

  if (itemCount == 0) {
    renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, CONTENT_START_Y, "No books found", !darkMode);
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;

  if (isSelectableIndex(selectorIndex)) {
    renderer.fillRect(0, CONTENT_START_Y + (selectorIndex - pageStartIndex) * LINE_HEIGHT - 2,
                      pageWidth - RIGHT_MARGIN, LINE_HEIGHT, darkMode);
  }

  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int y = CONTENT_START_Y + (i - pageStartIndex) * LINE_HEIGHT;
    const auto& item = items[i];

    if (item.type == LibraryItemType::Header) {
      renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, y, item.label, !darkMode, EpdFontFamily::BOLD);
      // Draw separator line next to header text
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, item.label, EpdFontFamily::BOLD);
      const int lineStart = LEFT_MARGIN + textWidth + 10;
      if (lineStart < pageWidth - RIGHT_MARGIN) {
        renderer.drawLine(lineStart, y + 8, pageWidth - RIGHT_MARGIN, y + 8, !darkMode);
      }
      continue;
    }

    if (item.type == LibraryItemType::Placeholder) {
      renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, y, item.label, !darkMode);
      continue;
    }

    const bool isSelected = (i == selectorIndex);
    const bool textColor = !darkMode;
    if (item.type == LibraryItemType::Recent) {
      const auto& book = recentBooks[static_cast<size_t>(item.index)];
      std::string title = book.title;
      if (title.empty()) {
        title = book.path;
        const size_t lastSlash = title.find_last_of('/');
        if (lastSlash != std::string::npos) {
          title = title.substr(lastSlash + 1);
        }
        const size_t dot = title.find_last_of('.');
        if (dot != std::string::npos) {
          title.resize(dot);
        }
      }
      auto truncatedTitle =
          renderer.truncatedText(UI_10_FONT_ID, title.c_str(), pageWidth - LEFT_MARGIN - RIGHT_MARGIN);
      renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, y, truncatedTitle.c_str(), textColor);
    } else if (item.type == LibraryItemType::File) {
      const auto& fileName = files[static_cast<size_t>(item.index)];
      auto truncatedName =
          renderer.truncatedText(UI_10_FONT_ID, fileName.c_str(), pageWidth - LEFT_MARGIN - RIGHT_MARGIN);
      renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, y, truncatedName.c_str(), textColor);
    }
  }
}
