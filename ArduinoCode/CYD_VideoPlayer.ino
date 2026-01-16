// CYD Video Player - V3p0 (with Serial debug)
// Tutorial : https://youtu.be/jYcxUgxz9ks
// Use board "ESP32 Dev Module" (tested with Arduino ESP32 core 3.x)
//
// V3p0 changes:
// - Ignore macOS AppleDouble + hidden dotfiles when scanning /mjpeg (._*, .DS_Store, etc.)
// - Loop selected file forever; only advance to next file on BOOT short press
// - Splash screen at boot (terraintronics.com rotated 90° right for 5 seconds)
// - BOOT long press toggles LCD invert colors and saves in NVS

//Tested by Dafydd (on PC) and TheosPrimus on Mac. 1/16/2026

#include <Arduino_GFX_Library.h>
#include "MjpegClass.h"
#include "SD.h"
#include <Preferences.h>

// -------------------- Firmware Version --------------------
#define FW_VERSION "V3p0"

// Pins for the display
#define BL_PIN 21 // On some cheap yellow display model, BL pin is 27
#define SD_CS 5
#define SD_MISO 19
#define SD_MOSI 23
#define SD_SCK 18

#define BOOT_PIN 0 // GPIO0 (BOOT)

// Timing
#define BUTTON_DEBOUNCE_MS 30
#define LONG_PRESS_MS 1200
#define SPLASH_MS 5000

// SPI speeds
#define DISPLAY_SPI_SPEED 40000000L // 40MHz
#define SD_SPI_SPEED 40000000L      // 40MHz

const char *MJPEG_FOLDER = "/mjpeg";

// Storage for files to read on the SD card
#define MAX_FILES 20
String mjpegFileList[MAX_FILES];
uint32_t mjpegFileSizes[MAX_FILES] = {0};
int mjpegCount = 0;
static int currentMjpegIndex = 0;

// MJPEG globals
MjpegClass mjpeg;
int total_frames;
unsigned long total_read_video;
unsigned long total_decode_video;
unsigned long total_show_video;
unsigned long start_ms, curr_ms;
long output_buf_size, estimateBufferSize;
uint8_t *mjpeg_buf;
uint16_t *output_buf;

// Display
Arduino_DataBus *bus = new Arduino_HWSPI(2 /* DC */, 15 /* CS */, 14 /* SCK */, 13 /* MOSI */, 12 /* MISO */);
Arduino_GFX *gfx = new Arduino_ILI9341(bus);

// SD Card reader is on a separate SPI
SPIClass sd_spi(VSPI);

// NVS prefs
Preferences prefs;
static bool gInvert = true;

// Button state (polled)
static bool btnPrev = true; // INPUT_PULLUP idle HIGH
static uint32_t btnDownMs = 0;
static uint32_t btnLastChangeMs = 0;

// Control flags
static bool nextFileRequested = false;

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------
void loadDisplayPrefs()
{
  prefs.begin("disp", true);
  gInvert = prefs.getBool("inv", gInvert);
  prefs.end();
}

void saveDisplayPrefs()
{
  prefs.begin("disp", false);
  prefs.putBool("inv", gInvert);
  prefs.end();
}

void applyDisplayPrefs()
{
  gfx->invertDisplay(gInvert);
}

// Splash screen: black + "terraintronics.com" rotated 90° right for 5s
// Now also shows FW_VERSION under the URL.
void showSplash()
{
  uint8_t oldRotation = 0; // we use rotation 0 for video

  // Rotate 90° right for splash only
  gfx->setRotation(1);
  gfx->fillScreen(RGB565_BLACK);

  const char *msg = "terraintronics.com";
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);

  // Center text using default GFX font metrics approximation
  int16_t titleCharW = 6 * 2;
  int16_t titleCharH = 8 * 2;
  int16_t titleW = (int16_t)strlen(msg) * titleCharW;

  int16_t x = (gfx->width() - titleW) / 2;
  int16_t y = (gfx->height() - titleCharH) / 2;
  if (x < 0) x = 0;
  if (y < 0) y = 0;

  gfx->setCursor(x, y);
  gfx->print(msg);

  // Version line (smaller) below the URL
  gfx->setTextSize(1);
  const char *ver = FW_VERSION;

  int16_t verCharW = 6 * 1;
  int16_t verCharH = 8 * 1;
  int16_t verW = (int16_t)strlen(ver) * verCharW;

  int16_t vx = (gfx->width() - verW) / 2;
  int16_t vy = y + titleCharH + 6; // below title with small gap
  if (vx < 0) vx = 0;
  if (vy < 0) vy = 0;

  gfx->setCursor(vx, vy);
  gfx->print(ver);

  delay(SPLASH_MS);

  // Restore rotation for video
  gfx->setRotation(oldRotation);
}

// Call once per frame (poll BOOT)
void handleBootButton()
{
  bool btn = digitalRead(BOOT_PIN); // LOW when pressed
  uint32_t now = millis();

  // Debounce edges
  if (btn != btnPrev && (now - btnLastChangeMs) > BUTTON_DEBOUNCE_MS)
  {
    btnLastChangeMs = now;

    if (btn == LOW)
    {
      // pressed
      btnDownMs = now;
    }
    else
    {
      // released
      uint32_t held = now - btnDownMs;

      if (held >= LONG_PRESS_MS)
      {
        // Long press: toggle inversion + save + apply (stay on same file)
        gInvert = !gInvert;
        saveDisplayPrefs();
        applyDisplayPrefs();
        Serial.printf("Invert toggled -> %s (saved)\n", gInvert ? "ON" : "OFF");
      }
      else
      {
        // Short press: request next file
        nextFileRequested = true;
      }
    }

    btnPrev = btn;
  }
}

// Function helper display sizes on the serial monitor
String formatBytes(size_t bytes)
{
  if (bytes < 1024) return String(bytes) + " B";
  if (bytes < (1024 * 1024)) return String(bytes / 1024.0, 2) + " KB";
  return String(bytes / 1024.0 / 1024.0, 2) + " MB";
}

// ------------------------------------------------------------
// Setup / Loop
// ------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  Serial.printf("CYD Video Player %s\n", FW_VERSION);

  // Backlight
  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);

  // Button
  pinMode(BOOT_PIN, INPUT_PULLUP);

  // Load saved display inversion
  loadDisplayPrefs();

  // Display init
  Serial.println("Display initialization");
  if (!gfx->begin(DISPLAY_SPI_SPEED))
  {
    Serial.println("Display initialization failed!");
    while (true) { }
  }

  gfx->setRotation(0);
  applyDisplayPrefs();
  gfx->fillScreen(RGB565_BLACK);

  Serial.printf("Screen size Width=%d,Height=%d\n", gfx->width(), gfx->height());
  Serial.printf("Invert=%s (tap BOOT=next file, hold BOOT=toggle)\n", gInvert ? "ON" : "OFF");

  // Splash before SD/video
  showSplash();

  // SD init
  Serial.println("SD Card initialization");
  if (!SD.begin(SD_CS, sd_spi, SD_SPI_SPEED, "/sd"))
  {
    Serial.println("ERROR: File system mount failed!");
    while (true) { }
  }

  // Buffers
  Serial.println("Buffer allocation");
  output_buf_size = gfx->width() * 4 * 2;
  output_buf = (uint16_t *)heap_caps_aligned_alloc(16, output_buf_size * sizeof(uint16_t), MALLOC_CAP_DMA);
  if (!output_buf)
  {
    Serial.println("output_buf aligned_alloc failed!");
    while (true) { }
  }

  estimateBufferSize = gfx->width() * gfx->height() * 2 / 5;
  mjpeg_buf = (uint8_t *)heap_caps_malloc(estimateBufferSize, MALLOC_CAP_8BIT);
  if (!mjpeg_buf)
  {
    Serial.println("mjpeg_buf allocation failed!");
    while (true) { }
  }

  loadMjpegFilesList();
}

void loop()
{
  // Loop the currently-selected file forever.
  // Only advance to next file when BOOT is short-pressed.
  while (true)
  {
    bool next = playSelectedMjpeg(currentMjpegIndex);

    if (next)
    {
      currentMjpegIndex++;
      if (currentMjpegIndex >= mjpegCount) currentMjpegIndex = 0;
    }
    // else: replay same index again
  }
}

// ------------------------------------------------------------
// File list
// ------------------------------------------------------------
void loadMjpegFilesList()
{
  File mjpegDir = SD.open(MJPEG_FOLDER);
  if (!mjpegDir)
  {
    Serial.printf("Failed to open %s folder\n", MJPEG_FOLDER);
    while (true) { }
  }

  mjpegCount = 0;

  while (true)
  {
    File file = mjpegDir.openNextFile();
    if (!file) break;

    if (!file.isDirectory())
    {
      String name = file.name();

      // V3p0: ignore macOS AppleDouble and hidden dotfiles
      // e.g. "._video.mjpeg", ".DS_Store"
      if (name.startsWith("."))
      {
        Serial.printf("Skipping hidden file: %s\n", name.c_str());
      }
      else if (name.endsWith(".mjpeg"))
      {
        mjpegFileList[mjpegCount] = name;
        mjpegFileSizes[mjpegCount] = file.size();
        Serial.printf("Found MJPEG: %s (%lu bytes / %s)\n",
                      name.c_str(),
                      (unsigned long)mjpegFileSizes[mjpegCount],
                      formatBytes(mjpegFileSizes[mjpegCount]).c_str());

        mjpegCount++;
        if (mjpegCount >= MAX_FILES)
        {
          file.close();
          break;
        }
      }
    }

    file.close();
  }

  mjpegDir.close();

  Serial.printf("%d mjpeg files read\n", mjpegCount);

  // Optional: print summary list
  for (int i = 0; i < mjpegCount; i++)
  {
    Serial.printf("  [%d] %s  (%s)\n", i, mjpegFileList[i].c_str(), formatBytes(mjpegFileSizes[i]).c_str());
  }
}

// ------------------------------------------------------------
// Playback
// ------------------------------------------------------------
bool playSelectedMjpeg(int mjpegIndex)
{
  String fullPath = String(MJPEG_FOLDER) + "/" + mjpegFileList[mjpegIndex];
  char mjpegFilename[128];
  fullPath.toCharArray(mjpegFilename, sizeof(mjpegFilename));

  Serial.printf("Playing %s\n", mjpegFilename);
  return mjpegPlayFromSDCard(mjpegFilename);
}

// Callback function to draw a JPEG
int jpegDrawCallback(JPEGDRAW *pDraw)
{
  unsigned long s = millis();
  gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  total_show_video += millis() - s;
  return 1;
}

// Play a mjpeg stored on the SD card
// Returns true if short-press requested next file; false if it ended naturally.
bool mjpegPlayFromSDCard(char *mjpegFilename)
{
  Serial.printf("Opening %s\n", mjpegFilename);
  File mjpegFile = SD.open(mjpegFilename, "r");

  if (!mjpegFile || mjpegFile.isDirectory())
  {
    Serial.printf("ERROR: Failed to open %s file for reading\n", mjpegFilename);
    return false;
  }

  Serial.println("MJPEG start");
  gfx->fillScreen(RGB565_BLACK);

  start_ms = millis();
  curr_ms = millis();
  total_frames = 0;
  total_read_video = 0;
  total_decode_video = 0;
  total_show_video = 0;

  mjpeg.setup(&mjpegFile, mjpeg_buf, jpegDrawCallback, true,
              0, 0, gfx->width(), gfx->height());

  nextFileRequested = false;

  while (mjpegFile.available() && mjpeg.readMjpegBuf())
  {
    // Read timing bucket
    total_read_video += millis() - curr_ms;
    curr_ms = millis();

    // Decode + draw
    mjpeg.drawJpg();
    total_decode_video += millis() - curr_ms;
    curr_ms = millis();

    total_frames++;

    // BOOT button handling (tap=next file, hold=toggle invert)
    handleBootButton();
    if (nextFileRequested) break;
  }

  int time_used = millis() - start_ms;
  Serial.println("MJPEG end");
  mjpegFile.close();

  bool next = nextFileRequested;
  nextFileRequested = false;

  float fps = (time_used > 0) ? (1000.0f * total_frames / time_used) : 0.0f;
  total_decode_video -= total_show_video;

  Serial.printf("Total frames: %d\n", total_frames);
  Serial.printf("Time used: %d ms\n", time_used);
  Serial.printf("Average FPS: %0.1f\n", fps);
  Serial.printf("Read MJPEG: %lu ms (%0.1f %%)\n", total_read_video, 100.0 * total_read_video / time_used);
  Serial.printf("Decode video: %lu ms (%0.1f %%)\n", total_decode_video, 100.0 * total_decode_video / time_used);
  Serial.printf("Show video: %lu ms (%0.1f %%)\n", total_show_video, 100.0 * total_show_video / time_used);
  Serial.printf("Video size (wxh): %d×%d, scale factor=%d\n", mjpeg.getWidth(), mjpeg.getHeight(), mjpeg.getScale());

  return next;
}
