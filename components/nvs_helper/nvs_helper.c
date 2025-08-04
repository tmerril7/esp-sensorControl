// nvs_helper.c
// Helper routines for storing and retrieving configuration parameters in ESP-IDF NVS.
// Provides typed access for integers and strings.
//
// Author: Your Name
// Date: 2025‑07‑22

#include "nvs_helper.h"

#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"

#include "esp_vfs_dev.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "argtable3/argtable3.h"

static const char *TAG = "NVS_HELPER";

/**
 * @brief Initialize the NVS flash partition.
 *
 * This function must be called once at application startup before any
 * other NVS operations. If no free pages remain or a version change
 * is detected, it erases and reinitializes the NVS partition.
 *
 * @return
 *   - ESP_OK on success
 *   - Otherwise, an error code (logged via ESP_ERROR_CHECK).
 */
esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS partition invalid, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS initialized");
    return err;
}

/**
 * @brief Store a 32-bit integer value in NVS.
 *
 * Opens the "storage" namespace in read-write mode, writes the value,
 * commits the change, then closes the handle.
 *
 * @param key    Null-terminated key under which to store the value.
 * @param value  The 32-bit integer to store.
 * @return
 *   - ESP_OK on success
 *   - Otherwise, an esp_err_t error code.
 */
esp_err_t save_config_int(const char *key, int32_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_i32(handle, key, value);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Saved int '%s' = %" PRIu32, key, value);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to save int '%s': %s", key, esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief Load a 32-bit integer value from NVS.
 *
 * If the key does not exist or an error occurs, returns the provided default.
 *
 * @param key             Null-terminated key to read.
 * @param default_value   Value to return if key is missing or on error.
 * @return                Stored value or default_value on error.
 */
int32_t load_config_int(const char *key, int32_t default_value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "nvs_open failed, using default %" PRIu32, default_value);
        return default_value;
    }

    int32_t value = default_value;
    err = nvs_get_i32(handle, key, &value);
    nvs_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Loaded int '%s' = %" PRIu32, key, value);
        return value;
    }
    else
    {
        ESP_LOGW(TAG, "Key '%s' not set, using default %" PRIu32, key, default_value);
        return default_value;
    }
}

/**
 * @brief Store a null-terminated string in NVS.
 *
 * Opens the "storage" namespace in read-write mode, writes the string,
 * commits the change, then closes the handle.
 *
 * @param key    Null-terminated key under which to store the string.
 * @param value  Null-terminated string to store.
 * @return
 *   - ESP_OK on success
 *   - Otherwise, an esp_err_t error code.
 */
esp_err_t save_config_str(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Saved str '%s' = \"%s\"", key, value);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to save str '%s': %s", key, esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief Load a string value from NVS into a buffer.
 *
 * If the key does not exist, or the stored string is larger than buf_len,
 * the default_str is copied into out_buf instead.
 *
 * @param key           Null-terminated key to read.
 * @param out_buf       Buffer to receive the string.
 * @param buf_len       Length of out_buf in bytes.
 * @param default_str   Fallback string if key is missing or too large.
 * @return              true if the stored string was loaded; false if
 *                      default_str was used (missing/too large/error).
 */
bool load_config_str(const char *key, char *out_buf, size_t buf_len, const char *default_str)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "nvs_open failed (%s), using default \"%s\"", key, default_str);
        strlcpy(out_buf, default_str, buf_len);
        return false;
    }

    // Get required length
    size_t required = 0;
    err = nvs_get_str(handle, key, NULL, &required);

    if (err != ESP_OK || required == 0 || required > buf_len)
    {
        ESP_LOGW(TAG, "Key '%s' missing or too large, using default \"%s\"", key, default_str);
        strlcpy(out_buf, default_str, buf_len);
        nvs_close(handle);
        return false;
    }

    // Read string into buffer
    err = nvs_get_str(handle, key, out_buf, &required);
    nvs_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Loaded str '%s' = \"%s\"", key, out_buf);
        return true;
    }
    else
    {
        ESP_LOGW(TAG, "Failed to read '%s', using default \"%s\"", key, default_str);
        strlcpy(out_buf, default_str, buf_len);
        return false;
    }
}

/*----------------------------------------------------------
 * 'set' command: set <key> <value>
 *----------------------------------------------------------*/

static struct
{
    struct arg_str *key;
    struct arg_str *value;
    struct arg_end *end;
} set_args;

static int cmd_set(int argc, char **argv)
{
    int ret = arg_parse(argc, argv, (void **)&set_args);
    if (ret != 0)
    {
        arg_print_errors(stdout, set_args.end, argv[0]);
        return 1;
    }
    const char *key = set_args.key->sval[0];
    const char *value = set_args.value->sval[0];
    esp_err_t err = save_config_str(key, value);
    if (err == ESP_OK)
    {
        printf("Saved '%s' = \"%s\"\n", key, value);
    }
    else
    {
        printf("Error saving '%s': %s\n", key, esp_err_to_name(err));
    }
    return 0;
}

/*----------------------------------------------------------
 * 'get' command: get <key> [<default>]
 *----------------------------------------------------------*/
static struct
{
    struct arg_str *key;
    struct arg_str *def;
    struct arg_end *end;
} get_args;

static int cmd_get(int argc, char **argv)
{
    int ret = arg_parse(argc, argv, (void **)&get_args);
    if (ret != 0)
    {
        arg_print_errors(stdout, get_args.end, argv[0]);
        return 1;
    }
    const char *key = get_args.key->sval[0];
    const char *def = get_args.def->sval[0];
    char buf[128];
    bool ok = load_config_str(key, buf, sizeof(buf), def);
    printf("%s: \"%s\" (%s)\n", key, buf, ok ? "loaded" : "default");
    return 0;
}

// void init_nvs_console(int args, int length)
// {

//     esp_console_repl_t *repl = NULL;
//     esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
//     repl_config.task_stack_size = 1024 * 8;
//     repl_config.prompt = "Console>";
//     repl_config.max_cmdline_length = 2048;
//     esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
//     ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));

//     set_args.key = arg_str1(NULL, NULL, "<key>", "NVS key");
//     set_args.value = arg_str1(NULL, NULL, "<value>", "String value");
//     set_args.end = arg_end(2);
//     const esp_console_cmd_t set_cmd = {
//         .command = "set",
//         .help = "set <key> <value> in NVS",
//         .hint = NULL,
//         .func = &cmd_set,
//         .argtable = &set_args};
//     ESP_ERROR_CHECK(esp_console_cmd_register(&set_cmd));

//     // 5) Start REPL loop
//     printf("\nESP32 Console ready. Type 'help' to list commands.\n\n");

//     ESP_ERROR_CHECK(esp_console_start_repl(repl));
// }