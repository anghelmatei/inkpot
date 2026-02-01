#include "BootActivity.h"

#include <GfxRenderer.h>

#include "util/WallpaperUtils.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  // Try to show user's wallpaper with "Opening..." popup overlay
  // This mirrors SleepActivity's "Entering Sleep..." behavior
  // renderBootWallpaper(renderer); 
  // OPTIMIZATION: Main loop now handles drawing (either popup-only or full wallpaper)
  // We do nothing here to avoid double-draw.
}

// renderPopup and renderCustomWallpaper and renderDefaultBootScreen 
// have been moved to WallpaperUtils to share logic with main.cpp
