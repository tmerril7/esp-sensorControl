#ifndef CONFIG_NVS_H
#define CONFIG_NVS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * @brief Initialize NVS. Must be called once at application startup.
 *        If no free pages or version mismatch, it erases and re‑inits.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t init_nvs(void);

/**
 * @brief Save a 32‑bit integer value under the given key.
 * @param key    Null‑terminated NVS key name.
 * @param value  Integer value to store.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t save_config_int(const char *key, int32_t value);

/**
 * @brief Load a 32‑bit integer value from NVS.
 * @param key             Null‑terminated NVS key name.
 * @param default_value   Returned if the key is not found or on error.
 * @return The stored value, or default_value if missing/error.
 */
int32_t load_config_int(const char *key, int32_t default_value);

/**
 * @brief Save a null‑terminated string under the given key.
 * @param key    Null‑terminated NVS key name.
 * @param value  Null‑terminated string to store.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t save_config_str(const char *key, const char *value);

/**
 * @brief Load a string value from NVS.
 * @param key           Null‑terminated NVS key name.
 * @param out_buf       Buffer to receive the string.
 * @param buf_len       Length of out_buf in bytes.
 * @param default_str   Fallback string if key not found or too large.
 * @return true if the stored string was loaded successfully;
 *         false if default_str was used (missing/key too large/error).
 */
bool load_config_str(const char *key,
                     char *out_buf,
                     size_t buf_len,
                     const char *default_str);

#endif // CONFIG_NVS_H