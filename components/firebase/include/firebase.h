#pragma once
#include "freertos/FreeRTOS.h"

esp_err_t firebase_get_access_token(char *out_token, size_t max_len, char *svc_acct_email);
void send_sensor_data_to_firestore(float temp, float hum);
