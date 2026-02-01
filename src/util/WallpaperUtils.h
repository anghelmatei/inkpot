#pragma once

#include <SDCardManager.h>

#include <string>
#include <vector>

class GfxRenderer;
class Bitmap;

bool listCustomWallpapers(std::vector<std::string>& files);
bool openCustomWallpaperFile(size_t index, FsFile& file, std::string& filename);
void renderWallpaperBitmap(GfxRenderer& renderer, const Bitmap& bitmap, bool crop, uint8_t filter = 0);
void renderPopup(GfxRenderer& renderer, const char* message);
void renderBootWallpaper(GfxRenderer& renderer);

// Render ONLY the boot popup ("Opening...") over existing screen content
void renderBootPopupOnly(GfxRenderer& renderer);
