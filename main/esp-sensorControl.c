// esp-sensorControl.c — Refactored

#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

// === Logging Tags ===
static const char *TAG_ETH = "ETH";
static const char *TAG_NTP = "SNTP";
static const char *TAG_AHT = "AHTSensor";
static const char *TAG_POSTIP = "PostIPTask";

// === Globals ===
static EventGroupHandle_t eth_event_group;
ahtxx_handle_t dev_hdl;
float temperature, humidity;

// === Function: Time Sync ===
void obtain_time(void)
{
    ESP_LOGI(TAG_NTP, "Initializing SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

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

// === Task: Post-IP Setup ===
void post_ip_task(void *pvParameters)
{
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

    // Read and log temperature/humidity
    esp_err_t err = ahtxx_get_measurement(dev_hdl, &temperature, &humidity);
    ESP_LOGI(TAG_AHT, "Temperature: %.2f C, Humidity: %.2f %%", temperature, humidity);

    // TODO Upload to Firebase
    // firebase_upload_temperature(temperature, humidity);

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
