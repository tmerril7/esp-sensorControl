#pragma once
#include "mqtt_client.h"

#define BROKER_URL_SIZE 64
#define MQTT_PASSWORD_SIZE 21
#define MQTT_USERNAME_SIZE 21
#define MQTT_CERT_SIZE 2049

esp_err_t mqtt_app_start(char *broker_uri, char *mqtt_username, char *mqtt_password, char *verification_cert);
esp_mqtt_client_handle_t mqtt_get_client(void);
