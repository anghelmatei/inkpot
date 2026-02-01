#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"

class MyLibraryActivity final : public Activity {
 public:
  enum class Tab { Recent, Files };

 private:
  enum class LibraryItemType { Header, Recent, File, Placeholder };
  struct LibraryItem {
    LibraryItemType type;
    int index;
    const char* label;
  };

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  Tab currentTab = Tab::Recent;
  int selectorIndex = 0;
  bool updateRequired = false;

  // Recent tab state
  std::vector<RecentBook> recentBooks;

  // Files tab state (from FileSelectionActivity)
  std::string basepath = "/";
  std::vector<std::string> files;

  std::vector<LibraryItem> items;

  // Callbacks
  const std::function<void()> onGoHome;
  const std::function<void(const std::string& path, Tab fromTab)> onSelectBook;

  // Number of items that fit on a page
  int getPageItems() const;
  int getCurrentItemCount() const;
  int getTotalPages() const;
  int getCurrentPage() const;
  int getFirstSelectableIndex() const;
  int findNextSelectableIndex(int startIndex, int direction) const;
  bool isSelectableIndex(int index) const;
  int findItemIndexForFileIndex(size_t fileIndex) const;

  // Data loading
  void loadRecentBooks();
  void loadFiles();
  void rebuildItemList();
  size_t findEntry(const std::string& name) const;

  // Rendering
  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void renderCombinedList() const;

 public:
  explicit MyLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             const std::function<void()>& onGoHome,
                             const std::function<void(const std::string& path, Tab fromTab)>& onSelectBook,
                             Tab initialTab = Tab::Recent, std::string initialPath = "/")
      : Activity("MyLibrary", renderer, mappedInput),
        currentTab(initialTab),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)),
        onGoHome(onGoHome),
        onSelectBook(onSelectBook) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void requestScreenRefresh() override { updateRequired = true; }
};
