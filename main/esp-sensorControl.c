// esp-sensorControl.c — Refactored

#include <stdio.h>
#include <time.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_sntp.h"

#include "ethernet_init.h"
#include "mqtt_man.h"
#include "mqtt_client.h"
#include "ahtxx.h"
#include "firebase.h"

// === Defines ===
#define GOT_IP_BIT BIT0
#define STACK_SIZE 10240
#define SAMPLE_INTERVAL_MS 5000  // read sensor every 5 s
#define WINDOW_INTERVAL_MS 60000 // average window = 60 s
#define BATCH_SIZE 5

// === Logging Tags ===
static const char *TAG_ETH = "ETH";
static const char *TAG_NTP = "SNTP";
static const char *TAG_AHT = "AHTSensor";
static const char *TAG_POSTIP = "PostIPTask";
static const char *TAG = "Averaging";

// === Globals ===
static EventGroupHandle_t eth_event_group;
ahtxx_handle_t dev_hdl;
float temperature, humidity;
static float window_sum;
static uint32_t window_count;

/* Structure for holding an averaged reading */
typedef struct
{
    time_t timestamp;
    float average;
} avg_sample_t;

/* Simple in-RAM circular buffer for a batch of averages */
static avg_sample_t batch_buffer[BATCH_SIZE];
static uint8_t batch_index;

/* Forward declarations */
static void sample_timer_cb(TimerHandle_t xTimer);
static void window_timer_cb(TimerHandle_t xTimer);
static void send_batch(void);

/* Create two timers in app_main() or initialization routine: */
void setup_averaging(void)
{
    TimerHandle_t sample_timer = xTimerCreate(
        "sampleTimer", pdMS_TO_TICKS(SAMPLE_INTERVAL_MS),
        pdTRUE, NULL, sample_timer_cb);
    TimerHandle_t window_timer = xTimerCreate(
        "windowTimer", pdMS_TO_TICKS(WINDOW_INTERVAL_MS),
        pdTRUE, NULL, window_timer_cb);

    if (sample_timer == NULL || window_timer == NULL)
    {
        ESP_LOGE(TAG, "Timer creation failed");
        return;
    }
    xTimerStart(sample_timer, 0);
    xTimerStart(window_timer, 0);
}

/* ----------------------------------------------------------------------------
 * sample_timer_cb
 *   Runs every SAMPLE_INTERVAL_MS:
 *   - Reads AHT21
 *   - Accumulates into window_sum and window_count
 * ------------------------------------------------------------------------- */
static void sample_timer_cb(TimerHandle_t xTimer)
{
    esp_err_t err = ahtxx_get_measurement(dev_hdl, &temperature, &humidity);

    if (err == ESP_OK)
    {
        window_sum += (temperature * 9.0 / 5.0) + 32.0;
        window_count += 1;
        ESP_LOGD(TAG, "Sampled: %.2f  (sum=%.2f count=%" PRIu32 ")",
                 temperature, window_sum, window_count);
    }
    else
    {
        ESP_LOGW(TAG, "Sensor read failed: %s", esp_err_to_name(err));
    }
}

/* ----------------------------------------------------------------------------
 * window_timer_cb
 *   Runs every WINDOW_INTERVAL_MS:
 *   - Computes average
 *   - Resets sum/count
 *   - Appends to batch_buffer
 *   - If buffer full, calls send_batch()
 * ------------------------------------------------------------------------- */
static void window_timer_cb(TimerHandle_t xTimer)
{
    if (window_count == 0)
    {
        ESP_LOGW(TAG, "No samples in this window");
        return;
    }

    /* Compute average and reset */
    float avg = window_sum / (float)window_count;
    window_sum = 0;
    window_count = 0;

    /* Record timestamped average */
    time_t now = time(NULL);
    batch_buffer[batch_index].timestamp = now;
    batch_buffer[batch_index].average = avg;
    batch_index++;

    ESP_LOGI(TAG, "Window avg: %.2f at %lld  (buffer=%u/%u)",
             avg, (long long)now, batch_index, BATCH_SIZE);

    /* If we’ve collected enough, send them */
    if (batch_index >= BATCH_SIZE)
    {
        // send_batch();
        batch_index = 0;
    }
}

/* ----------------------------------------------------------------------------
 * send_batch
 *   Iterates buffered averages and sends each to Firestore
 *   (you could also build a single batched JSON payload if you prefer)
 * ------------------------------------------------------------------------- */
static void send_batch(void)
{
    ESP_LOGI(TAG, "Sending batch of %u samples", BATCH_SIZE);

    // for (uint8_t i = 0; i < BATCH_SIZE; i++) {
    //     avg_sample_t *s = &batch_buffer[i];

    //     /* Option A: modify your existing function to accept parameters */
    //     // send_sensor_data_to_firestore(s->timestamp, s->average);

    //     /* Option B: temporarily set a global or use a struct */
    //     set_fake_sensor_data(s->average);       // if your send fn reads from a global
    //     override_timestamp(s->timestamp);       // likewise for timestamp
    //     send_sensor_data_to_firestore();        // existing no-arg sender

    //     vTaskDelay(pdMS_TO_TICKS(100));         // small delay to avoid back-to-back HTTP
    // }
}

// === Function: Time Sync ===
void obtain_time(void)
{
    ESP_LOGI(TAG_NTP, "Initializing SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    // Switch to “smooth” mode to slewing‐adjust the clock
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);

    // Re‑sync every hour (3600s), in milliseconds
    esp_sntp_set_sync_interval(3600 * 1000);

    // setup callback to notify of time-sync
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);

    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int max_retries = 10;

    while (timeinfo.tm_year < (2022 - 1900) && ++retry < max_retries)
    {
        ESP_LOGI(TAG_NTP, "Waiting for system time to be set... (%d/%d)", retry, max_retries);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (retry >= max_retries)
    {
        ESP_LOGW(TAG_NTP, "System time was not set via SNTP");
    }
    else
    {
        ESP_LOGI(TAG_NTP, "System time set successfully");
    }
}

// === time sync callback ===
void time_sync_notification_cb(struct timeval *tv)
{
    time_t now = time(NULL);
    ESP_LOGI("SNTP_CB", "Time synchronized: %s", ctime(&now));
}

// === Task: Post-IP Setup ===
void post_ip_task(void *pvParameters)
{

    ESP_LOGI(TAG_POSTIP, "starting post-ip");
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG_POSTIP, "Sub task stack remaining: %u bytes", watermark);

    ESP_LOGI(TAG_POSTIP, "Waiting for IP...");
    xEventGroupWaitBits(eth_event_group, GOT_IP_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    obtain_time();

    // Initialize AHT21 sensor
    ESP_LOGI(TAG_AHT, "Initializing AHT21 sensor");
    ahtxx_config_t dev_cfg = I2C_AHT21_CONFIG_DEFAULT;
    i2c_master_bus_handle_t bus_handle;
    i2c_master_bus_config_t bus_cfg = {
        .sda_io_num = 1,
        .scl_io_num = 2,
        .i2c_port = I2C_NUM_0,
        .clk_source = I2C_CLK_SRC_DEFAULT};
    i2c_new_master_bus(&bus_cfg, &bus_handle);
    ahtxx_init(bus_handle, &dev_cfg, &dev_hdl);

    if (dev_hdl == NULL)
    {
        ESP_LOGE(TAG_AHT, "ahtxx handle init failed");
        assert(dev_hdl);
    }
    ESP_LOGI(TAG_POSTIP, "AHT21 initialized");
    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG_POSTIP, "Sub task stack remaining: %u bytes", watermark);

    // Read and log temperature/humidity
    esp_err_t err = ahtxx_get_measurement(dev_hdl, &temperature, &humidity);
    ESP_LOGI(TAG_AHT, "Temperature: %.2f C, Humidity: %.2f %%", temperature, humidity);
    // send_sensor_data_to_firestore(temperature, humidity);
    //  TODO Upload to Firebase
    //  firebase_upload_temperature(temperature, humidity);
    setup_averaging();
    vTaskDelete(NULL);
}

// === Ethernet Event Callback ===
void on_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG_ETH, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(eth_event_group, GOT_IP_BIT);
}

// === Initialize Ethernet ===
void ethernet_init()
{
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(ethernet_init_all(&eth_handles, &eth_port_cnt));
    eth_event_group = xEventGroupCreate();
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[0])));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth_handles[0]));
    eth_dev_info_t info = ethernet_init_get_dev_info(eth_handles[0]);
    ESP_LOGI(TAG_ETH, "Device Name: %s", info.name);
    ESP_LOGI(TAG_ETH, "Device type: ETH_DEV_TYPE_SPI(%d)", info.type);
    ESP_LOGI(TAG_ETH, "Pins: cs: %d, intr: %d", info.pin.eth_spi_cs, info.pin.eth_spi_int);
}

// === Main App Entry ===
void app_main(void)
{
    ESP_LOGI(TAG_ETH, "Starting app_main");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ethernet_init();

    // Start post-IP task
    xTaskCreate(&post_ip_task, "post_ip_task", STACK_SIZE, NULL, 5, NULL);
}
