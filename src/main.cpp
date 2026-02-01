#include <Arduino.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <SDCardManager.h>
#include <SPI.h>
#include <builtinFonts/all.h>

#include <cstring>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/boot_sleep/BootActivity.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/home/HomeActivity.h"
#include "activities/home/MyLibraryActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "fontIds.h"
#include "images/CrossLarge.h"
#include "util/WallpaperUtils.h"

HalDisplay display;
HalGPIO gpio;
MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
Activity* currentActivity;

// Double-tap detection state
unsigned long firstTapTime = 0;
bool waitingForSecondTap = false;  // True after first tap, waiting for potential second
bool doubleTapFiredThisFrame = false;  // True if double-tap action fired this frame
bool singleTapPendingThisFrame = false;  // True if deferred single-tap should fire this frame
unsigned long doubleTapSuppressUntil = 0;  // Time until which to suppress single-tap actions after double-tap
constexpr unsigned long DOUBLE_TAP_WINDOW_MS = 300;  // Max time between taps (reduced for faster response)
constexpr unsigned long DOUBLE_TAP_SUPPRESS_MS = 500;  // Suppress single-tap for this long after double-tap

// Call this from activities to check if power button release should be ignored
// Returns true if the tap was consumed by double-tap logic (double-tap fired recently, or first tap is deferred)
bool isPowerButtonConsumedByDoubleTap() {
  const unsigned long now = millis();
  // Consumed if: double-tap fired recently, OR we're waiting for second tap (first tap is deferred)
  return (now < doubleTapSuppressUntil) || waitingForSecondTap;
}

// Call this to check if a deferred single-tap should now be processed
bool shouldProcessDeferredSingleTap() {
  return singleTapPendingThisFrame;
}

// Fonts
EpdFont bookerly14RegularFont(&bookerly_14_regular);
EpdFont bookerly14BoldFont(&bookerly_14_bold);
EpdFont bookerly14ItalicFont(&bookerly_14_italic);
EpdFont bookerly14BoldItalicFont(&bookerly_14_bolditalic);
EpdFontFamily bookerly14FontFamily(&bookerly14RegularFont, &bookerly14BoldFont, &bookerly14ItalicFont,
                                   &bookerly14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont bookerly12RegularFont(&bookerly_12_regular);
EpdFont bookerly12BoldFont(&bookerly_12_bold);
EpdFont bookerly12ItalicFont(&bookerly_12_italic);
EpdFont bookerly12BoldItalicFont(&bookerly_12_bolditalic);
EpdFontFamily bookerly12FontFamily(&bookerly12RegularFont, &bookerly12BoldFont, &bookerly12ItalicFont,
                                   &bookerly12BoldItalicFont);
EpdFont bookerly16RegularFont(&bookerly_16_regular);
EpdFont bookerly16BoldFont(&bookerly_16_bold);
EpdFont bookerly16ItalicFont(&bookerly_16_italic);
EpdFont bookerly16BoldItalicFont(&bookerly_16_bolditalic);
EpdFontFamily bookerly16FontFamily(&bookerly16RegularFont, &bookerly16BoldFont, &bookerly16ItalicFont,
                                   &bookerly16BoldItalicFont);
EpdFont bookerly18RegularFont(&bookerly_18_regular);
EpdFont bookerly18BoldFont(&bookerly_18_bold);
EpdFont bookerly18ItalicFont(&bookerly_18_italic);
EpdFont bookerly18BoldItalicFont(&bookerly_18_bolditalic);
EpdFontFamily bookerly18FontFamily(&bookerly18RegularFont, &bookerly18BoldFont, &bookerly18ItalicFont,
                                   &bookerly18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

EpdFont opendyslexic8RegularFont(&opendyslexic_8_regular);
EpdFont opendyslexic8BoldFont(&opendyslexic_8_bold);
EpdFont opendyslexic8ItalicFont(&opendyslexic_8_italic);
EpdFont opendyslexic8BoldItalicFont(&opendyslexic_8_bolditalic);
EpdFontFamily opendyslexic8FontFamily(&opendyslexic8RegularFont, &opendyslexic8BoldFont, &opendyslexic8ItalicFont,
                                      &opendyslexic8BoldItalicFont);
EpdFont opendyslexic10RegularFont(&opendyslexic_10_regular);
EpdFont opendyslexic10BoldFont(&opendyslexic_10_bold);
EpdFont opendyslexic10ItalicFont(&opendyslexic_10_italic);
EpdFont opendyslexic10BoldItalicFont(&opendyslexic_10_bolditalic);
EpdFontFamily opendyslexic10FontFamily(&opendyslexic10RegularFont, &opendyslexic10BoldFont, &opendyslexic10ItalicFont,
                                       &opendyslexic10BoldItalicFont);
EpdFont opendyslexic12RegularFont(&opendyslexic_12_regular);
EpdFont opendyslexic12BoldFont(&opendyslexic_12_bold);
EpdFont opendyslexic12ItalicFont(&opendyslexic_12_italic);
EpdFont opendyslexic12BoldItalicFont(&opendyslexic_12_bolditalic);
EpdFontFamily opendyslexic12FontFamily(&opendyslexic12RegularFont, &opendyslexic12BoldFont, &opendyslexic12ItalicFont,
                                       &opendyslexic12BoldItalicFont);
EpdFont opendyslexic14RegularFont(&opendyslexic_14_regular);
EpdFont opendyslexic14BoldFont(&opendyslexic_14_bold);
EpdFont opendyslexic14ItalicFont(&opendyslexic_14_italic);
EpdFont opendyslexic14BoldItalicFont(&opendyslexic_14_bolditalic);
EpdFontFamily opendyslexic14FontFamily(&opendyslexic14RegularFont, &opendyslexic14BoldFont, &opendyslexic14ItalicFont,
                                       &opendyslexic14BoldItalicFont);
#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

void exitActivity() {
  if (currentActivity) {
    currentActivity->onExit();
    delete currentActivity;
    currentActivity = nullptr;
  }
}

void enterNewActivity(Activity* activity) {
  currentActivity = activity;
  currentActivity->onEnter();
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

// Verify power button press duration on wake-up from deep sleep
// Pre-condition: isWakeupByPowerButton() == true
void verifyPowerButtonDuration() {
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
    // Fast path for short press
    // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
    return;
  }

  // Must hold for SETTINGS.getPowerButtonDuration() (match sleep timing)
  const uint16_t requiredDuration = SETTINGS.getPowerButtonDuration();
  gpio.update();
  t2 = millis();

  if (!gpio.isPressed(HalGPIO::BTN_POWER)) {
    // Button already released - return to sleep
    gpio.startDeepSleep();
    return;
  }

  while (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() < requiredDuration) {
    delay(10);
    gpio.update();
    yield();  // Allow other tasks to run (display refresh, etc.)
  }

  if (!gpio.isPressed(HalGPIO::BTN_POWER) || gpio.getHeldTime() < requiredDuration) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    if (!gpio.isUsbConnected()) {
      gpio.startDeepSleep();
    }
    return;
  }

  // Button was held long enough - wait for it to be released before continuing
  // This prevents the main activity from processing the held button as input
  waitForPowerRelease();
}

// Enter deep sleep mode

void enterDeepSleep() {
  exitActivity();
  enterNewActivity(new SleepActivity(renderer, mappedInputManager));

  display.deepSleep();
  Serial.printf("[%lu] [   ] Power button press calibration value: %lu ms\n", millis(), t2 - t1);
  Serial.printf("[%lu] [   ] Entering deep sleep.\n", millis());

  gpio.startDeepSleep();
}

void onGoHome();
void onGoToMyLibraryWithTab(const std::string& path, MyLibraryActivity::Tab tab);
void onGoToReader(const std::string& initialEpubPath, MyLibraryActivity::Tab fromTab) {
  exitActivity();
  enterNewActivity(
      new ReaderActivity(renderer, mappedInputManager, initialEpubPath, fromTab, onGoHome, onGoToMyLibraryWithTab));
}
void onContinueReading() { onGoToReader(APP_STATE.openEpubPath, MyLibraryActivity::Tab::Recent); }

void onGoToFileTransfer() {
  exitActivity();
  enterNewActivity(new CrossPointWebServerActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToSettings() {
  exitActivity();
  enterNewActivity(new SettingsActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToMyLibrary() {
  exitActivity();
  enterNewActivity(new MyLibraryActivity(renderer, mappedInputManager, onGoHome, onGoToReader));
}

void onGoToMyLibraryWithTab(const std::string& path, MyLibraryActivity::Tab tab) {
  exitActivity();
  enterNewActivity(new MyLibraryActivity(renderer, mappedInputManager, onGoHome, onGoToReader, tab, path));
}

void onGoToBrowser() {
  exitActivity();
  enterNewActivity(new OpdsBookBrowserActivity(renderer, mappedInputManager, onGoHome));
}

void onGoHome() {
  exitActivity();
  enterNewActivity(new HomeActivity(renderer, mappedInputManager, onContinueReading, onGoToMyLibrary, onGoToSettings,
                                    onGoToFileTransfer, onGoToBrowser));
}

void setupDisplayAndFonts() {
  // Display and UI fonts already initialized early in setup() for boot screen
  Serial.printf("[%lu] [   ] Loading additional fonts\n", millis());
  renderer.insertFont(BOOKERLY_14_FONT_ID, bookerly14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(BOOKERLY_12_FONT_ID, bookerly12FontFamily);
  renderer.insertFont(BOOKERLY_16_FONT_ID, bookerly16FontFamily);
  renderer.insertFont(BOOKERLY_18_FONT_ID, bookerly18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
  renderer.insertFont(OPENDYSLEXIC_8_FONT_ID, opendyslexic8FontFamily);
  renderer.insertFont(OPENDYSLEXIC_10_FONT_ID, opendyslexic10FontFamily);
  renderer.insertFont(OPENDYSLEXIC_12_FONT_ID, opendyslexic12FontFamily);
  renderer.insertFont(OPENDYSLEXIC_14_FONT_ID, opendyslexic14FontFamily);
#endif  // OMIT_FONTS
  Serial.printf("[%lu] [   ] Fonts setup complete\n", millis());
}

void setup() {
  t1 = millis();

  gpio.begin();

  // Initialize display
  display.begin();
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // SD Card Initialization first (needed for wallpaper)
  // We need 6 open files concurrently when parsing a new chapter
  if (!SdMan.begin()) {
    Serial.printf("[%lu] [   ] SD card initialization failed\n", millis());
    // Show error screen with default CrossPoint logo since we can't load wallpaper
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();
    renderer.clearScreen();
    renderer.drawImage(CrossLarge, (pageWidth - 128) / 2, (pageHeight - 128) / 2, 128, 128);
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 + 70, "SD Card Error", true, EpdFontFamily::BOLD);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    enterNewActivity(new FullScreenMessageActivity(renderer, mappedInputManager, "SD card error", EpdFontFamily::BOLD));
    return;
  }

  // Load settings and app state early (needed for wallpaper selection)
  SETTINGS.loadFromFile();
  APP_STATE.loadFromFile();

  if (gpio.isWakeupByPowerButton() && !gpio.isUsbConnected()) {
    // Show "Opening..." popup IMMEDIATELY so user sees feedback while holding button
    // The wallpaper should still be on screen from sleep state
    renderBootPopupOnly(renderer);

    // Now verify power button duration (user sees popup while holding)
    Serial.printf("[%lu] [   ] Verifying power button press duration\n", millis());
    verifyPowerButtonDuration();
  } else {
    // Cold boot or other wake - force full wallpaper load
    renderBootWallpaper(renderer);
  }

  // Only start serial if USB connected - reduced wait time
  if (gpio.isUsbConnected()) {
    Serial.begin(115200);
    // Wait up to 500ms for Serial to be ready
    unsigned long start = millis();
    while (!Serial && (millis() - start) < 500) {
      delay(10);
    }
  }

  // Now show boot screen with user's wallpaper + "Opening..." popup
  // (Redundant call if we just woke up from power button, but safe and ensures state if woke from other sources)
  // Or we can check if currentActivity is NOT set?
  // Actually, BootActivity does nothing but show the screen.
  // We can just enter it.
  enterNewActivity(new BootActivity(renderer, mappedInputManager));

  // Load remaining stores
  KOREADER_STORE.loadFromFile();

  // First serial output
  Serial.printf("[%lu] [   ] Starting CrossPoint version " CROSSPOINT_VERSION "\n", millis());

  // Load remaining fonts
  setupDisplayAndFonts();

  RECENT_BOOKS.loadFromFile();

  // Exit boot activity before entering main activity
  exitActivity();

  if (APP_STATE.openEpubPath.empty()) {
    onGoHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.lastSleepImage = 0;
    APP_STATE.saveToFile();
    onGoToReader(path, MyLibraryActivity::Tab::Recent);
  }

  // Note: We don't wait for power button release here anymore
  // The BootActivity has already shown the popup, and the main activity
  // will handle any lingering power button input appropriately
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();

  if (Serial && millis() - lastMemPrint >= 10000) {
    Serial.printf("[%lu] [MEM] Free: %d bytes, Total: %d bytes, Min Free: %d bytes\n", millis(), ESP.getFreeHeap(),
                  ESP.getHeapSize(), ESP.getMinFreeHeap());
    lastMemPrint = millis();
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || (currentActivity && currentActivity->preventAutoSleep())) {
    lastActivityTime = millis();  // Reset inactivity timer
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (millis() - lastActivityTime >= sleepTimeoutMs) {
    Serial.printf("[%lu] [SLP] Auto-sleep triggered after %lu ms of inactivity\n", millis(), sleepTimeoutMs);
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Double-tap power button detection
  // Tracks press events: first press starts timer, second press within window = double-tap
  doubleTapFiredThisFrame = false;
  singleTapPendingThisFrame = false;
  
  if (SETTINGS.doubleTapPwrBtn != CrossPointSettings::DT_IGNORE) {
    const unsigned long now = millis();
    
    if (gpio.wasPressed(HalGPIO::BTN_POWER)) {
      if (waitingForSecondTap && (now - firstTapTime) < DOUBLE_TAP_WINDOW_MS) {
        // Second press arrived within window - double-tap detected!
        doubleTapFiredThisFrame = true;
        doubleTapSuppressUntil = now + DOUBLE_TAP_SUPPRESS_MS;  // Suppress single-tap for next 500ms
        waitingForSecondTap = false;
        firstTapTime = 0;
        
        if (SETTINGS.doubleTapPwrBtn == CrossPointSettings::DT_TOGGLE_DARK_MODE) {
          SETTINGS.readerDarkMode = !SETTINGS.readerDarkMode;
          SETTINGS.saveToFile();
          if (currentActivity) {
            currentActivity->requestScreenRefresh();
          }
        }
      } else {
        // First press - start waiting for possible second tap
        waitingForSecondTap = true;
        firstTapTime = now;
      }
    } else if (waitingForSecondTap && (now - firstTapTime) >= DOUBLE_TAP_WINDOW_MS && !gpio.isPressed(HalGPIO::BTN_POWER)) {
      // Timeout expired and button was released within window - fire deferred single-tap action
      // Note: If button is still held, this is likely a shutdown hold, not a tap
      singleTapPendingThisFrame = true;
      waitingForSecondTap = false;
      firstTapTime = 0;
    }
  }

  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  const unsigned long activityStartTime = millis();
  if (currentActivity) {
    currentActivity->loop();
  }
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      Serial.printf("[%lu] [LOOP] New max loop duration: %lu ms (activity: %lu ms)\n", millis(), maxLoopDuration,
                    activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (currentActivity && currentActivity->skipLoopDelay()) {
    yield();  // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    delay(10);  // Normal delay when no activity requires fast response
  }
}
