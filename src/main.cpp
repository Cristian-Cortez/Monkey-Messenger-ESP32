#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include <defaultMonkey.h>
#include <credentials.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"

// --- Pin Definitions ---
#define TFT_CS    5
#define TFT_RST   4
#define TFT_DC    22
#define TFT_MOSI  23
#define TFT_SCLK  18
#define SETUP_BUTTON_PIN 15
#define TOUCH_CS  14
#define TOUCH_IRQ 27

// --- Configuration ---
const char* apiUrl = "https://cristianwuzhear.pythonanywhere.com/api/monkey_messages/latest/";
// const char *apiUrl = "http://192.168.1.173:8000/api/monkey_messages/latest/";
const unsigned long POLL_INTERVAL = 300000; // 5 minutes
const unsigned long HOLD_TIME = 5000;

// --- Hardware & LVGL Objects ---
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

static const uint16_t screenWidth  = 320;
static const uint16_t screenHeight = 240;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 10]; // 10 lines buffer for efficient RAM usage
static uint16_t imageBuffer[64 * 64];

// --- UI Element Handles ---
lv_obj_t * avatar;
lv_obj_t * card_container;
lv_obj_t * badge_label;
lv_obj_t * message_label;
lv_obj_t * status_label;

// --- State Variables ---
unsigned long lastPollTime = 0;
unsigned long buttonPressStart = 0;
bool resetTriggered = false;
int lastMessageId = -1;

// Dynamic LVGL Image Descriptor for uploaded Base64 photos
static lv_img_dsc_t dynamic_img_dsc;

// LVGL Image Descriptor wrapper for defaultMonkey.h
const lv_img_dsc_t monkey_img_dsc = {
  .header = {
    .cf = LV_IMG_CF_TRUE_COLOR, // 16-bit RGB565 format
    .always_zero = 0,
    .w = MONKEY_WIDTH,                 
    .h = MONKEY_HEIGHT                 
  },
  .data_size = MONKEY_WIDTH * MONKEY_HEIGHT * 2,
  .data = (const uint8_t *)monkey,
};

bool drawBase64Image(const char* base64Str, int w, int h) {
  if (!base64Str || strlen(base64Str) == 0) return false;

  size_t outLen = 0;
  size_t inLen = strlen(base64Str);

  // Decode Base64 string directly into imageBuffer memory
  int ret = mbedtls_base64_decode(
    (unsigned char*)imageBuffer,
    sizeof(imageBuffer),
    &outLen,
    (const unsigned char*)base64Str,
    inLen
  );

  // ret == 0 means decoding succeeded
  if (ret == 0 && outLen == (size_t)(w * h * 2)) {
    // Byte-swap Big-Endian RGB565 bytes to native ESP32 Little-Endian format
    for (int i = 0; i < w * h; i++) {
      uint16_t pixel = imageBuffer[i];
      imageBuffer[i] = (pixel << 8) | (pixel >> 8);
    }
    return true;
  }

  return false;
}

// --- LVGL Touch Read Callback ---
void my_touchpad_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data) {
  if (touch.touched()) {
    TS_Point p = touch.getPoint();

    // Filter out false noise reads (pressure check)
    if (p.z > 300) {
      // Map XPT2046 ADC ranges (200-3700) to screen pixel dimensions (320x240)
      int16_t x = map(p.x, 200, 3700, 0, 320);
      int16_t y = map(p.y, 240, 3800, 0, 240);

      data->point.x = constrain(x, 0, 320);
      data->point.y = constrain(y, 0, 240);
      data->state = LV_INDEV_STATE_PR;

      Serial.printf("[TOUCH] Raw X:%d Y:%d Z:%d  ->  Mapped X:%d Y:%d\n", p.x, p.y, p.z, data->point.x, data->point.y);
      return;
    }
  }
  data->state = LV_INDEV_STATE_REL;
}

// --- LVGL Display Flush Callback ---
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.writePixels((uint16_t *)&color_p->full, w * h);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

void checkForUpdates() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[API] Wi-Fi not connected yet.");
    return;
  }

  Serial.println("[API] Fetching latest message...");

  // 1. Heap allocate client to prevent stack overflow in ESP32 loopTask
  WiFiClient* client = nullptr;
  if (strncmp(apiUrl, "https://", 8) == 0) {
    WiFiClientSecure* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client = secureClient;
  } else {
    client = new WiFiClient();
  }

  // 2. Explicit scope block ensures HTTPClient destructor runs BEFORE 'client' is deleted
  {
    HTTPClient http;

    if (!http.begin(*client, apiUrl)) {
      Serial.println("[API] Failed to connect to host endpoint.");
      lv_label_set_text(status_label, "Conn Failed");
    } else {
      http.addHeader("X-API-Key", SECRET_KEY);
      http.setTimeout(15000);

      int statusCode = http.GET();
      Serial.print("[API] HTTP Status Code: ");
      Serial.println(statusCode);

      if (statusCode == 200) {
        // Stream JSON directly from network response to prevent large String allocation in RAM
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, http.getStream());

        if (error) {
          Serial.print("[API] JSON parsing failed: ");
          Serial.println(error.c_str());
        } else {
          int currentId = doc["id"] | -1;

          if (currentId != -1 && currentId != lastMessageId) {
            lastMessageId = currentId;

            const char* messageText = doc["text"] | "";
            const char* imgBase64 = doc["image_base64"] | "";

            Serial.print("[API] New message received: ");
            Serial.println(messageText);

            // Update text content
            lv_label_set_text(message_label, messageText);

            // Strip "data:image/...;base64," header if included from Django
            if (imgBase64 != nullptr && strlen(imgBase64) > 0) {
              const char* comma = strchr(imgBase64, ',');
              if (comma != nullptr) {
                imgBase64 = comma + 1;
              }
            }

            // Decode Base64 string into 64x64 buffer
            bool hasImage = drawBase64Image(imgBase64, 64, 64);

            // Update avatar image source
            if (hasImage) {
              lv_img_set_src(avatar, &dynamic_img_dsc);
              // lv_img_set_angle(avatar, 900);
              // lv_obj_align(avatar, LV_ALIGN_TOP_MID, 0, 20);
            } else {
              lv_img_set_src(avatar, &monkey_img_dsc);
            }
            lv_obj_invalidate(avatar);

            lv_label_set_text(status_label, "Online | Synced");
          } else {
            Serial.println("[API] No new message ID update.");
            lv_label_set_text(status_label, "Online | Synced");
          }
        }
      } else {
        Serial.printf("[API] HTTP GET failed, error: %s\n", http.errorToString(statusCode).c_str());
        lv_label_set_text(status_label, "API Error");
      }

      http.end();
    }
  } // 'http' and its resources are safely destroyed here

  // 3. Free network client memory from heap
  delete client;
}

// --- Button Event Callback ---
static void refresh_btn_event_cb(lv_event_t * e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    Serial.println("[UI] Refresh button tapped.");
    lv_label_set_text(status_label, "Refreshing...");
    checkForUpdates();
  }
}

// --- Build LVGL User Interface ---
void build_sleek_ui() {
  lv_obj_t * scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x22272D), 0);

  // 1. Status Bar (Top Left)
  status_label = lv_label_create(scr);
  lv_label_set_text(status_label, "Connecting Wi-Fi...");
  lv_obj_set_style_text_color(status_label, lv_color_hex(0x8D9855), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 12, 12);

  // 2. Interactive Refresh Button (Top Right)
  lv_obj_t * btn_refresh = lv_btn_create(scr);
  lv_obj_set_size(btn_refresh, 42, 30);
  lv_obj_align(btn_refresh, LV_ALIGN_TOP_RIGHT, -10, 6);
  
  // Refresh Button Styling
  lv_obj_set_style_bg_color(btn_refresh, lv_color_hex(0x1D2127), 0);
  lv_obj_set_style_bg_color(btn_refresh, lv_color_hex(0x2E333A), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(btn_refresh, lv_color_hex(0x373E47), 0);
  lv_obj_set_style_border_width(btn_refresh, 1, 0);
  lv_obj_set_style_radius(btn_refresh, 6, 0);

  // Refresh Icon inside Button
  lv_obj_t * btn_label = lv_label_create(btn_refresh);
  lv_label_set_text(btn_label, LV_SYMBOL_REFRESH);
  lv_obj_set_style_text_color(btn_label, lv_color_hex(0xC49852), 0);
  lv_obj_center(btn_label);

  // Attach Touch Click Event
  lv_obj_add_event_cb(btn_refresh, refresh_btn_event_cb, LV_EVENT_CLICKED, NULL);

  // 3. Avatar Image (Centered at top)
  avatar = lv_img_create(scr);
  lv_img_set_src(avatar, &monkey_img_dsc);
  lv_obj_align(avatar, LV_ALIGN_TOP_MID, 0, 20);

  // 4. Main RPG Card Container (Bottom)
  card_container = lv_obj_create(scr);
  lv_obj_set_size(card_container, 300, 130);
  lv_obj_align(card_container, LV_ALIGN_BOTTOM_MID, 0, -10);
  
  lv_obj_set_style_bg_color(card_container, lv_color_hex(0x1D2127), 0);
  lv_obj_set_style_border_color(card_container, lv_color_hex(0x373E47), 0);
  lv_obj_set_style_border_width(card_container, 2, 0);
  lv_obj_set_style_radius(card_container, 10, 0);
  lv_obj_set_style_pad_all(card_container, 12, 0);

  // 5. Speaker Name Badge
  lv_obj_t * badge_bg = lv_obj_create(card_container);
  lv_obj_set_size(badge_bg, LV_SIZE_CONTENT, 26);
  lv_obj_align(badge_bg, LV_ALIGN_TOP_LEFT, -2, -2);
  lv_obj_set_style_bg_color(badge_bg, lv_color_hex(0xC49852), 0);
  lv_obj_set_style_border_width(badge_bg, 0, 0);
  lv_obj_set_style_radius(badge_bg, 4, 0);
  lv_obj_set_style_pad_hor(badge_bg, 8, 0);
  lv_obj_set_style_pad_ver(badge_bg, 2, 0);

  badge_label = lv_label_create(badge_bg);
  lv_label_set_text(badge_label, "MONKEY");
  lv_obj_set_style_text_color(badge_label, lv_color_hex(0x1D2127), 0);
  lv_obj_center(badge_label);

  // 6. Message Text Body
  message_label = lv_label_create(card_container);
  lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(message_label, 260);
  lv_label_set_text(message_label, "Waiting for new messages...");
  lv_obj_set_style_text_color(message_label, lv_color_hex(0xA9B665), 0);
  lv_obj_align(message_label, LV_ALIGN_TOP_LEFT, 0, 30);
}

void setup() {
  Serial.begin(115200);
  pinMode(SETUP_BUTTON_PIN, INPUT_PULLUP);

  // Initialize Hardware SPI for Display & Touch
  SPI.begin(TFT_SCLK, 19, TFT_MOSI, TOUCH_CS);

  // Initialize Dynamic Image Descriptor safely
  memset(&dynamic_img_dsc, 0, sizeof(lv_img_dsc_t));
  dynamic_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  dynamic_img_dsc.header.always_zero = 0;
  dynamic_img_dsc.header.w = 64;
  dynamic_img_dsc.header.h = 64;
  dynamic_img_dsc.data_size = 64 * 64 * 2;
  dynamic_img_dsc.data = (const uint8_t *)imageBuffer;

  // Initialize Touch Hardware
  touch.begin();
  touch.setRotation(1);

  // Initialize Display Hardware
  tft.begin();
  tft.setRotation(1);

  // Initialize LVGL Graphics Engine
  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 10);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  // Register Touch Input Device
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  // Render UI
  build_sleek_ui();
  lv_timer_handler();

  // Connect Wi-Fi
  WiFiManager wm;
  if (!wm.autoConnect("MonkeyManager-Setup")) {
    ESP.restart();
  }

  lv_label_set_text(status_label, "Online");
  checkForUpdates();
}

void loop() {
  lv_tick_inc(5);
  lv_timer_handler();
  delay(5);

  unsigned long currentMillis = millis();

  // Periodically poll API
  if (currentMillis - lastPollTime >= POLL_INTERVAL) {
    lastPollTime = currentMillis;
    checkForUpdates();
  }

  // Physical Reset Button Check
  if (digitalRead(SETUP_BUTTON_PIN) == LOW) {
    if (buttonPressStart == 0) {
      buttonPressStart = currentMillis;
    }

    if (currentMillis - buttonPressStart >= HOLD_TIME && !resetTriggered) {
      resetTriggered = true;
      lv_label_set_text(status_label, "Resetting Wi-Fi...");
      
      WiFiManager wm;
      wm.resetSettings();
      delay(1000);
      ESP.restart();
    }
  } else {
    buttonPressStart = 0;
  }
}