#pragma once
#include "mqtt_client.h"

void mqtt_app_start(void);
esp_mqtt_client_handle_t mqtt_get_client(void);
