#include "WallpaperUtils.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "fontIds.h"
#include "images/CrossLarge.h"

bool listCustomWallpapers(std::vector<std::string>& files) {
  files.clear();

  auto dir = SdMan.open("/sleep");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }
    file.getName(name, sizeof(name));
    auto filename = std::string(name);
    if (filename.empty() || filename[0] == '.') {
      file.close();
      continue;
    }
    if (filename.length() < 4 || filename.substr(filename.length() - 4) != ".bmp") {
      file.close();
      continue;
    }
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) {
      file.close();
      continue;
    }
    files.emplace_back(filename);
    file.close();
  }

  dir.close();
  return !files.empty();
}

bool openCustomWallpaperFile(const size_t index, FsFile& file, std::string& filename) {
  std::vector<std::string> files;
  if (!listCustomWallpapers(files)) {
    return false;
  }

  size_t selectedIndex = index;
  if (selectedIndex >= files.size()) {
    selectedIndex = 0;
  }

  filename = "/sleep/" + files[selectedIndex];
  return SdMan.openFileForRead("WAL", filename, file);
}

void renderWallpaperBitmap(GfxRenderer& renderer, const Bitmap& bitmap, const bool crop, const uint8_t filter, const bool displayAfterRender) {
  int x = 0;
  int y = 0;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0.0f;
  float cropY = 0.0f;

  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      if (crop) {
        cropX = 1.0f - (screenRatio / ratio);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      if (crop) {
        cropY = 1.0f - (ratio / screenRatio);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
    }
  } else {
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  // Check if we should invert for dark mode compatibility
  // Invert dark images when in dark mode so they're visible
  const bool shouldInvertForDarkMode = SETTINGS.readerDarkMode && !hasGreyscale;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  // Apply filter-based inversion
  if (filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }
  // Apply dark mode inversion for predominantly dark bitmaps
  else if (shouldInvertForDarkMode) {
    // Only invert if the image appears to be predominantly dark (would be invisible on black background)
    // For sleep screen covers, we assume book covers with dark artwork need inversion
    renderer.invertScreen();
  }

  if (displayAfterRender) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  if (hasGreyscale && displayAfterRender) {
    bitmap.rewindToData();
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void renderPopup(GfxRenderer& renderer, const char* message) {
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::BOLD);
  constexpr int margin = 20;
  const int x = (renderer.getScreenWidth() - textWidth - margin * 2) / 2;
  constexpr int y = 117;
  const int w = textWidth + margin * 2;
  const int h = renderer.getLineHeight(UI_12_FONT_ID) + margin * 2;

  // Draw popup box with border
  renderer.fillRect(x - 5, y - 5, w + 10, h + 10, true);
  renderer.fillRect(x + 5, y + 5, w - 10, h - 10, false);
  renderer.drawText(UI_12_FONT_ID, x + margin, y + margin, message, true, EpdFontFamily::BOLD);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void renderBootWallpaper(GfxRenderer& renderer) {
  // Try to load user's custom wallpaper (same one used for sleep)
  FsFile file;
  std::string filename;
  bool wallpaperLoaded = false;

  if (openCustomWallpaperFile(APP_STATE.lastSleepImage, file, filename)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      // Render wallpaper WITHOUT displaying - we'll batch it with the popup
      renderWallpaperBitmap(renderer, bitmap, false, CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER, false);
      wallpaperLoaded = true;
      file.close();
    } else {
      file.close();
    }
  }

  // Fallback to sleep.bmp in root if custom wallpaper not loaded
  if (!wallpaperLoaded && SdMan.openFileForRead("BOOT", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      renderWallpaperBitmap(renderer, bitmap, false, CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER, false);
      wallpaperLoaded = true;
      file.close();
    } else {
      file.close();
    }
  }

  // If no wallpaper loaded, screen still has whatever was there (sleep image or cleared)
  // Now draw popup on top and do SINGLE display refresh
  const char* message = "Opening...";
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::BOLD);
  constexpr int margin = 20;
  const int x = (renderer.getScreenWidth() - textWidth - margin * 2) / 2;
  constexpr int y = 117;
  const int w = textWidth + margin * 2;
  const int h = renderer.getLineHeight(UI_12_FONT_ID) + margin * 2;

  // Draw popup box with border directly (don't call renderPopup which does its own display)
  renderer.fillRect(x - 5, y - 5, w + 10, h + 10, true);
  renderer.fillRect(x + 5, y + 5, w - 10, h - 10, false);
  renderer.drawText(UI_12_FONT_ID, x + margin, y + margin, message, true, EpdFontFamily::BOLD);

  // Single display refresh for wallpaper + popup together
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void renderBootPopupOnly(GfxRenderer& renderer) {
  // Fast popup render for boot - uses FAST_REFRESH for minimal latency
  const char* message = "Opening...";
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::BOLD);
  constexpr int margin = 20;

  // Calculate centered position
  const int x = (renderer.getScreenWidth() - textWidth - margin * 2) / 2;
  constexpr int y = 117;
  const int w = textWidth + margin * 2;
  const int h = renderer.getLineHeight(UI_12_FONT_ID) + margin * 2;

  // Clear only the popup area in shadow buffer (preserve rest of screen)
  // This assumes the sleep wallpaper is still in the hardware display buffer
  renderer.fillRect(x - 5, y - 5, w + 10, h + 10, false);  // Clear to white
  
  // Draw popup box with border
  renderer.fillRect(x - 5, y - 5, w + 10, h + 10, true);   // Outer border (black)
  renderer.fillRect(x + 5, y + 5, w - 10, h - 10, false);  // Inner area (white)
  renderer.drawText(UI_12_FONT_ID, x + margin, y + margin, message, true, EpdFontFamily::BOLD);
  
  // Use FAST_REFRESH for quick display - we want the popup to appear ASAP
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
