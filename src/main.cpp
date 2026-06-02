#define WIFI_SSID       "1111"
#define WIFI_PASSWORD   "12348765"
#define BOT_TOKEN       "***REDACTED***"
#define CHAT_ID         "6928524630"

// Налаштування виявлення
#define CONFIDENCE_THR      0.70   
#define ALERT_COOLDOWN      30000
#define CONFIRM_FRAMES      3      //  людина має бути на N кадрах поспіль
#define MIN_BOX_SIZE        20     //  мінімальний розмір bbox в пікселях
#define STABILITY_FRAMES    2      //  скільки кадрів без виявлення скидає лічильник

#define CAM_WIDTH   96
#define CAM_HEIGHT  96


#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <D_inferencing.h>

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

bool            wifiConnected    = false;
bool            debug_nn         = false;
unsigned long   lastAlert        = 0;
static uint8_t *snapshot_buf     = nullptr;
int             totalDetections  = 0;
int             totalFrames      = 0;

// Стан фільтра підтвердження
int             confirmCount     = 0;  // скільки кадрів поспіль є людина
int             noDetectCount    = 0;  // скільки кадрів поспіль немає людини
float           avgConfidence    = 0;  // середня впевненість за CONFIRM_FRAMES


void setCameraResolution(framesize_t size) {
    sensor_t *s = esp_camera_sensor_get();
    if (s) s->set_framesize(s, size);
    delay(300);
}


bool initCamera() {
    Serial.println("[CAM] Ініціалізація...");
    Serial.flush();

    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 8;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    config.fb_location  = CAMERA_FB_IN_PSRAM;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] ПОМИЛКА init: 0x%x\n", err);
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, 1);
        s->set_brightness(s, 1);
        s->set_saturation(s, 0);
        s->set_contrast(s, 1);
        s->set_sharpness(s, 2);
        s->set_denoise(s, 1);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_aec2(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_lenc(s, 1);
        s->set_hmirror(s, 0);
        s->set_framesize(s, FRAMESIZE_96X96);
    }

    Serial.println("[CAM] OK");
    return true;
}


void connectWiFi() {
    Serial.print("[WiFi] Підключення");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int t = 0;
    while (WiFi.status() != WL_CONNECTED && t < 20) {
        delay(500); Serial.print("."); t++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.println("\n[WiFi] OK: " + WiFi.localIP().toString());
    } else {
        Serial.println("\n[WiFi] НЕВДАЛО");
    }
}


void sendTelegramMessage(String message) {
    if (!wifiConnected) return;
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);
    if (!client.connect("api.telegram.org", 443)) return;

    message.replace("\"", "\\\"");
    message.replace("\n", "\\n");
    String body = "{\"chat_id\":\"" + String(CHAT_ID) +
                  "\",\"text\":\"" + message + "\"}";

    client.println("POST /bot" + String(BOT_TOKEN) + "/sendMessage HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Content-Type: application/json");
    client.println("Content-Length: " + String(body.length()));
    client.println("Connection: close");
    client.println();
    client.print(body);

    unsigned long ts = millis();
    while (client.connected() && millis() - ts < 5000) {
        if (client.available()) {
            String r = client.readString();
            Serial.println(r.indexOf("\"ok\":true") >= 0
                ? "[TG] Надіслано" : "[TG] Помилка");
            break;
        }
        delay(10);
    }
    client.stop();
}


void sendTelegramPhoto(float confidence) {
    if (!wifiConnected) {
        sendTelegramMessage("Людину виявлено! " +
            String(confidence * 100, 1) + "%");
        return;
    }

    Serial.println("[CAM] Перемикання на VGA...");
    setCameraResolution(FRAMESIZE_VGA);

    for (int i = 0; i < 3; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
        delay(100);
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        setCameraResolution(FRAMESIZE_96X96);
        sendTelegramMessage("Людину виявлено! " +
            String(confidence * 100, 1) + "% (фото недоступне)");
        return;
    }

    Serial.printf("[CAM] Фото: %zu байт %dx%d\n",
                  fb->len, fb->width, fb->height);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(30);

    if (!client.connect("api.telegram.org", 443)) {
        esp_camera_fb_return(fb);
        setCameraResolution(FRAMESIZE_96X96);
        return;
    }

    String boundary = "ESP32Boundary";
    String caption  = "👤 Людину виявлено!\n"
                      "Впевненість: " + String(confidence * 100, 1) + "%\n"
                      "Підтверджено: " + String(confirmCount) + " кадри\n"
                      "Всього виявлень: " + String(totalDetections);

    String head =
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

    String tail = "\r\n--" + boundary + "--\r\n";
    uint32_t totalLen = head.length() + fb->len + tail.length();

    client.println("POST /bot" + String(BOT_TOKEN) + "/sendPhoto HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Content-Type: multipart/form-data; boundary=" + boundary);
    client.println("Content-Length: " + String(totalLen));
    client.println("Connection: close");
    client.println();
    client.print(head);

    const uint8_t *p = fb->buf;
    size_t left = fb->len;
    while (left > 0) {
        size_t n = left > 2048 ? 2048 : left;
        client.write(p, n); p += n; left -= n;
    }
    client.print(tail);
    esp_camera_fb_return(fb);

    String resp = "";
    unsigned long ts = millis();
    while (client.connected() && millis() - ts < 10000) {
        if (client.available()) { resp = client.readString(); break; }
        delay(10);
    }
    client.stop();

    Serial.println(resp.indexOf("\"ok\":true") >= 0
        ? "[TG] Фото надіслано!"
        : "[TG] Помилка: " + resp.substring(0, 200));

    setCameraResolution(FRAMESIZE_96X96);
    delay(300);
}


bool ei_camera_capture(uint32_t w, uint32_t h, uint8_t *buf) {
    (void)w; (void)h; (void)buf;
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { Serial.println("[EI] Кадр не захоплено"); return false; }

    bool converted = jpg2rgb565(fb->buf, fb->len,
                                snapshot_buf, JPG_SCALE_NONE);
    esp_camera_fb_return(fb);
    if (!converted) {
        Serial.println("[EI] Конвертація не вдалась");
        return false;
    }
    return true;
}


static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr) {
    for (size_t i = 0; i < length; i++) {
        size_t dst_pixel = offset + i;
        size_t dst_x = dst_pixel % EI_CLASSIFIER_INPUT_WIDTH;
        size_t dst_y = dst_pixel / EI_CLASSIFIER_INPUT_WIDTH;
        size_t src_x = dst_x * CAM_WIDTH  / EI_CLASSIFIER_INPUT_WIDTH;
        size_t src_y = dst_y * CAM_HEIGHT / EI_CLASSIFIER_INPUT_HEIGHT;
        size_t px = (src_y * CAM_WIDTH + src_x) * 2;

        uint16_t raw = ((uint16_t)snapshot_buf[px] << 8)
                      | snapshot_buf[px + 1];
        uint8_t r = ((raw >> 11) & 0x1F) << 3;
        uint8_t g = ((raw >> 5)  & 0x3F) << 2;
        uint8_t b = (raw & 0x1F) << 3;
        out_ptr[i] = (float)((r << 16) | (g << 8) | b);
    }
    return 0;
}


// ФІЛЬТР ПІДТВЕРДЖЕННЯ
// Повертає true тільки якщо людина є на CONFIRM_FRAMES кадрах поспіль

bool confirmDetection(bool detected, float confidence) {
    if (detected) {
        noDetectCount = 0;
        confirmCount++;
        // Ковзне середнє впевненості
        avgConfidence = avgConfidence * 0.6f + confidence * 0.4f;

        Serial.printf("[FILTER] Підтвердження: %d/%d | avg: %.1f%%\n",
                      confirmCount, CONFIRM_FRAMES, avgConfidence * 100);

        if (confirmCount >= CONFIRM_FRAMES) {
            return true; 
        }
    } else {
        noDetectCount++;
        if (noDetectCount >= STABILITY_FRAMES) {
            // Скидаємо лічильник якщо кілька кадрів без виявлення
            if (confirmCount > 0) {
                Serial.printf("[FILTER] Скидання після %d кадрів без виявлення\n",
                              noDetectCount);
            }
            confirmCount  = 0;
            avgConfidence = 0;
            noDetectCount = 0;
        }
    }
    return false;
}


void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);
    delay(1500);

    Serial.println("\n ESP32-CAM START ");
    Serial.printf("[INIT] Model: %dx%d | THR: %.0f%% | Confirm: %d кадри\n",
                  EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT,
                  CONFIDENCE_THR * 100, CONFIRM_FRAMES);
    Serial.printf("[INIT] PSRAM: %s | Heap: %d\n",
                  psramFound() ? "є" : "немає", ESP.getFreeHeap());
    Serial.flush();

    if (!initCamera()) {
        Serial.println("[INIT] КАМЕРА НЕ ЗАПУСТИЛАСЬ");
        while (1) delay(1000);
    }

    size_t bufSize = (size_t)CAM_WIDTH * CAM_HEIGHT * 2;
    snapshot_buf = (uint8_t*) ps_malloc(bufSize);
    if (!snapshot_buf) {
        Serial.println("[INIT] БУФЕР НЕ ВИДІЛЕНО");
        while (1) delay(1000);
    }

    connectWiFi();
    Serial.printf("[INIT] Heap після WiFi: %d\n", ESP.getFreeHeap());

    if (wifiConnected) {
        sendTelegramMessage(
            "ESP32-CAM активовано.\n"
            "IP: " + WiFi.localIP().toString() + "\n"
            "Поріг: " + String(CONFIDENCE_THR * 100, 0) + "%\n"
            "Підтвердження: " + String(CONFIRM_FRAMES) + " кадри"
        );
    }

    Serial.println(" READY \n");
}


void loop() {
    totalFrames++;

    ei::signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data     = &ei_camera_get_data;

    if (!ei_camera_capture(CAM_WIDTH, CAM_HEIGHT, nullptr)) {
        delay(500);
        return;
    }

    ei_impulse_result_t result = {0};
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);
    if (err != EI_IMPULSE_OK) {
        Serial.printf("[EI] Помилка: %d\n", err);
        delay(500);
        return;
    }

    bool  personFound   = false;
    float maxConfidence = 0.0f;

#if EI_CLASSIFIER_OBJECT_DETECTION == 1
    for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
        auto bb = result.bounding_boxes[i];
        if (bb.value == 0) continue;

        // поріг впевненості
        if (bb.value < CONFIDENCE_THR) {
            Serial.printf("  [SKIP] %s %.1f%% — нижче порогу\n",
                          bb.label, bb.value * 100);
            continue;
        }

        // мінімальний розмір bbox
        if (bb.width < MIN_BOX_SIZE || bb.height < MIN_BOX_SIZE) {
            Serial.printf("  [SKIP] %s — bbox замалий (%dx%d)\n",
                          bb.label, bb.width, bb.height);
            continue;
        }

        Serial.printf("  [OK] %s %.1f%% bbox:%dx%d\n",
                      bb.label, bb.value * 100, bb.width, bb.height);

        if (strcmp(bb.label, "person") == 0) {
            personFound = true;
            if (bb.value > maxConfidence)
                maxConfidence = bb.value;
        }
    }

    if (result.bounding_boxes_count == 0 && totalFrames % 50 == 0)
        Serial.println("  [EI] Немає виявлень");
#endif

    // підтвердження на кількох кадрах поспіль
    bool confirmed = confirmDetection(personFound, maxConfidence);

    if (confirmed) {
        totalDetections++;
        unsigned long now = millis();
        if (now - lastAlert > ALERT_COOLDOWN) {
            Serial.printf("[!] ЛЮДИНА ПІДТВЕРДЖЕНА! avg:%.1f%%\n",
                          avgConfidence * 100);
            sendTelegramPhoto(avgConfidence);
            lastAlert    = now;
            confirmCount = 0; // скидаємо після відправки
        } else {
            Serial.printf("[!] Cooldown: %lus\n",
                (ALERT_COOLDOWN - (now - lastAlert)) / 1000);
        }
    }

    if (totalFrames % 100 == 0) {
        Serial.printf("[STAT] Кадрів:%d Виявлень:%d Heap:%d\n",
                      totalFrames, totalDetections, ESP.getFreeHeap());
    }

    delay(200);
}