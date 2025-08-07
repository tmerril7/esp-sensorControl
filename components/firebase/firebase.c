/* components/firebase/firebase.c
 *
 * Firebase integration for ESP32-S3:
 *  - Obtain OAuth2 access token via JWT (RS256)
 *  - Send sensor data to Firestore via REST API
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <sys/param.h>

/* ESP-IDF headers */
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "nvs_flash.h"
#include "nvs.h"

/* mbedTLS for JWT signing */
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"

/* JSON handling */
#include "cJSON.h"

/* Project header */
#include "firebase.h"

/* My modules*/
#include "nvs_helper.h"
#include "usb_helper.h"
/*=============================================================================
 *                                CONSTANTS
 *============================================================================*/

/* OAuth2 token endpoint and scope */
#define TOKEN_URL "https://oauth2.googleapis.com/token"
#define SCOPE "https://www.googleapis.com/auth/datastore"
/* JWT expiration interval (seconds) */
#define EXPIRATION_SEC 3600
#define TOKEN_REFRESH_MARGIN 60

/* HTTP buffer sizes */
#define MAX_HTTP_OUTPUT_BUFFER 1024
#define SVC_ACCT_EMAIL_SIZE 75
#define PROJ_ID_SIZE 50

/*=============================================================================
 *                           EXTERNALLY EMBEDDED KEYS
 *============================================================================*/
/*=============================================================================
 *                             STATIC STATE & TAGS
 *============================================================================*/

static const char *TAG = "FIREBASE";
static char cached_token[1200] = {0};
static time_t cached_expiry = 0;
static const char *config_path = "/data/cfg.json";
#define COMMIT_URL_FMT "https://firestore.googleapis.com/v1/projects/%s/databases/(default)/documents:commit"
/*=============================================================================
 *                         FORWARD DECLARATIONS
 *============================================================================*/

/** @brief mbedTLS RNG callback for signing. */
static int _mbedtls_rng(void *ctx, unsigned char *buf, size_t len);

/** @brief RS256-sign `header.payload` and base64-encode the signature. */
static esp_err_t _sign_jwt_rs256(const char *header_payload, char *out_sig_b64, size_t sig_len);

/*=============================================================================
 *                           PRIVATE HELPER FUNCTIONS
 *============================================================================*/

void dump_hex16(const char *tag, const uint8_t *buf)
{
    printf("%s: ", tag);
    for (int i = 0; i < 64; i++)
    {
        printf("%02X ", buf[i]);
    }
    printf("\n");
}

static int _mbedtls_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

static esp_err_t _sign_jwt_rs256(const char *header_payload,
                                 char *out_sig_b64,
                                 size_t sig_len)
{
    // get firebase_key from config
    char *firebase_system_key = load_config_from_fat(config_path, "firebase_system_key");
    if (!firebase_system_key)
    {
        ESP_LOGE("CONFIG_HELPER", "Did not load firebase_system_key");
        return ESP_FAIL;
    }

    UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("firebase_stack", "Sub task stack remaining: %u bytes", watermark);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    /* Parse the service account private key */
    int ret = mbedtls_pk_parse_key(&pk, (const unsigned char *)firebase_system_key, strlen(firebase_system_key) + 1, NULL, 0, _mbedtls_rng, NULL);
    if (ret)
    {
        ESP_LOGE(TAG, "Private key parse failed: %d", ret);
        mbedtls_pk_free(&pk);
        return ESP_FAIL;
    }

    free(firebase_system_key);
    ESP_LOGI(TAG, "key is parsed");

    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("firebase_stack", "Sub task stack remaining: %u bytes", watermark);

    /* SHA256 hash of header.payload */
    unsigned char hash[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const unsigned char *)header_payload,
               strlen(header_payload),
               hash);
    ESP_LOGI(TAG, "hash is created");
    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("firebase_stack", "Sub task stack remaining: %u bytes", watermark);

/* Sign the hash */
#define LEN_SIG 512
    unsigned char *sig = malloc(LEN_SIG);
    size_t sig_actual = 0;
    ret = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256,
                          hash, sizeof(hash),
                          sig, LEN_SIG, &sig_actual,
                          _mbedtls_rng, NULL);
    mbedtls_pk_free(&pk);
    if (ret)
    {
        ESP_LOGE(TAG, "RSA sign failed: %d", ret);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "hash is signed");
    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("firebase_stack", "Sub task stack remaining: %u bytes", watermark);

    /* Base64-encode the signature */
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)out_sig_b64,
                          sig_len, &olen,
                          sig, sig_actual);
    out_sig_b64[olen] = '\0';

    ESP_LOGI(TAG, "signature is base64 encoded");
    free(sig);
    return ESP_OK;
}

/*=============================================================================
 *                           PUBLIC API FUNCTIONS
 *============================================================================*/

/**
 * @brief  Obtain (and cache until expiry) a Firebase OAuth2 access token.
 * @param  out_token   Buffer to receive the access token string.
 * @param  max_len     Maximum length of `out_token`.
 * @param  svc_acct_email  Service account email string.
 * @return ESP_OK on success, otherwise an ESP error.
 */
esp_err_t firebase_get_access_token(char *out_token, size_t max_len, char *svc_acct_email)
{
    ESP_LOGI(TAG, "Requesting access token...");

    /* Build JWT header and payload */
    time_t now = time(NULL);
    time_t exp = now + EXPIRATION_SEC;

    /* -----check for existing token----- */
    if (cached_token[0] != '\0' && now < (cached_expiry - TOKEN_REFRESH_MARGIN))
    {
        ESP_LOGI(TAG, "Reusing valid token, expires in %llds",
                 (long long)(cached_expiry - now));
        strlcpy(out_token, cached_token, max_len);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Token expired or missing (now=%lld, expiry=%lld), fetching new one",
             (long long)now, (long long)cached_expiry);

    /* 1) Base64(header) */
    const char hdr[] = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";
    char hdr_b64[64];
    size_t hdr_b64_len;
    mbedtls_base64_encode((unsigned char *)hdr_b64,
                          sizeof(hdr_b64), &hdr_b64_len,
                          (const unsigned char *)hdr, strlen(hdr));
    hdr_b64[hdr_b64_len] = '\0';

/* 2) Base64(payload) */
#define PAYLOAD_SIZE 512
    char *payload = malloc(PAYLOAD_SIZE);
    snprintf(payload, PAYLOAD_SIZE,
             "{\"iss\":\"%s\",\"scope\":\"%s\",\"aud\":\"%s\",\"iat\":%lld,\"exp\":%lld}",
             svc_acct_email, SCOPE, TOKEN_URL,
             (long long)now, (long long)exp);
    ESP_LOGI(TAG, "payload: %s", payload);
#define PAYLOAD_B64_SIZE 512
    char *payload_b64 = malloc(PAYLOAD_B64_SIZE);
    size_t payload_b64_len;
    mbedtls_base64_encode((unsigned char *)payload_b64,
                          PAYLOAD_B64_SIZE, &payload_b64_len,
                          (const unsigned char *)payload, strlen(payload));
    payload_b64[payload_b64_len] = '\0';
    ESP_LOGI(TAG, "%s", payload_b64);
    free(payload);

/* 3) Sign header.payload */
#define HEADER_PAYLOAD_SIZE 1024
    char *header_payload = malloc(HEADER_PAYLOAD_SIZE);
    snprintf(header_payload, HEADER_PAYLOAD_SIZE, "%s.%s", hdr_b64, payload_b64);
    ESP_LOGI(TAG, "%s", header_payload);
    free(payload_b64);

#define SIG_B64_SIZE 1024
    char *sig_b64 = malloc(SIG_B64_SIZE);
    _sign_jwt_rs256(header_payload, sig_b64, SIG_B64_SIZE);

/* 4) Complete JWT */
#define JWT_SIZE 1024
    char *jwt = malloc(JWT_SIZE);
    snprintf(jwt, JWT_SIZE, "%s.%s", header_payload, sig_b64);
    free(header_payload);
    free(sig_b64);
    ESP_LOGI(TAG, "%s", jwt);

    /* 5) Prepare OAuth2 POST body */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "grant_type",
                            "urn:ietf:params:oauth:grant-type:jwt-bearer");
    cJSON_AddStringToObject(root, "assertion", jwt);
    free(jwt);
#define POST_DATA_SIZE 1024
    char *post_data = malloc(POST_DATA_SIZE);
    snprintf(post_data, POST_DATA_SIZE, "%s", cJSON_PrintUnformatted(root));
    ESP_LOGI(TAG, "Post Data: %s", post_data);
    cJSON_Delete(root);

    /* 6) Perform HTTP POST */
    // get firebase_key from config
    char *firebase_cert = load_config_from_fat(config_path, "G_ROOT_CA_CERT");
    if (!firebase_cert)
    {
        ESP_LOGE("CONFIG_HELPER", "Did not load firebase_cert");
        return ESP_FAIL;
    }
    esp_http_client_config_t config = {
        .url = TOKEN_URL,
        .timeout_ms = 5000,
        .cert_pem = firebase_cert,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    esp_err_t err = esp_http_client_open(client, strlen(post_data));

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int wlen = esp_http_client_write(client, post_data, strlen(post_data));
    free(post_data);
    if (wlen < 0)
    {
        ESP_LOGE(TAG, "Write failed");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int resp_headers_len = esp_http_client_fetch_headers(client);
    if (resp_headers_len < 0)
    {
        ESP_LOGE(TAG, "HTTP client fetch headers failed");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
#define RESPONSE_BUFFER_SIZE 2048
    char *response_buffer = malloc(RESPONSE_BUFFER_SIZE + 1);
    int response_content_len = esp_http_client_read_response(client, response_buffer, RESPONSE_BUFFER_SIZE);
    if (response_content_len < 0)
    {
        ESP_LOGE(TAG, "No response from HTTP request");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    response_buffer[response_content_len] = '\0';

    ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %" PRId64,
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));
    ESP_LOGI(TAG, "%s", response_buffer);

    free(firebase_cert);

    /* 7) Parse JSON response */
    cJSON *resp_json = cJSON_Parse(response_buffer);
    free(response_buffer);
    if (!resp_json)
    {
        ESP_LOGE(TAG, "Failed to parse token JSON");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    cJSON *token_item_token = cJSON_GetObjectItem(resp_json, "access_token");
    cJSON *token_item_expires_in = cJSON_GetObjectItem(resp_json, "expires_in");
    if (!cJSON_IsString(token_item_token) || !cJSON_IsNumber(token_item_expires_in))
    {
        ESP_LOGE(TAG, "Unexpected JSON format");
        cJSON_Delete(resp_json);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    strlcpy(cached_token, token_item_token->valuestring, max_len);
    strlcpy(out_token, cached_token, max_len);
    cached_expiry = now + (time_t)token_item_expires_in->valuedouble;

    cJSON_Delete(resp_json); // only delete the root
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Access token obtained successfully");
    return ESP_OK;
}

bool get_json_string(const cJSON *root, const char *key, char *out_buf, size_t buf_len)
{
    // 1) Find the item
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item)
    {
        // key not found
        return false;
    }

    // 2) Verify it’s a string
    if (!cJSON_IsString(item) || (item->valuestring == NULL))
    {
        // wrong type
        return false;
    }

    // 3) Copy safely into your buffer
    // Option A: snprintf (always null‑terminates)
    snprintf(out_buf, buf_len, "%s", item->valuestring);

    return true;
}

/**
 * @brief  Send a batch of readings to Firestore under `sensor_data` collection.
 */
esp_err_t send_sensor_data_to_firestore(const char *doc)
{

    // extract svc_acct_email from nvs_config
    char *svc_acct_email = load_config_from_fat(config_path, "svc_acct_email");
    if (!svc_acct_email)
    {
        ESP_LOGE("CONFIG_HELPER", "Did not load svc_acct_email");
        return ESP_FAIL;
    }

    // char svc_acct_email[SVC_ACCT_EMAIL_SIZE];
    // if (!load_config_str("svs_acct_email", svc_acct_email, SVC_ACCT_EMAIL_SIZE - 1, "default-config"))
    // {
    //     ESP_LOGE(TAG, "Failed to get svc_acct_email");
    //     return;
    // }
    ESP_LOGI(TAG, "loaded svc_acct_email");

    // extract proj_id from nvs_config
    char *proj_id = load_config_from_fat(config_path, "proj_id");
    if (!proj_id)
    {
        ESP_LOGE("CONFIG_HELPER", "Did not load proj_id");
        return ESP_FAIL;
    }

    // char proj_id[PROJ_ID_SIZE];
    // if (!load_config_str("proj_id", proj_id, PROJ_ID_SIZE - 1, "default-config"))
    // {
    //     ESP_LOGE(TAG, "Failed to get project_id");
    //     return;
    // }
    ESP_LOGI(TAG, "extracted proj_id");

// get access token
#define TOKEN_SIZE 1200
    char *token = malloc(TOKEN_SIZE);
    if (firebase_get_access_token(token, TOKEN_SIZE - 1, svc_acct_email) != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot obtain access token, aborting send");
        return ESP_FAIL;
    }
    free(svc_acct_email);

    char *auth_header = malloc(TOKEN_SIZE - 20);
    snprintf(auth_header, TOKEN_SIZE - 20 - 1, "Bearer %s", token);
    free(token);

    /* Build Firestore REST endpoint URL */
    char url[256];
    snprintf(url, sizeof(url), COMMIT_URL_FMT, proj_id);
    free(proj_id);

    char *firebase_cert = load_config_from_fat(config_path, "G_ROOT_CA_CERT");
    if (!firebase_cert)
    {
        ESP_LOGE("CONFIG_HELPER", "Did not load firebase_cert");
        return ESP_FAIL;
    }

    /* Perform HTTP POST */
    esp_http_client_config_t config = {
        .url = url,
        .cert_pem = firebase_cert,
        .timeout_ms = 5000,
        .buffer_size_tx = 2048,
        .buffer_size = 2048};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    ESP_LOGI(TAG, "Set first header");

    if (esp_http_client_set_header(client, "Authorization", auth_header) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set first header");
    }
    ESP_LOGI(TAG, "Set second header");

    if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set second header");
    }
    // ***Delete? esp_http_client_set_post_field(client, json_str, strlen(json_str));

    esp_err_t err = esp_http_client_open(client, strlen(doc));

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    int wlen = esp_http_client_write(client, doc, strlen(doc));
    if (wlen <= 0)
    {
        ESP_LOGE(TAG, "Write failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Wrote %u bytes", wlen);

    int resp_header_len = esp_http_client_fetch_headers(client);
    if (resp_header_len < 0)
    {
        ESP_LOGE(TAG, "HTTP client fetch headers failed");
        // return;
    }
    ESP_LOGI(TAG, "%u bytes in response headers", resp_header_len);

#define FIRESTORE_RESPONSE_BUFFER_SIZE 2048
    char *firestore_resp_buffer = malloc(FIRESTORE_RESPONSE_BUFFER_SIZE);
    int data_read = esp_http_client_read_response(client, firestore_resp_buffer, FIRESTORE_RESPONSE_BUFFER_SIZE - 1);
    if (data_read <= 0)
    {
        ESP_LOGE(TAG, "Failed to read http request response");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "read %u bytes", data_read);
    ESP_LOGI(TAG, "Status Code: %u", esp_http_client_get_status_code(client));
    esp_http_client_cleanup(client);

    free(auth_header);

    free(firestore_resp_buffer);
    free(firebase_cert);
    return ESP_OK;
}
