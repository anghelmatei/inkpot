#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include <cstring>
#include <string>
#include <vector>

#include "CategorySettingsActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "CalibreSettingsActivity.h"
#include "ClearCacheActivity.h"
#include "fontIds.h"
#include "util/WallpaperUtils.h"

namespace {
constexpr int SETTINGS_TOP_Y = 60;
constexpr int LINE_HEIGHT = 30;

constexpr int displaySettingsCount = 7;
const SettingInfo displaySettings[displaySettingsCount] = {
  SettingInfo::Toggle("Dark Mode", &CrossPointSettings::readerDarkMode),
    // Should match with SLEEP_SCREEN_MODE
    SettingInfo::Enum("Sleep Screen", &CrossPointSettings::sleepScreen, {"Dark", "Light", "Custom", "Cover", "None"}),
    SettingInfo::Enum("Sleep Screen Cover Mode", &CrossPointSettings::sleepScreenCoverMode, {"Fit", "Crop"}),
    SettingInfo::Enum("Sleep Screen Cover Filter", &CrossPointSettings::sleepScreenCoverFilter,
                      {"None", "Contrast", "Inverted"}),
    SettingInfo::Action("Next Wallpaper"),
    SettingInfo::Enum("Status Bar", &CrossPointSettings::statusBar,
                      {"None", "No Progress", "Full w/ Percentage", "Full w/ Progress Bar", "Progress Bar"}),
    SettingInfo::Enum("Hide Battery %", &CrossPointSettings::hideBatteryPercentage, {"Never", "In Reader", "Always"})};

constexpr int readerSettingsCount = 10;
const SettingInfo readerSettings[readerSettingsCount] = {
    SettingInfo::Enum("Refresh Frequency", &CrossPointSettings::refreshFrequency,
                      {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"}),
    SettingInfo::Enum("Font Family", &CrossPointSettings::fontFamily, {"Bookerly", "Noto Sans", "Open Dyslexic"}),
    SettingInfo::Enum("Font Size", &CrossPointSettings::fontSize, {"Small", "Medium", "Large", "X Large"}),
    SettingInfo::Enum("Line Spacing", &CrossPointSettings::lineSpacing, {"Tight", "Normal", "Wide"}),
    SettingInfo::Value("Screen Margin", &CrossPointSettings::screenMargin, {5, 40, 5}),
    SettingInfo::Enum("Paragraph Alignment", &CrossPointSettings::paragraphAlignment,
                      {"Justify", "Left", "Center", "Right"}),
    SettingInfo::Toggle("Hyphenation", &CrossPointSettings::hyphenationEnabled),
    SettingInfo::Enum("Reading Orientation", &CrossPointSettings::orientation,
                      {"Portrait", "Landscape CW", "Inverted", "Landscape CCW"}),
    SettingInfo::Toggle("Extra Paragraph Spacing", &CrossPointSettings::extraParagraphSpacing),
    SettingInfo::Toggle("Text Anti-Aliasing", &CrossPointSettings::textAntiAliasing)};

constexpr int controlsSettingsCount = 5;
const SettingInfo controlsSettings[controlsSettingsCount] = {
    SettingInfo::Enum(
        "Front Button Layout", &CrossPointSettings::frontButtonLayout,
        {"Bck, Cnfrm, Lft, Rght", "Lft, Rght, Bck, Cnfrm", "Lft, Bck, Cnfrm, Rght", "Bck, Cnfrm, Rght, Lft"}),
    SettingInfo::Enum("Side Button Layout (reader)", &CrossPointSettings::sideButtonLayout,
                      {"Prev, Next", "Next, Prev"}),
    SettingInfo::Toggle("Long-press Chapter Skip", &CrossPointSettings::longPressChapterSkip),
  SettingInfo::Enum("Short Power Button Click", &CrossPointSettings::shortPwrBtn,
            {"Ignore", "Sleep", "Page Turn", "Orientation Cycle"}),
  SettingInfo::Enum("Double-Tap Power Button", &CrossPointSettings::doubleTapPwrBtn,
            {"Ignore", "Toggle Light/Dark"})};

constexpr int systemSettingsCount = 5;
const SettingInfo systemSettings[systemSettingsCount] = {
    SettingInfo::Enum("Time to Sleep", &CrossPointSettings::sleepTimeout,
                      {"1 min", "5 min", "10 min", "15 min", "30 min"}),
    SettingInfo::Action("KOReader Sync"), SettingInfo::Action("OPDS Browser"), SettingInfo::Action("Clear Cache"),
    SettingInfo::Action("Check for updates")};
}  // namespace

void SettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<SettingsActivity*>(param);
  self->displayTaskLoop();
}

void SettingsActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();

  buildSettingsItems();
  selectedItemIndex = getFirstSelectableIndex();

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&SettingsActivity::taskTrampoline, "SettingsActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void SettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void SettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Handle setting selection
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    updateRequired = true;
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    onGoHome();
    return;
  }

  // Handle navigation
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    const int nextIndex = findNextSelectableIndex(selectedItemIndex, -1);
    if (nextIndex != selectedItemIndex) {
      selectedItemIndex = nextIndex;
      updateRequired = true;
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    const int nextIndex = findNextSelectableIndex(selectedItemIndex, 1);
    if (nextIndex != selectedItemIndex) {
      selectedItemIndex = nextIndex;
      updateRequired = true;
    }
  }
}

void SettingsActivity::toggleCurrentSetting() {
  if (!isSelectableIndex(selectedItemIndex)) {
    return;
  }

  const auto& item = settingsItems[selectedItemIndex];
  const auto& setting = *(item.setting);

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    if (strcmp(setting.name, "KOReader Sync") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new KOReaderSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
      return;
    }
    if (strcmp(setting.name, "OPDS Browser") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new CalibreSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
      return;
    }
    if (strcmp(setting.name, "Clear Cache") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ClearCacheActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
      return;
    }
    if (strcmp(setting.name, "Check for updates") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new OtaUpdateActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
      return;
    }
    if (strcmp(setting.name, "Next Wallpaper") == 0) {
      std::vector<std::string> files;
      if (listCustomWallpapers(files)) {
        size_t nextIndex = APP_STATE.lastSleepImage + 1;
        if (nextIndex >= files.size()) {
          nextIndex = 0;
        }
        APP_STATE.lastSleepImage = nextIndex;
        APP_STATE.saveToFile();
      }
    }
  } else {
    return;
  }

  SETTINGS.saveToFile();
}

void SettingsActivity::buildSettingsItems() {
  settingsItems.clear();
  settingsItems.reserve(displaySettingsCount + readerSettingsCount + controlsSettingsCount + systemSettingsCount + 4);

  settingsItems.push_back({SettingsItemType::Header, "Display", nullptr});
  for (int i = 0; i < displaySettingsCount; i++) {
    settingsItems.push_back({SettingsItemType::Setting, nullptr, &displaySettings[i]});
  }
  settingsItems.push_back({SettingsItemType::Header, "Reader", nullptr});
  for (int i = 0; i < readerSettingsCount; i++) {
    settingsItems.push_back({SettingsItemType::Setting, nullptr, &readerSettings[i]});
  }
  settingsItems.push_back({SettingsItemType::Header, "Controls", nullptr});
  for (int i = 0; i < controlsSettingsCount; i++) {
    settingsItems.push_back({SettingsItemType::Setting, nullptr, &controlsSettings[i]});
  }
  settingsItems.push_back({SettingsItemType::Header, "System", nullptr});
  for (int i = 0; i < systemSettingsCount; i++) {
    settingsItems.push_back({SettingsItemType::Setting, nullptr, &systemSettings[i]});
  }
}

int SettingsActivity::getPageItems() const {
  const int screenHeight = renderer.getScreenHeight();
  const int bottomBarHeight = 60;
  const int availableHeight = screenHeight - SETTINGS_TOP_Y - bottomBarHeight;
  // Subtract 1 to account for header separator lines taking extra space
  int items = (availableHeight / LINE_HEIGHT) - 1;
  if (items < 1) {
    items = 1;
  }
  return items;
}

int SettingsActivity::getTotalPages() const {
  const int itemCount = static_cast<int>(settingsItems.size());
  const int pageItems = getPageItems();
  if (itemCount == 0) return 1;
  return (itemCount + pageItems - 1) / pageItems;
}

int SettingsActivity::getCurrentPage() const {
  const int pageItems = getPageItems();
  return selectedItemIndex / pageItems + 1;
}

bool SettingsActivity::isSelectableIndex(const int index) const {
  if (index < 0 || index >= static_cast<int>(settingsItems.size())) {
    return false;
  }
  return settingsItems[index].type == SettingsItemType::Setting;
}

int SettingsActivity::getFirstSelectableIndex() const {
  for (size_t i = 0; i < settingsItems.size(); i++) {
    if (settingsItems[i].type == SettingsItemType::Setting) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

int SettingsActivity::findNextSelectableIndex(const int startIndex, const int direction) const {
  const int itemCount = static_cast<int>(settingsItems.size());
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

void SettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void SettingsActivity::render() const {
  const bool darkMode = SETTINGS.readerDarkMode;
  renderer.clearScreen(darkMode ? 0x00 : 0xFF);

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Settings", !darkMode, EpdFontFamily::BOLD);

  const int pageItems = getPageItems();
  const int totalItems = static_cast<int>(settingsItems.size());
  int pageStartIndex = (pageItems > 0) ? (selectedItemIndex / pageItems * pageItems) : 0;

  auto computeReservedRows = [&](int startIndex) {
    const bool moreAbove = startIndex > 0;
    const bool moreBelow = (startIndex + pageItems) < totalItems;
    const int reservedTop = moreAbove ? 1 : 0;
    const int reservedBottom = moreBelow ? 1 : 0;
    int visibleRows = pageItems - reservedTop - reservedBottom;
    if (visibleRows < 1) {
      visibleRows = 1;
    }
    return std::make_tuple(moreAbove, moreBelow, reservedTop, reservedBottom, visibleRows);
  };

  bool hasMoreAbove = false;
  bool hasMoreBelow = false;
  int reservedTop = 0;
  int reservedBottom = 0;
  int visibleRows = 1;
  
  // Iterate to find stable pageStartIndex that keeps selectedItemIndex visible
  // This handles the case where adjusting pageStartIndex changes visibleRows
  for (int iteration = 0; iteration < 3; iteration++) {
    std::tie(hasMoreAbove, hasMoreBelow, reservedTop, reservedBottom, visibleRows) = computeReservedRows(pageStartIndex);
    
    bool needsAdjustment = false;
    if (selectedItemIndex < pageStartIndex) {
      pageStartIndex = selectedItemIndex;
      needsAdjustment = true;
    } else if (selectedItemIndex >= pageStartIndex + visibleRows) {
      pageStartIndex = selectedItemIndex - visibleRows + 1;
      needsAdjustment = true;
    }
    
    if (pageStartIndex < 0) {
      pageStartIndex = 0;
      needsAdjustment = true;
    } else if (pageStartIndex > totalItems - 1) {
      pageStartIndex = std::max(0, totalItems - 1);
      needsAdjustment = true;
    }
    
    if (!needsAdjustment) {
      break;
    }
  }
  
  // Final computation after stable pageStartIndex
  std::tie(hasMoreAbove, hasMoreBelow, reservedTop, reservedBottom, visibleRows) = computeReservedRows(pageStartIndex);

  // Calculate the row position for the selected item
  // Items are rendered starting at row reservedTop, ending before row (pageItems - reservedBottom)
  const int selectedRowInView = selectedItemIndex - pageStartIndex;
  const bool selectedItemVisible = (selectedRowInView >= 0 && selectedRowInView < visibleRows);
  
  if (selectedItemVisible && isSelectableIndex(selectedItemIndex)) {
    renderer.fillRect(0,
                      SETTINGS_TOP_Y + (selectedRowInView + reservedTop) * LINE_HEIGHT - 2,
                      pageWidth - 1, LINE_HEIGHT, !darkMode);
  }

  const int renderEndIndex = std::min(totalItems, pageStartIndex + visibleRows);
  for (int i = pageStartIndex; i < renderEndIndex; i++) {
    const int itemY = SETTINGS_TOP_Y + (i - pageStartIndex + reservedTop) * LINE_HEIGHT;
    const auto& item = settingsItems[i];

    if (item.type == SettingsItemType::Header) {
      renderer.drawText(UI_10_FONT_ID, 20, itemY, item.header, !darkMode, EpdFontFamily::BOLD);
      // Draw separator line below the header text
      const int lineY = itemY + LINE_HEIGHT - 2;
      renderer.drawLine(20, lineY, pageWidth - 20, lineY, !darkMode);
      continue;
    }

    if (item.type == SettingsItemType::Separator) {
      // Draw separator text (like "(more)") without selection highlight
      renderer.drawText(UI_10_FONT_ID, 20, itemY, item.header, !darkMode);
      continue;
    }

    const bool isSelected = (i == selectedItemIndex);
    const bool textColor = darkMode ? isSelected : !isSelected;
    renderer.drawText(UI_10_FONT_ID, 20, itemY, item.setting->name, textColor);

    std::string valueText;
    if (item.setting->type == SettingType::TOGGLE && item.setting->valuePtr != nullptr) {
      const bool value = SETTINGS.*(item.setting->valuePtr);
      valueText = value ? "ON" : "OFF";
    } else if (item.setting->type == SettingType::ENUM && item.setting->valuePtr != nullptr) {
      const uint8_t value = SETTINGS.*(item.setting->valuePtr);
      valueText = item.setting->enumValues[value];
    } else if (item.setting->type == SettingType::VALUE && item.setting->valuePtr != nullptr) {
      valueText = std::to_string(SETTINGS.*(item.setting->valuePtr));
    }
    if (!valueText.empty()) {
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, itemY, valueText.c_str(), textColor);
    }
  }

  // Draw "(more)" indicators with visual separators to make them clearly non-selectable
  if (hasMoreAbove) {
    const int moreY = SETTINGS_TOP_Y;
    // Draw "(more)" text
    renderer.drawText(UI_10_FONT_ID, 20, moreY, "(more)", !darkMode);
    // Draw separator line below the "(more)" row - centered in the gap
    const int lineY = moreY + LINE_HEIGHT - 2;
    renderer.drawLine(20, lineY, pageWidth - 20, lineY, !darkMode);
  }
  if (hasMoreBelow) {
    const int lastRowY = SETTINGS_TOP_Y + (pageItems - 1) * LINE_HEIGHT;
    // Draw separator line above the "(more)" row - centered in the gap
    renderer.drawLine(20, lastRowY - 2, pageWidth - 20, lastRowY - 2, !darkMode);
    // Draw "(more)" text
    renderer.drawText(UI_10_FONT_ID, 20, lastRowY, "(more)", !darkMode);
  }

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Back", "Toggle", "Up", "Down");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4, !darkMode);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
