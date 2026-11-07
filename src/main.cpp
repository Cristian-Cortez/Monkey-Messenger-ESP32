#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include "../include/defaultMonkey.h"
#include "../include/credentials.h"

// TFT pins
#define TFT_CS    5
#define TFT_RST   4
#define TFT_DC    22
#define TFT_MOSI  23
#define TFT_SCLK  18

// Custom 16-bit RGB565 Retro Colors
#define COLOR_BOX_BG      0x6d6b  // matcha green
#define COLOR_BOX_BORDER  0xFFFF  // Bright White
#define COLOR_ACCENT      0x22e2  // dark green
#define COLOR_TEXT        0xFFFF  // White Text
#define COLOR_BG          0xf77c  // cream brown

// Create the display
Adafruit_ILI9341 tft(
  TFT_CS,
  TFT_DC,
  TFT_MOSI,
  TFT_SCLK,
  TFT_RST
);

// Wi-Fi setup button
const int SETUP_BUTTON_PIN = 15;
const unsigned long HOLD_TIME = 5000;
const unsigned long REFRESH_TIME = 3000;

unsigned long buttonPressStart = 0;
bool resetTriggered = false;

const char* apiUrl = "https://cristianwuzhear.pythonanywhere.com/api/monkey_messages/latest/";

// Polling configuration
unsigned long lastPollTime = 0;
const unsigned long POLL_INTERVAL = 300000; // Check every 5 min

int lastMessageId = -1; // Stores current message ID to detect updates

void drawTransparentRGBBitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h, uint16_t transparentColor) {
  for (int16_t j = 0; j < h; j++) {
    for (int16_t i = 0; i < w; i++) {
      // Read 16-bit color value from flash memory (PROGMEM)
      uint16_t pixel = pgm_read_word(&bitmap[j * w + i]);
      
      // Only draw the pixel if it doesn't match the transparent color key
      if (pixel != transparentColor) {
        tft.drawPixel(x + i, y + j, pixel);
      }
    }
  }
}

void drawRPGBox(String speakerName) {
  tft.fillScreen(COLOR_BG);

  // 1. Calculate centered coordinates for the upper half (Y: 0 to 110)
  int imgX = (320 - MONKEY_WIDTH) / 2;  // Centered horizontally (X = 128 for 64px img)
  int imgY = (115 - MONKEY_HEIGHT) / 2; // Centered vertically in top half (Y = 25 for 64px img)

  // 2. Draw the Avatar Image
  // tft.drawRGBBitmap(imgX, imgY, monkey, MONKEY_WIDTH, MONKEY_HEIGHT);
  drawTransparentRGBBitmap(imgX, imgY, monkey, MONKEY_WIDTH, MONKEY_HEIGHT, 0xffff);

  // Screen Dimensions for Rotation 1: 320x240
  int boxX = 10;
  int boxY = 120;
  int boxW = 300;
  int boxH = 110;

  // 1. Draw Outer & Inner Borders (Classic RPG Layering)
  tft.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_BOX_BORDER);            // Outer White
  tft.fillRoundRect(boxX + 3, boxY + 3, boxW - 6, boxH - 6, 6, COLOR_ACCENT); // Cyan Accent Line
  tft.fillRoundRect(boxX + 5, boxY + 5, boxW - 10, boxH - 10, 5, COLOR_BOX_BG); // Dark Navy Core

  // 2. Draw Speaker Name Badge
  int badgeX = boxX + 16;
  int badgeY = boxY - 10;
  int badgeW = speakerName.length() * 12 + 20;
  int badgeH = 22;

  tft.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 4, COLOR_ACCENT);
  tft.fillRoundRect(badgeX + 2, badgeY + 2, badgeW - 4, badgeH - 4, 3, COLOR_BOX_BG);

  tft.setTextSize(2);
  tft.setTextColor(COLOR_ACCENT);
  tft.setCursor(badgeX + 10, badgeY + 4);
  tft.print(speakerName);
}

void printRPGMessage(String message, bool animate = true) {
  int startX = 22;
  int startY = 140;
  int maxX = 290;
  int lineSpacing = 22;
  int charWidth = 12; // Text size 2 character width

  int cursorX = startX;
  int cursorY = startY;

  tft.setTextSize(2);
  tft.setTextColor(COLOR_TEXT);

  // Word wrapping loop
  int startIdx = 0;
  while (startIdx < message.length()) {
    int endIdx = message.indexOf(' ', startIdx);
    if (endIdx == -1) endIdx = message.length();

    String word = message.substring(startIdx, endIdx);

    // If word exceeds right border, wrap to next line
    if (cursorX + (word.length() * charWidth) > maxX) {
      cursorX = startX;
      cursorY += lineSpacing;
    }

    // Print the word character by character (Typewriter effect)
    for (int i = 0; i < word.length(); i++) {
      tft.setCursor(cursorX, cursorY);
      tft.print(word[i]);
      cursorX += charWidth;

      if (animate) delay(30); // Speed of typewriter animation (ms)
    }

    // Add space after word
    if (endIdx < message.length()) {
      cursorX += charWidth;
      startIdx = endIdx + 1;
    } else {
      break;
    }
  }
}

void checkForUpdates() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(apiUrl);
  http.addHeader("X-API-Key", SECRET_KEY);

  int statusCode = http.GET();

  if (statusCode == 200) {
    String response = http.getString();

    // 1. Extract "id" from JSON
    int currentId = -1;
    int idIdx = response.indexOf("\"id\":");
    if (idIdx != -1) {
      int commaIdx = response.indexOf(",", idIdx);
      currentId = response.substring(idIdx + 5, commaIdx).toInt();
    }

    // 2. Only redraw if a NEW message arrived
    if (currentId != -1 && currentId != lastMessageId) {
      lastMessageId = currentId;

      // Extract "text" from JSON
      String messageText = "";
      int textIdx = response.indexOf("\"text\":");
      if (textIdx != -1) {
        int startQuote = response.indexOf("\"", textIdx + 7);
        int endQuote = response.indexOf("\"", startQuote + 1);
        if (startQuote != -1 && endQuote != -1) {
          messageText = response.substring(startQuote + 1, endQuote);
        }
      }

      // Redraw display with new message
      drawRPGBox("MONKEY");
      printRPGMessage(messageText, true); // Play typewriter animation
    }
  }

  http.end();
}

void setup() {
  Serial.begin(115200);
  pinMode(SETUP_BUTTON_PIN, INPUT_PULLUP);

  // Initialize display
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(10, 10);
  tft.println("Connecting Wi-Fi...");

  // Wi-Fi AutoConnect
  WiFiManager wm;
  if (!wm.autoConnect("MonkeyManager-Setup")) {
    ESP.restart();
  }

  // Fetch initial message immediately on boot
  checkForUpdates();

}

void loop() {
  unsigned long currentMillis = millis();

  // 1. Check for API updates periodically (non-blocking)
  if (currentMillis - lastPollTime >= POLL_INTERVAL) {
    lastPollTime = currentMillis;
    checkForUpdates();
  }

  // 2. Wi-Fi Reset Button Handler
  if (digitalRead(SETUP_BUTTON_PIN) == LOW) {
    if (buttonPressStart == 0) {
      buttonPressStart = currentMillis;
    }

    if (currentMillis - buttonPressStart >= HOLD_TIME && !resetTriggered) {
      resetTriggered = true;
      tft.fillScreen(ILI9341_BLACK);
      tft.setCursor(10, 10);
      tft.setTextColor(ILI9341_YELLOW);
      tft.println("Resetting Wi-Fi...");
      
      WiFiManager wm;
      wm.resetSettings();
      delay(1000);
      ESP.restart();
    }else if(currentMillis - buttonPressStart <= REFRESH_TIME){
      checkForUpdates();
    }
  } else {
    buttonPressStart = 0;
  }

}