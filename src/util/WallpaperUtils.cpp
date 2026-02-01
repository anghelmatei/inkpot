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

void renderWallpaperBitmap(GfxRenderer& renderer, const Bitmap& bitmap, const bool crop, const uint8_t filter) {
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

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
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

  if (openCustomWallpaperFile(APP_STATE.lastSleepImage, file, filename)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      // Render using the shared bitmap renderer, but force crop=false and no filter for boot speed/simplicity
      // Effectively doing what BootActivity did manually but reusing the core bitmap logic
      // Note: BootActivity had specific simplified scaling logic which renderWallpaperBitmap covers largely
      
      // Using generic renderWallpaperBitmap to ensure consistent "fill" logic
      // BootActivity's custom logic was:
      // if (ratio > screenRatio) y = ...; else x = ...;
      // renderWallpaperBitmap logic is almost identical for crop=false
      
      renderWallpaperBitmap(renderer, bitmap, false, CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);
      
      file.close();
      renderPopup(renderer, "Opening...");
      return;
    }
    file.close();
  }

  // Fallback to sleep.bmp in root
  if (SdMan.openFileForRead("BOOT", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      renderWallpaperBitmap(renderer, bitmap, false, CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);
      file.close();
      renderPopup(renderer, "Opening...");
      return;
    }
    file.close();
  }

  // No custom wallpaper found - just show popup over whatever is on screen
  // (likely the sleep image that was rendered before sleep)
  renderPopup(renderer, "Opening...");
}

void renderBootPopupOnly(GfxRenderer& renderer) {
  // Logic derived from renderPopup but using partial window update
  const char* message = "Opening...";
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::BOLD);
  constexpr int margin = 20;

  // Calculate centered position
  const int x = (renderer.getScreenWidth() - textWidth - margin * 2) / 2;
  constexpr int y = 117;
  const int w = textWidth + margin * 2;
  const int h = renderer.getLineHeight(UI_12_FONT_ID) + margin * 2;

  // Visual bounds for the popup
  int rx = x - 5;
  int ry = y - 5;
  int rw = w + 10;
  int rh = h + 10;

  // Alignment for partial update (EInkDisplay requires byte alignment)
  // Portrait: y and h must be multiples of 8 (mapped to physical X)
  // Landscape: x and w must be multiples of 8 (mapped to physical X)
  const auto orientation = renderer.getOrientation();
  
  // Clear the aligned area in the shadow buffer to white (0xFF)
  // This ensures the background behind the popup is white for the partial update
  renderer.clearScreen(0xFF); 

  if (orientation == GfxRenderer::Portrait || orientation == GfxRenderer::PortraitInverted) {
    // vertical logical orientation -> Logical Y maps to Physical X
    // Align Logical Y and Height
    const int rYAligned = ry & ~7;
    const int rHAligned = (ry + rh + 7 & ~7) - rYAligned;
    
    // Re-draw the popup in the shadow buffer
    renderer.fillRect(rx, ry, rw, rh, true);
    renderer.fillRect(rx + 5, ry + 5, rw - 10, rh - 10, false);
    renderer.drawText(UI_12_FONT_ID, x + margin, y + margin, message, true, EpdFontFamily::BOLD);
    
    // Display only the aligned window
    // Restriction: In Portrait, Logical X has no alignment constraint, only Logical Y
    renderer.displayWindow(rx, rYAligned, rw, rHAligned);
  } else {
    // horizontal logical orientation -> Logical X maps to Physical X
    // Align Logical X and Width
    const int rXAligned = rx & ~7;
    const int rWAligned = (rx + rw + 7 & ~7) - rXAligned;
    
    renderer.fillRect(rx, ry, rw, rh, true);
    renderer.fillRect(rx + 5, ry + 5, rw - 10, rh - 10, false);
    renderer.drawText(UI_12_FONT_ID, x + margin, y + margin, message, true, EpdFontFamily::BOLD);
    
    renderer.displayWindow(rXAligned, ry, rWAligned, rh);
  }
}
