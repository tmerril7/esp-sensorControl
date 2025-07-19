
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "sdkconfig.h"
#include "ethernet_init.h"
#include "esp_event.h"
#include "mqtt_man.h"
#include "mqtt_client.h"
#include "ahtxx.h"
#include "firebase.h"
#include "esp_sntp.h"
#include <time.h>

#define GOT_IP_BIT BIT0
#define STACK_SIZE 10240
static const char *TAGE = "Ethernet";
static const char *TAGPIPT = "post-ip-task";
static const char *TAGaht = "AHTSensor";
static const char *TAG_TIME = "sntp";
float temperature, humidity;
ahtxx_handle_t dev_hdl;

static EventGroupHandle_t eth_event_group;

void obtain_time(void)
{
    ESP_LOGI(TAG_TIME, "Initializing SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int max_retries = 10;
    while (timeinfo.tm_year < (2020 - 1900) && ++retry < max_retries)
    {
        ESP_LOGI(TAG_TIME, "Waiting for system time to be set... (%d)", retry);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (retry == max_retries)
    {
        ESP_LOGE(TAG_TIME, "Failed to set time after retries.");
    }
    else
    {
        ESP_LOGI(TAG_TIME, "Time is set: %s", asctime(&timeinfo));
    }
}

void start_ahtxx(void)
{
    ahtxx_config_t dev_cfg = I2C_AHT21_CONFIG_DEFAULT;

    i2c_master_bus_handle_t bus_handle;
    i2c_master_bus_config_t bus_cfg = {
        .sda_io_num = 1,
        .scl_io_num = 2,
        .i2c_port = I2C_NUM_0,
        .clk_source = I2C_CLK_SRC_DEFAULT

    };
    i2c_new_master_bus(&bus_cfg, &bus_handle);
    ahtxx_init(bus_handle, &dev_cfg, &dev_hdl);
    if (dev_hdl == NULL)
    {
        ESP_LOGE(TAGaht, "ahtxx handle init failed");
        assert(dev_hdl);
    }
}

void poll_temp_task(void *pvParameters)
{
    while (1)
    {
        esp_err_t result = ahtxx_get_measurement(dev_hdl, &temperature, &humidity);
        if (result != ESP_OK)
        {
            ESP_LOGE(TAGaht, "ahtxx device read failed (%s)", esp_err_to_name(result));
        }
        else
        {
            ESP_LOGI(TAGaht, "air temperature:     %.1f °F", temperature * 9.0 / 5.0 + 32);
            ESP_LOGI(TAGaht, "relative humidity:   %.1f %s", humidity, "%");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP)
    {
        xEventGroupSetBits(eth_event_group, GOT_IP_BIT);
    }
    ESP_LOGI(TAGE, "Ethernet Got IP Address");
    ESP_LOGI(TAGE, "~~~~~~~~~~~");
    ESP_LOGI(TAGE, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAGE, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAGE, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAGE, "~~~~~~~~~~~");
}

void post_ip_task(void *pvParameters)
{
    ESP_LOGI(TAGPIPT, "running post ip task");

    ESP_LOGI(TAGPIPT, "Task rar...");
    vTaskDelete(NULL);
}

void my_main_task(void *pv)
{
    while (true)
    {
        send_sensor_data_to_firestore();
        vTaskDelay(pdMS_TO_TICKS(20000));
    }
}
void app_main(void)
{
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("STACK", "Main task stack remaining: %u bytes", watermark);

    // start_ahtxx();
    // xTaskCreate(&poll_temp_task, "poll_temp_task", 4096, NULL, 5, NULL);
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(ethernet_init_all(&eth_handles, &eth_port_cnt));

    eth_event_group = xEventGroupCreate();

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    // Attach Ethernet driver to TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[0])));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth_handles[0]));
    eth_dev_info_t info = ethernet_init_get_dev_info(eth_handles[0]);
    ESP_LOGI(TAGE, "Device Name: %s", info.name);
    ESP_LOGI(TAGE, "Device type: ETH_DEV_TYPE_SPI(%d)", info.type);
    ESP_LOGI(TAGE, "Pins: cs: %d, intr: %d", info.pin.eth_spi_cs, info.pin.eth_spi_int);

    ESP_LOGI(TAGPIPT, "Waiting for IP...");
    xEventGroupWaitBits(eth_event_group, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAGPIPT, "IP acquired. Starting post-IP tasks.");
    obtain_time();
    // mqtt_app_start();
    //  vTaskDelay(pdMS_TO_TICKS(1000));
    // esp_mqtt_client_handle_t client = mqtt_get_client();
    // msg_id = esp_mqtt_client_publish(client, "topic/thebigtest", "HELLO", 0, 0, 0);
    //  Start task(s)
    // xTaskCreate(&post_ip_task, "post_ip_task", 4096, NULL, 5, NULL);
    xTaskCreate(my_main_task, "main_task", STACK_SIZE, NULL, 5, NULL);
    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("STACK", "Main task stack remaining: %u bytes", watermark);
}
