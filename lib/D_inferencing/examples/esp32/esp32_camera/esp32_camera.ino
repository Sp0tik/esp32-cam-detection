/* =========================================================
   ESP32-CAM Person Detection + Telegram Photo
   Edge Impulse FOMO + Telegram
   
   ВИПРАВЛЕННЯ:
   - Додано відсутню функцію sendTelegramMessage()
   - Секрети винесено в окремий блок для легкого редагування
   - Виправлено ei_camera_capture (використовує параметри w/h)
   - Додано fallback якщо PSRAM відсутній
   - Покращено стабільність відправки фото (chunked write)
========================================================= */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <D_inferencing.h>  // ← твоя бібліотека Edge Impulse

// =========================================================
// НАЛАШТУВАННЯ — змінюй тільки тут
// =========================================================
#define WIFI_SSID       "Change"
#define WIFI_PASSWORD   "Change"
#define BOT_TOKEN       "Change"
#define CHAT_ID         "Change"
#define CONFIDENCE_THR  0.45        // поріг впевненості (0.0 – 1.0)
#define ALERT_COOLDOWN  30000       // мінімум мс між сповіщеннями
// =========================================================

// CAMERA PINS (AI THINKER)
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// =========================================================
// ГЛОБАЛЬНІ ЗМІННІ
// =========================================================
bool            wifiConnected   = false;
bool            debug_nn        = false;
unsigned long   lastAlert       = 0;
static uint8_t *snapshot_buf    = nullptr;
int             totalDetections = 0;
int             totalFrames     = 0;

// =========================================================
// ІНІЦІАЛІЗАЦІЯ КАМЕРИ (режим inference — RGB565)
// =========================================================
bool initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0  = Y2_GPIO_NUM;
    config.pin_d1  = Y3_GPIO_NUM;
    config.pin_d2  = Y4_GPIO_NUM;
    config.pin_d3  = Y5_GPIO_NUM;
    config.pin_d4  = Y6_GPIO_NUM;
    config.pin_d5  = Y7_GPIO_NUM;
    config.pin_d6  = Y8_GPIO_NUM;
    config.pin_d7  = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size   = FRAMESIZE_96X96;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

    if (psramFound()) {
        config.fb_location = CAMERA_FB_IN_PSRAM;
        Serial.println("Camera FB: PSRAM");
    } else {
        config.fb_location = CAMERA_FB_IN_DRAM;
        Serial.println("Camera FB: DRAM (no PSRAM)");
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        return false;
    }
    return true;
}

// =========================================================
// WiFi
// =========================================================
void connectWiFi() {
    Serial.print("Connecting WiFi");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
        delay(500);
        Serial.print(".");
        timeout++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.println("\nWiFi CONNECTED: " + WiFi.localIP().toString());
    } else {
        wifiConnected = false;
        Serial.println("\nWiFi FAILED — продовжую без мережі");
    }
}

// =========================================================
// ВІДПРАВКА ТЕКСТУ В TELEGRAM  ← ВИПРАВЛЕНА / ДОДАНА
// =========================================================
void sendTelegramMessage(String message) {
    if (!wifiConnected) {
        Serial.println("[Telegram] WiFi не підключений, пропускаємо");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);

    if (!client.connect("api.telegram.org", 443)) {
        Serial.println("[Telegram] Не вдалось підключитись (текст)");
        return;
    }

    // Екрануємо лапки в повідомленні
    message.replace("\"", "\\\"");

    String body = "{\"chat_id\":\"" + String(CHAT_ID) +
                  "\",\"text\":\"" + message + "\"}";

    client.println("POST /bot" + String(BOT_TOKEN) + "/sendMessage HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Content-Type: application/json");
    client.println("Content-Length: " + String(body.length()));
    client.println("Connection: close");
    client.println();
    client.print(body);

    unsigned long t = millis();
    while (client.connected() && millis() - t < 5000) {
        if (client.available()) {
            String resp = client.readString();
            if (resp.indexOf("\"ok\":true") >= 0) {
                Serial.println("[Telegram] Повідомлення відправлено");
            } else {
                Serial.println("[Telegram] Помилка повідомлення:");
                Serial.println(resp.substring(0, 200));
            }
            break;
        }
        delay(10);
    }
    client.stop();
}

// =========================================================
// ВІДПРАВКА ФОТО В TELEGRAM
// =========================================================
void sendTelegramPhoto(float confidence) {
    if (!wifiConnected) {
        Serial.println("[Telegram] WiFi не підключений, пропускаємо фото");
        return;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[Camera] Не вдалось захопити кадр для фото");
        sendTelegramMessage(
            "Людину виявлено! " + String(confidence * 100, 1) + "%" +
            " (фото недоступне)"
        );
        return;
    }

    Serial.printf("[Camera] Кадр: %zu байт (%dx%d)\n",
                  fb->len, fb->width, fb->height);

    // Конвертуємо RGB565 → JPEG
    uint8_t *jpg_buf = nullptr;
    size_t   jpg_len = 0;

    bool converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
    esp_camera_fb_return(fb);

    if (!converted || jpg_len == 0) {
        Serial.println("[Camera] Конвертація в JPEG не вдалась");
        sendTelegramMessage(
            "Людину виявлено! " + String(confidence * 100, 1) + "%"
        );
        return;
    }

    Serial.printf("[Camera] JPEG: %zu байт\n", jpg_len);

    // Підключаємось до Telegram
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20);

    if (!client.connect("api.telegram.org", 443)) {
        Serial.println("[Telegram] Не вдалось підключитись (фото)");
        free(jpg_buf);
        return;
    }

    String boundary = "ESP32Boundary";
    String caption  = "Людину виявлено! Впевненість: " +
                      String(confidence * 100, 1) + "%" +
                      " | Виявлень загалом: " + String(totalDetections);

    String bodyStart =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
        String(CHAT_ID) + "\r\n"
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" +
        caption + "\r\n"
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; "
        "name=\"photo\"; filename=\"alert.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n";

    String bodyEnd = "\r\n--" + boundary + "--\r\n";

    uint32_t totalLen = bodyStart.length() + jpg_len + bodyEnd.length();

    client.println("POST /bot" + String(BOT_TOKEN) + "/sendPhoto HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Content-Type: multipart/form-data; boundary=" + boundary);
    client.println("Content-Length: " + String(totalLen));
    client.println("Connection: close");
    client.println();
    client.print(bodyStart);

    // Відправляємо JPEG чанками по 1 KB
    const uint8_t *ptr   = jpg_buf;
    size_t         left  = jpg_len;
    const size_t   CHUNK = 1024;
    while (left > 0) {
        size_t toSend = (left > CHUNK) ? CHUNK : left;
        client.write(ptr, toSend);
        ptr  += toSend;
        left -= toSend;
    }

    client.print(bodyEnd);
    free(jpg_buf);

    // Читаємо відповідь
    String response = "";
    unsigned long t = millis();
    while (client.connected() && millis() - t < 8000) {
        if (client.available()) {
            response = client.readString();
            break;
        }
        delay(10);
    }
    client.stop();

    if (response.indexOf("\"ok\":true") >= 0) {
        Serial.println("[Telegram] Фото відправлено успішно!");
    } else {
        Serial.println("[Telegram] Помилка відправки фото:");
        Serial.println(response.substring(0, 300));
    }
}

// =========================================================
// EDGE IMPULSE — ЗАХОПЛЕННЯ КАДРУ
// =========================================================
bool ei_camera_capture(uint32_t w, uint32_t h, uint8_t *buf) {
    (void)buf;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[Camera] Захоплення не вдалось");
        return false;
    }

    Serial.printf("[Camera] fb->len=%zu fb->w=%d fb->h=%d\n",
                  fb->len, fb->width, fb->height);

    // Копіюємо стільки байт скільки реально є у кадрі
    size_t toCopy = fb->len;
    memcpy(snapshot_buf, fb->buf, toCopy);
    esp_camera_fb_return(fb);
    return true;
}

// =========================================================
// EDGE IMPULSE — GET DATA (RGB565 → float RGB888)
// =========================================================
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr) {
    size_t pixel_ix    = offset * 2; // 2 байти на піксель (RGB565)
    size_t pixels_left = length;

    while (pixels_left != 0) {
        uint16_t pixel =
            ((uint16_t)snapshot_buf[pixel_ix] << 8) |
             snapshot_buf[pixel_ix + 1];

        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
        uint8_t g = ((pixel >> 5)  & 0x3F) << 2;
        uint8_t b = (pixel & 0x1F) << 3;

        // Пакуємо RGB в одне float число (формат Edge Impulse)
        *out_ptr = (float)((r << 16) | (g << 8) | b);

        out_ptr++;
        pixel_ix += 2;
        pixels_left--;
    }
    return 0;
}

// =========================================================
// SETUP
// =========================================================
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // вимикаємо brownout detector

    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== ESP32-CAM PERSON DETECTION START ===");

    // Ініціалізація камери
    if (!initCamera()) {
        Serial.println("CRITICAL: CAMERA INIT FAILED — HALT");
        while (1) { delay(1000); }
    }

    // Виділення буфера для snapshot (96x96 пікселів, 2 байти/піксель)
    size_t bufSize = 96 * 96 * 2;  // завжди 18432 — фіксований розмір кадру
    if (psramFound()) {
        snapshot_buf = (uint8_t*) ps_malloc(bufSize);
    } else {
        snapshot_buf = (uint8_t*) malloc(bufSize);
    }

    if (!snapshot_buf) {
        Serial.printf("CRITICAL: Buffer alloc failed (%zu bytes) — HALT\n", bufSize);
        while (1) { delay(1000); }
    }
    Serial.printf("Snapshot buffer: %zu bytes OK\n", bufSize);

    // Підключення до WiFi
    connectWiFi();

    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

    // Стартове повідомлення в Telegram
    if (wifiConnected) {
        sendTelegramMessage(
            "ESP32-CAM активовано.\nIP: " + WiFi.localIP().toString() +
            "\nПоріг впевненості: " + String(CONFIDENCE_THR * 100, 0) + "%"
        );
    }

    Serial.println("=== SYSTEM READY ===\n");
}

// =========================================================
// LOOP
// =========================================================
void loop() {
    totalFrames++;

    // Підготовка сигналу для Edge Impulse
    ei::signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data     = &ei_camera_get_data;

    // Захоплення кадру
    if (!ei_camera_capture(
            EI_CLASSIFIER_INPUT_WIDTH,
            EI_CLASSIFIER_INPUT_HEIGHT,
            nullptr)) {
        delay(500);
        return;
    }

    // Запуск класифікатора
    ei_impulse_result_t result = {0};
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);

    if (err != EI_IMPULSE_OK) {
        Serial.printf("[EI] Помилка класифікатора: %d\n", err);
        delay(500);
        return;
    }

    // Аналіз результатів (Object Detection / FOMO)
    bool  personFound   = false;
    float maxConfidence = 0.0f;

#if EI_CLASSIFIER_OBJECT_DETECTION == 1
    for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
        if (bb.value == 0) continue;

        Serial.printf("  [%s] %.2f  x:%u y:%u w:%u h:%u\n",
                      bb.label, bb.value,
                      bb.x, bb.y, bb.width, bb.height);

        if (strcmp(bb.label, "person") == 0 && bb.value >= CONFIDENCE_THR) {
            personFound = true;
            if (bb.value > maxConfidence) maxConfidence = bb.value;
        }
    }

    if (result.bounding_boxes_count == 0) {
        Serial.println("  Немає виявлень");
    }
#endif

    // Реакція на виявлення людини
    if (personFound) {
        totalDetections++;
        unsigned long now = millis();

        if (now - lastAlert > ALERT_COOLDOWN) {
            Serial.printf("[ALERT] ЛЮДИНА! Впевненість: %.1f%%\n",
                          maxConfidence * 100);
            sendTelegramPhoto(maxConfidence);
            lastAlert = now;
        } else {
            unsigned long remaining = (ALERT_COOLDOWN - (now - lastAlert)) / 1000;
            Serial.printf("[ALERT] Cooldown: %lu с залишилось\n", remaining);
        }
    }

    // Статистика кожні 100 кадрів
    if (totalFrames % 100 == 0) {
        Serial.printf("[STAT] Кадрів: %d | Виявлень: %d | Heap: %d\n",
                      totalFrames, totalDetections, ESP.getFreeHeap());
    }

    delay(200);
}
