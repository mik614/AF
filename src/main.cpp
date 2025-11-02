#include <Arduino.h>
#include <WiFi.h>
#include <cstring>
extern "C" {
#include "esp_wifi.h"
#include "esp_netif.h"
#include "tcpip_adapter.h"
#include "lwip/lwip_napt.h"
}
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- Настройки проекта ---
static constexpr const char *PROJECT_NAME = "ESP32 Wi-Fi Repeater";
static constexpr const char *STA_SSID = "MainNetwork";      // Задайте SSID основной сети
static constexpr const char *STA_PASSWORD = "MainPassword"; // Задайте пароль основной сети
static constexpr const char *AP_SSID = "ESP32Repeater";      // Имя создаваемой точки доступа
static constexpr const char *AP_PASSWORD = "esp32pass";     // Пароль создаваемой точки доступа
static constexpr uint8_t WIFI_CHANNEL = 6;                   // Жестко заданный канал Wi-Fi
static constexpr gpio_num_t OLED_RESET_PIN = GPIO_NUM_NC;    // Аппаратный reset не используется

static constexpr uint16_t SCREEN_WIDTH = 128;
static constexpr uint16_t SCREEN_HEIGHT = 64;
static constexpr uint8_t SPEED_SEGMENTS = 16;
static constexpr uint8_t ACTIVE_SEGMENTS = 4;

static constexpr uint32_t DISPLAY_UPDATE_INTERVAL_MS = 1000; // Обновление OLED не чаще 1 Гц

// --- Глобальные объекты и состояния ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN);

struct TrafficCounters {
    uint64_t txBytes;
    uint64_t rxBytes;
};

static TrafficCounters s_previousCounters{0, 0};
static float s_avgSpeedMbps = 0.0f;
static uint16_t s_clientCount = 0;
static uint32_t s_lastSampleTime = 0;
static SemaphoreHandle_t s_statsMutex = nullptr;
static TaskHandle_t s_displayTaskHandle = nullptr;
static uint8_t s_speedPhase = 0;

// --- Вспомогательные функции ---

// Получение агрегированных счетчиков трафика через драйвер Wi-Fi.
static bool fetchTrafficCounters(TrafficCounters &outCounters, uint16_t &outClients) {
    wifi_sta_list_t wifiStaList;
    tcpip_adapter_sta_list_t adapterStaList;
    memset(&wifiStaList, 0, sizeof(wifiStaList));
    memset(&adapterStaList, 0, sizeof(adapterStaList));

    if (esp_wifi_ap_get_sta_list(&wifiStaList) != ESP_OK) {
        return false;
    }
    if (tcpip_adapter_get_sta_list(&wifiStaList, &adapterStaList) != ESP_OK) {
        return false;
    }

    outCounters = {0, 0};
    outClients = adapterStaList.num;
    for (int i = 0; i < adapterStaList.num; ++i) {
        outCounters.txBytes += adapterStaList.sta[i].tx_bytes;
        outCounters.rxBytes += adapterStaList.sta[i].rx_bytes;
    }
    return true;
}

// Вычисление средней скорости с экспоненциальным сглаживанием.
static void updateSpeedSamples() {
    TrafficCounters current{};
    uint16_t clients = 0;
    if (!fetchTrafficCounters(current, clients)) {
        return;
    }

    uint32_t now = millis();
    if (s_lastSampleTime != 0 && now > s_lastSampleTime) {
        uint64_t previousTotal = s_previousCounters.txBytes + s_previousCounters.rxBytes;
        uint64_t currentTotal = current.txBytes + current.rxBytes;
        uint64_t deltaBytes = (currentTotal >= previousTotal) ? (currentTotal - previousTotal) : 0;
        float deltaTimeSeconds = (now - s_lastSampleTime) / 1000.0f;
        float newSpeed = deltaTimeSeconds > 0.0f ? (deltaBytes * 8.0f) / (1000000.0f * deltaTimeSeconds) : 0.0f;
        s_avgSpeedMbps = 0.7f * s_avgSpeedMbps + 0.3f * newSpeed; // Экспоненциальное сглаживание
    } else if (s_lastSampleTime == 0) {
        s_avgSpeedMbps = 0.0f;
    }

    s_previousCounters = current;
    s_clientCount = clients;
    s_lastSampleTime = now;
}

// Отрисовка бегущих сегментов скорости "в стиле Windows".
static void drawSpeedAnimation(float speedMbps) {
    const int16_t barTop = SCREEN_HEIGHT - 16;
    const int16_t barHeight = 8;
    const int16_t barLeft = 8;
    const int16_t totalWidth = SCREEN_WIDTH - 2 * barLeft;
    const float segmentWidth = static_cast<float>(totalWidth) / SPEED_SEGMENTS;

    s_speedPhase = (s_speedPhase + 1) % (SPEED_SEGMENTS + ACTIVE_SEGMENTS);
    for (uint8_t i = 0; i < SPEED_SEGMENTS; ++i) {
        int16_t x0 = barLeft + static_cast<int16_t>(i * segmentWidth);
        int16_t x1 = barLeft + static_cast<int16_t>((i + 1) * segmentWidth) - 1;
        bool active = false;
        uint8_t start = s_speedPhase;
        uint8_t end = s_speedPhase + ACTIVE_SEGMENTS;
        if (end <= SPEED_SEGMENTS) {
            active = (i >= start && i < end);
        } else {
            uint8_t wrapEnd = end - SPEED_SEGMENTS;
            active = (i >= start) || (i < wrapEnd);
        }

        if (active) {
            display.fillRect(x0, barTop, x1 - x0 + 1, barHeight, SSD1306_WHITE);
        } else {
            display.drawRect(x0, barTop, x1 - x0 + 1, barHeight, SSD1306_WHITE);
        }
    }

    display.setCursor(0, barTop - 10);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.printf("Скорость: %5.2f Мбит/с", speedMbps);
}

// Обновление OLED в отдельном FreeRTOS-потоке, чтобы не мешать NAT.
static void displayTask(void *param) {
    (void)param;
    for (;;) {
        if (xSemaphoreTake(s_statsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            updateSpeedSamples();
            display.clearDisplay();
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);
            display.setCursor(0, 0);
            display.println(PROJECT_NAME);

            display.print("STA: ");
            display.println(STA_SSID);

            IPAddress apIp = WiFi.softAPIP();
            display.print("AP IP: ");
            display.println(apIp.toString());

            display.print("Клиенты: ");
            display.println(s_clientCount);

            drawSpeedAnimation(s_avgSpeedMbps);
            display.display();
            xSemaphoreGive(s_statsMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_UPDATE_INTERVAL_MS));
    }
}

// Настройка NAT-моста на базе esp_wifi_repeater_plus (включая ускорение).
static void configureNat() {
#if defined(CONFIG_LWIP_NAPT) || defined(LWIP_NAPT)
    esp_netif_t *apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (apNetif != nullptr) {
        esp_netif_ip_info_t ipInfo;
        memset(&ipInfo, 0, sizeof(ipInfo));
        if (esp_netif_get_ip_info(apNetif, &ipInfo) == ESP_OK) {
            ip_napt_enable(ipInfo.ip.addr, 1); // Включаем NAT для адреса точки доступа
        }
    }
#else
    Serial.println("NAPT не активирован в прошивке — убедитесь, что включен CONFIG_LWIP_NAPT");
#endif
    // Повышаем TX power до 20.5 dBm для максимальной мощности передачи
    esp_wifi_set_max_tx_power(82);

#ifdef ENABLE_HIGH_SPEED
    // Переводим интерфейсы в режим повышенной ширины канала
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
#endif
}

// Инициализация OLED с комментариями по работе дисплея.
static void initDisplay() {
    Wire.begin();
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
        Serial.println("Не удалось инициализировать SSD1306");
        for (;;) {
            delay(1000);
        }
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(PROJECT_NAME);
    display.println("Инициализация...");
    display.display();
}

// Инициализация Wi-Fi в режиме AP+STA с жестким каналом и повышенной мощностью.
static void initWifi() {
    WiFi.mode(WIFI_MODE_APSTA);
    esp_wifi_set_ps(WIFI_PS_NONE); // Отключаем энергосбережение ради стабильности

    wifi_config_t wifiConfig = {};
    strlcpy(reinterpret_cast<char *>(wifiConfig.sta.ssid), STA_SSID, sizeof(wifiConfig.sta.ssid));
    strlcpy(reinterpret_cast<char *>(wifiConfig.sta.password), STA_PASSWORD, sizeof(wifiConfig.sta.password));
    wifiConfig.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifiConfig.sta.pmf_cfg.capable = true;
    wifiConfig.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &wifiConfig);

    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE); // Принудительно задаем канал

    WiFi.begin();
    Serial.print("Подключение к ");
    Serial.println(STA_SSID);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
    }
    Serial.println();
    Serial.print("STA IP: ");
    Serial.println(WiFi.localIP());

    bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, WIFI_CHANNEL, false, 8);
    if (!apStarted) {
        Serial.println("Не удалось создать точку доступа");
    } else {
        Serial.print("AP IP: ");
        Serial.println(WiFi.softAPIP());
    }

    configureNat(); // Включаем NAT и High Speed режим
}

void setup() {
    Serial.begin(115200);
    delay(200);

    if (s_statsMutex == nullptr) {
        s_statsMutex = xSemaphoreCreateMutex();
    }

    initDisplay();

    // Комментарий: Порядок инициализации повторяет esp_wifi_repeater_plus — сначала сеть, затем NAT и дисплей.
    initWifi();

    // Создаем отдельную задачу для обновления OLED, чтобы освободить главный цикл для работы NAT.
    xTaskCreatePinnedToCore(displayTask, "display", 4096, nullptr, 1, &s_displayTaskHandle, 1);
}

void loop() {
    // Главный цикл может быть пустым — NAT и стек Wi-Fi работают в системных задачах.
    delay(1000);
}
