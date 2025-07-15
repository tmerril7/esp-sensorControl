#include <stdio.h>
#include "firebase.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include "esp_tls.h"
#include <sys/param.h>
#define TOKEN_URL "https://oauth2.googleapis.com/token"
#define SCOPE "https://www.googleapis.com/auth/datastore"
#define EXPIRATION_SEC 3600
#define SERVICE_ACCOUNT_EMAIL "sensorsvc@sensor-control-835f2.iam.gserviceaccount.com"
#define FIREBASE_PROJECT_ID "sensor-control-835f2"
#define MAX_HTTP_OUTPUT_BUFFER 2048
#define RESPONSE_BUF_SIZE 2048

static const char PRIVATE_KEY_PEM[] = "-----BEGIN PRIVATE KEY-----\nMIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQDiwdQH98/pIllt\ncTyMH0JfxzuthPGp9rvtyZIKsVerej7SwrnTv5O5q66qiqbPRyklDrQCoObkiX01\nSXeLaE5Cw53aby5neq0D97QJtnIZLE/9+Czpo9urlEY/MF5ejeZ37bMEIVrqCeTM\nUaBW8pYN9dwLRccTdjNO6ta9euYY0JoU2VpAJMYX2ucmrpWT+vxeG9nsvhfsq3Jo\n46N2Y8ftlUctstqlBSjmOI8Y1QNqs84MoXrC54QJ2zPKGdtbP6aZWL0yf9/ML2p1\nUDUq9JLDLwicBr8OWzn0Nu3v7HZ/rGi7obfxT7kxhhRKojGC6kxPI8UDmmWfK/Or\nFKAtUt87AgMBAAECggEAIG8VAubBSFlvbSYLQQefmM+Ii7M+Vc9C5io0x27CWXas\n0bykk9MNMDuMSjx2y6MkEXbe7JlTLVE1JZASl1AeEZKlW38Xphl38d9WUyVTgKJD\n9tGuquSTISGeQp+Kf//P/Ut0lZynwl4T6d2rD4S3Tdvi04HxjakUga85c/TGQLog\ntEqjZb0u1mRu/J4uOBsAD4tymK2CKRaB5oz51UznoT3ZBCp9yvLmArGje7gE/XVi\n7uUzsGG1rnPZqy0KYrgUk5BGAwiMC8f+NQx1lHHCpoQJlVEUenmRntg0kKJz1g6C\nFbI1Al6YnBennZ3N7Txe7Pl29mbT8/muQ9Lk+6m6wQKBgQD7B33j6MaC98wqy5wj\ncsGGkZHaAJFS3NDJrussYEQH6EOpzxOSWJ7pBkCOCWtw1BuPDro+MJHoepktE2nG\nBhqOiD7du/wu7xRbcy53Ffe3UEjXgM8FO6MVEUYxPx2jl0tF7uqE6DPX1hBQMMvx\nVRdwiZFjvO0+u64isT1UbLqOEQKBgQDnP0wRcwtVpxiVjCSOeCUVKa2bh3RMSF3h\nxkpiKOSSduLlB5KcpUzvRKQ2qL3wmiJ5+QnZZQRVJqzNqcdA+tjBc1Z1B3VZ5qm5\nZEOk0sCxDouFdLhqCWRNEyyBrdSnvqQYXKEPMI4B8jOhVBOntWFa/2cfkqhH90YL\nKKwt6j/8iwKBgBIQW/LGkWJjSoHZ3QZ//4UbfI9fcxWvZibdO3caBks7X4Mcr5/c\nWLMUDBksfFrGKKWGvcgz3owIJnWj6/yf+9E95Kg4GtGVyrU5+KIBJq4+TL+VOVB7\nFiUx9QceL5fSD0ydAKtHulNRyCK9IC/hm6oxfBDdS4U0JDfC4VLt8A7BAoGADzGX\ne+YFLqGF+f11QW8fcJ6Ga5ugxopSsMzogj6RlhX9nnK67VPnFCl8aKL5p99YuI7m\nWMRMXpPl8rVfBCP3Le64FYRh74A6UpYF48R9KKT+AczDeSQY1P+XgwW63TKncXpU\nkrzr97DEN6tghzphpSr2yZpHOENE9OK9PV8QyTUCgYBTMRhKiHoDcaee8xE/WoL9\nVAqcfnsTBTw+LMzsMobEYqvqbfybqvg8UEARcChp81YTuIAzsPhri98XDJzGM1pR\ntAc8wyQ0Iqj3ueDREhh1SGiaZqMiwGoKyq+J1IiVUQP6vNKh3V2vKlByblSEjS5e\nvXOV3bbeaS+kkfkF17l7XQ==\n-----END PRIVATE KEY-----\n";
static const char GOOGLE_ROOT_CA_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
    "-----END CERTIFICATE-----\n";

static const char *TAG = "FIREBASE_AUTH";
float read_fake_sensor_data()
{
    return (rand() % 1000) / 10.0; // e.g., 0.0 to 99.9
}
int my_rng(void *ctx, unsigned char *output, size_t len)
{
    (void)ctx;
    esp_fill_random(output, len);
    return 0;
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static const char *TAG = "HTTP_EVENT";
    static char *output_buffer; // Buffer to store response of http request from event handler
    static int output_len;      // Stores number of bytes read
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        // Clean the buffer in case of a new request
        if (output_len == 0 && evt->user_data)
        {
            // we are just starting to copy the output data into the use
            memset(evt->user_data, 0, MAX_HTTP_OUTPUT_BUFFER);
        }
        /*
         *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
         *  However, event handler can also be used in case chunked encoding is used.
         */
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            // If user_data buffer is configured, copy the response into the buffer
            int copy_len = 0;
            if (evt->user_data)
            {
                // The last byte in evt->user_data is kept for the NULL character in case of out-of-bound access.
                copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - output_len));
                if (copy_len)
                {
                    memcpy(evt->user_data + output_len, evt->data, copy_len);
                }
            }
            else
            {
                int content_len = esp_http_client_get_content_length(evt->client);
                if (output_buffer == NULL)
                {
                    // We initialize output_buffer with 0 because it is used by strlen() and similar functions therefore should be null terminated.
                    output_buffer = (char *)calloc(content_len + 1, sizeof(char));
                    output_len = 0;
                    if (output_buffer == NULL)
                    {
                        ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                        return ESP_FAIL;
                    }
                }
                copy_len = MIN(evt->data_len, (content_len - output_len));
                if (copy_len)
                {
                    memcpy(output_buffer + output_len, evt->data, copy_len);
                }
            }
            output_len += copy_len;
        }

        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        if (output_buffer != NULL)
        {
#if CONFIG_EXAMPLE_ENABLE_RESPONSE_BUFFER_DUMP
            ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
#endif
            free(output_buffer);
            output_buffer = NULL;
        }
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
        if (err != 0)
        {
            ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
        }
        if (output_buffer != NULL)
        {
            free(output_buffer);
            output_buffer = NULL;
        }
        output_len = 0;
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        esp_http_client_set_header(evt->client, "From", "user@example.com");
        esp_http_client_set_header(evt->client, "Accept", "text/html");
        esp_http_client_set_redirection(evt->client);
        break;
    }
    return ESP_OK;
}
static esp_err_t sign_jwt_rs256(const char *header_payload, char *out_sig_base64, size_t sig_len)
{

    int ret;
    size_t sig_actual = 0;
    size_t len_sig = 512;
    unsigned char *sig = malloc(len_sig);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    mbedtls_pk_parse_key(&pk, (const uint8_t *)PRIVATE_KEY_PEM, strlen(PRIVATE_KEY_PEM) + 1, NULL, 0, my_rng, NULL);

    unsigned char hash[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const unsigned char *)header_payload, strlen(header_payload), hash);

    ret = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, len_sig, &sig_actual, my_rng, NULL);
    if (ret != 0)
    {
        ESP_LOGE(TAG, "RSA Sign failed: %d", ret);
        return ESP_FAIL;
    }

    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)out_sig_base64, sig_len, &olen, sig, sig_actual);
    free(sig);
    out_sig_base64[olen] = '\0';

    mbedtls_pk_free(&pk);
    return ESP_OK;
}

esp_err_t firebase_get_access_token(char *out_token, size_t max_len)
{

    ESP_LOGI(TAG, "*********************Getting access token*******************");
    // char jwt[320];
    size_t len_jwt = 1024;
    char *jwt = malloc(len_jwt);
    size_t len_signed_b64 = 1024;
    char *signed_b64 = malloc(len_signed_b64);

    time_t now = time(NULL);
    ESP_LOGI(TAG, "time: %llu", now);
    time_t exp = now + EXPIRATION_SEC;

    // Create JWT Header and Payload
    char header[] = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";
    size_t len_payload = 512;
    char *payload = malloc(len_payload);
    snprintf(payload, len_payload,
             "{\"iss\":\"%s\",\"scope\":\"%s\",\"aud\":\"%s\",\"iat\":%lld,\"exp\":%lld}",
             SERVICE_ACCOUNT_EMAIL, SCOPE, TOKEN_URL, now, exp);
    ESP_LOGI(TAG, "%s", payload);
    // Base64 encode header and payload
    char b64_header[64], b64_payload[512];
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)b64_header, sizeof(b64_header), &olen, (unsigned char *)header, strlen(header));
    mbedtls_base64_encode((unsigned char *)b64_payload, sizeof(b64_payload), &olen, (unsigned char *)payload, strlen(payload));

    ESP_LOGI(TAG, "*********************Base64 encoding*******************");
    ESP_LOGI(TAG, "header: %s", header);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "b64 header: %s", b64_header);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "payload: %s", payload);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "b64 payload: %s", b64_payload);
    vTaskDelay(pdMS_TO_TICKS(5000));
    // Combine and sign
    free(payload);
    snprintf(jwt, len_jwt, "%s.%s", b64_header, b64_payload);
    ESP_ERROR_CHECK(sign_jwt_rs256(jwt, signed_b64, len_signed_b64));

    size_t len_jwt_full = 900;
    char *jwt_full = malloc(len_jwt_full);
    snprintf(jwt_full, len_jwt_full, "%s.%s", jwt, signed_b64);
    ESP_LOGI(TAG, "jwt_full: %s", jwt_full);
    free(jwt);
    free(signed_b64);
    // Build HTTP POST
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer");
    cJSON_AddStringToObject(root, "assertion", jwt_full);
    char *post_data = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "post len: %u, post data: %s", strlen(post_data), post_data);
    free(jwt_full);
    // ESP_LOGI(TAG, "google cert: %s", GOOGLE_ROOT_CA_PEM);
    // ESP_LOGI(TAG, "my key: %s", PRIVATE_KEY_PEM);
    char response_buf[RESPONSE_BUF_SIZE] = {0};
    esp_http_client_config_t config = {
        .event_handler = _http_event_handler,
        .url = TOKEN_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 6000,
        .cert_pem = GOOGLE_ROOT_CA_PEM,
        .user_data = response_buf};

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        int status = esp_http_client_get_status_code(client);
        int content_len = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "Status = %d, content_length = %d", status, content_len);
        ESP_LOGI(TAG, "first request was OK!");
        ESP_LOGI(TAG, "Response: %s", response_buf);
        // int len = esp_http_client_read_response(client, response_buf, sizeof(buffer) - 1);
        // buffer[len] = '\0';
        // ESP_LOGI(TAG, "length of response: %u, full response: %s", len, buffer);
        cJSON *resp = cJSON_Parse(response_buf);
        if (resp)
        {
            const cJSON *token = cJSON_GetObjectItem(resp, "access_token");
            if (token)
            {
                strncpy(out_token, token->valuestring, max_len - 1);
                out_token[max_len - 1] = '\0';
                cJSON_Delete(resp);
                ESP_LOGI(TAG, "Access token: %s", out_token);
                esp_http_client_cleanup(client);
                free(post_data);
                return ESP_OK;
            }
            else
            {
                ESP_LOGE(TAG, "No access_token in response!");
                cJSON_Delete(resp);
            }
        }
        else
        {
            ESP_LOGE(TAG, "Failed to parse JSON response");
        }

        cJSON_Delete(resp);
    }
    else
    {
        ESP_LOGE(TAG, "Token request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(post_data);
    return ESP_FAIL;
}

void send_sensor_data_to_firestore()
{
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("STACK_2", "Sub task stack remaining: %u bytes", watermark);
    char url[256];
    snprintf(url, sizeof(url),
             "https://firestore.googleapis.com/v1/projects/%s/databases/(default)/documents/sensor_data",
             FIREBASE_PROJECT_ID);

    float value = read_fake_sensor_data();

    cJSON *root = cJSON_CreateObject();
    cJSON *fields = cJSON_CreateObject();
    cJSON *temp = cJSON_CreateObject();

    cJSON_AddStringToObject(temp, "doubleValue", cJSON_PrintUnformatted(cJSON_CreateNumber(value)));
    cJSON_AddItemToObject(fields, "temperature", temp);
    cJSON_AddItemToObject(root, "fields", fields);

    char *json_str = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "POST JSON: %s", json_str);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .cert_pem = GOOGLE_ROOT_CA_PEM};

    esp_http_client_handle_t client = esp_http_client_init(&config);
    char access_token[512];
    char auth_header[1024];

    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("STACK_2", "Sub task stack remaining: %u bytes", watermark);
    if (firebase_get_access_token(access_token, sizeof(access_token)) == ESP_OK)
    {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", access_token);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Data sent to Firestore. HTTP Status = %d",
                 esp_http_client_get_status_code(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    cJSON_Delete(root);
    free(json_str);
}
