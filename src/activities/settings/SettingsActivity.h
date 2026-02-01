#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"

class CrossPointSettings;
struct SettingInfo;

class SettingsActivity final : public ActivityWithSubactivity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  int selectedItemIndex = 0;
  const std::function<void()> onGoHome;

  enum class SettingsItemType { Header, Setting, Separator };
  struct SettingsItem {
    SettingsItemType type;
    const char* header;
    const SettingInfo* setting;
  };
  std::vector<SettingsItem> settingsItems;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void toggleCurrentSetting();
  void buildSettingsItems();
  int getPageItems() const;
  int getTotalPages() const;
  int getCurrentPage() const;
  int findNextSelectableIndex(int startIndex, int direction) const;
  bool isSelectableIndex(int index) const;
  int getFirstSelectableIndex() const;

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("Settings", renderer, mappedInput), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void requestScreenRefresh() override { updateRequired = true; }
};
