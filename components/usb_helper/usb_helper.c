#include "usb_helper.h"
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>
#include "tinyusb.h"
#include "tusb_config.h"
#include "tusb.h"
#include "wear_levelling.h"
#include "vfs_tinyusb.h"
#include "tusb_msc_storage.h"
#include "esp_console.h"
#include "cJSON.h"

/* TinyUSB descriptors
 ********************************************************************* */
#define EPNUM_MSC 1
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
#define CONFIG_ESP_CONSOLE_UART_DEFAULT 1
static const char *TAG = "USB_HELPER";
static esp_console_repl_t *repl = NULL;

enum
{
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL
};

enum
{
    EDPT_CTRL_OUT = 0x00,
    EDPT_CTRL_IN = 0x80,

    EDPT_MSC_OUT = 0x01,
    EDPT_MSC_IN = 0x81,
};

static tusb_desc_device_t descriptor_config = {
    .bLength = sizeof(descriptor_config),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, // This is Espressif VID. This needs to be changed according to Users / Customers
    .idProduct = 0x4002,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01};

static void storage_mount_changed_cb(tinyusb_msc_event_t *event);

static uint8_t const msc_fs_configuration_desc[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, // 0: is supported language is English (0x0409)
    "TNSTech",                  // 1: Manufacturer
    "TNSTech SensorControl",    // 2: Product
    "123456",                   // 3: Serials
    "Example MSC",              // 4. MSC
};

const tinyusb_config_t tusb_cfg = {
    .device_descriptor = &descriptor_config,
    .string_descriptor = string_desc_arr,
    .string_descriptor_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]),
    .external_phy = false,
    .configuration_descriptor = msc_fs_configuration_desc,
};

#define BASE_PATH "/data" // base path to mount the partition

#define PROMPT_STR CONFIG_IDF_TARGET
static int console_unmount(int argc, char **argv);
static int console_read(int argc, char **argv);
static int console_write(int argc, char **argv);
static int console_size(int argc, char **argv);
static int console_exit(int argc, char **argv);

const esp_console_cmd_t cmds[] = {
    {
        .command = "read",
        .help = "read BASE_PATH/README.MD and print its contents",
        .hint = NULL,
        .func = &console_read,
    },
    {
        .command = "write",
        .help = "create file BASE_PATH/README.MD if it does not exist",
        .hint = NULL,
        .func = &console_write,
    },
    {
        .command = "size",
        .help = "show storage size and sector size",
        .hint = NULL,
        .func = &console_size,
    },
    {
        .command = "unmount",
        .help = "Unmount SMC Storage",
        .hint = NULL,
        .func = &console_unmount,
    },
    {
        .command = "exit",
        .help = "exit from application",
        .hint = NULL,
        .func = &console_exit,
    }};

// mount the partition and show all the files in BASE_PATH

static void _mount(void)
{
    ESP_LOGI(TAG, "Mount storage...");
    ESP_ERROR_CHECK(tinyusb_msc_storage_mount(BASE_PATH));

    // List all the files in this directory
    ESP_LOGI(TAG, "\nls command output:");
    struct dirent *d;
    DIR *dh = opendir(BASE_PATH);
    if (!dh)
    {
        if (errno == ENOENT)
        {
            // If the directory is not found
            ESP_LOGE(TAG, "Directory doesn't exist %s", BASE_PATH);
        }
        else
        {
            // If the directory is not readable then throw error and exit
            ESP_LOGE(TAG, "Unable to read directory %s", BASE_PATH);
        }
        return;
    }
    // While the next entry is not readable we will print directory files
    while ((d = readdir(dh)) != NULL)
    {
        printf("%s\n", d->d_name);
    }
    return;
}

// unmount storage
static int console_unmount(int argc, char **argv)
{
    tinyusb_msc_unregister_callback(TINYUSB_MSC_EVENT_MOUNT_CHANGED);
    tinyusb_msc_storage_deinit();
    tinyusb_driver_uninstall();
    return 0;
}

// read BASE_PATH/README.MD and print its contents
static int console_read(int argc, char **argv)
{
    if (tinyusb_msc_storage_in_use_by_usb_host())
    {
        ESP_LOGE(TAG, "storage exposed over USB. Application can't read from storage.");
        return -1;
    }
    ESP_LOGD(TAG, "read from storage:");
    const char *filename = BASE_PATH "/config.json";
    FILE *ptr = fopen(filename, "r");
    if (ptr == NULL)
    {
        ESP_LOGE(TAG, "Filename not present - %s", filename);
        return -1;
    }
    char buf[1024];
    while (fgets(buf, 1000, ptr) != NULL)
    {
        printf("%s", buf);
    }
    fclose(ptr);
    return 0;
}

// create file BASE_PATH/README.MD if it does not exist
static int console_write(int argc, char **argv)
{
    if (tinyusb_msc_storage_in_use_by_usb_host())
    {
        ESP_LOGE(TAG, "storage exposed over USB. Application can't write to storage.");
        return -1;
    }
    ESP_LOGD(TAG, "write to storage:");
    const char *filename = BASE_PATH "/README.MD";
    FILE *fd = fopen(filename, "r");
    if (!fd)
    {
        ESP_LOGW(TAG, "README.MD doesn't exist yet, creating");
        fd = fopen(filename, "w");
        fprintf(fd, "Mass Storage Devices are one of the most common USB devices. It use Mass Storage Class (MSC) that allow access to their internal data storage.\n");
        fprintf(fd, "In this example, ESP chip will be recognised by host (PC) as Mass Storage Device.\n");
        fprintf(fd, "Upon connection to USB host (PC), the example application will initialize the storage module and then the storage will be seen as removable device on PC.\n");
        fclose(fd);
    }
    return 0;
}

// Show storage size and sector size
static int console_size(int argc, char **argv)
{
    if (tinyusb_msc_storage_in_use_by_usb_host())
    {
        ESP_LOGE(TAG, "storage exposed over USB. Application can't access storage");
        return -1;
    }
    uint32_t sec_count = tinyusb_msc_storage_get_sector_count();
    uint32_t sec_size = tinyusb_msc_storage_get_sector_size();
    printf("Storage Capacity %lluKb\n", ((uint64_t)sec_count) * sec_size / (1024));
    return 0;
}

// Exit from application
static int console_exit(int argc, char **argv)
{
    tinyusb_msc_unregister_callback(TINYUSB_MSC_EVENT_MOUNT_CHANGED);
    tinyusb_msc_storage_deinit();
    tinyusb_driver_uninstall();
    printf("Application Exit\n");
    repl->del(repl);
    return 0;
}

// callback that is delivered when storage is mounted/unmounted by application.
static void storage_mount_changed_cb(tinyusb_msc_event_t *event)
{
    ESP_LOGI(TAG, "Storage mounted to application: %s", event->mount_changed_data.is_mounted ? "Yes" : "No");
}

static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle)
{
    ESP_LOGI(TAG, "Initializing wear levelling");

    const esp_partition_t *data_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (data_partition == NULL)
    {
        ESP_LOGE(TAG, "Failed to find FATFS partition. Check the partition table.");
        return ESP_ERR_NOT_FOUND;
    }

    return wl_mount(data_partition, wl_handle);
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    if (!buf)
    {
        ESP_LOGE(TAG, "Out of memory for %ld bytes", len + 1);
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, len, f);
    buf[read] = '\0';
    fclose(f);

    if (out_len)
        *out_len = read;
    ESP_LOGI(TAG, "Read %u bytes from %s", (unsigned)read, path);
    return buf;
}

/**
 * @brief  Load a single JSON field from a file and return it as a malloc'd string.
 * @param  path      Full path to the JSON file (e.g. "/fatfs/config.json")
 * @param  item_key  Top-level key to extract
 * @return A heap-allocated string containing the value (caller must free()), or NULL on error/not-found.
 */
char *load_config_from_fat(const char *path, const char *item_key)
{
    size_t len;
    char *json = read_file(path, &len);
    if (!json)
    {
        ESP_LOGW(TAG, "Using default configuration");
        return NULL;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root)
    {
        ESP_LOGE(TAG, "JSON parse error");
        return NULL;
    }

    // Example: string value
    cJSON *config_item = cJSON_GetObjectItemCaseSensitive(root, item_key);
    char *result = NULL;

    if (cJSON_IsString(config_item) && config_item->valuestring)
    {
        result = strdup(config_item->valuestring);
    }
    else if (cJSON_IsNumber(config_item))
    {
        // convert number to string
        char buf[32];
        if ((double)config_item->valueint == config_item->valuedouble)
        {
            // integer
            snprintf(buf, sizeof(buf), "%d", config_item->valueint);
        }
        else
        {
            // float
            snprintf(buf, sizeof(buf), "%f", config_item->valuedouble);
        }
        result = strdup(buf);
    }

    cJSON_Delete(root);
    if (!result)
    {
        ESP_LOGW("CFG", "Key '%s' not found or unsupported type", item_key);
    }
    return result;
}

void usb_helper_init(void)
{
    ESP_LOGI(TAG, "Initializing storage...");
    static wl_handle_t wl_handle = WL_INVALID_HANDLE;

    ESP_ERROR_CHECK(storage_init_spiflash(&wl_handle));
    const tinyusb_msc_spiflash_config_t config_spi = {
        .wl_handle = wl_handle,
        .callback_mount_changed = storage_mount_changed_cb, /* First way to register the callback. This is while initializing the storage. */
        .mount_config.max_files = 5,
    };

    ESP_ERROR_CHECK(tinyusb_msc_storage_init_spiflash(&config_spi));
    _mount();

    ESP_LOGI(TAG, "USB MSC initialization");

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB MSC initialization DONE");

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    /* Prompt to be printed before each line.
     * This can be customized, made dynamic, etc.
     */
    repl_config.prompt = PROMPT_STR ">";
    repl_config.max_cmdline_length = 64;

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));

#endif

    for (int count = 0; count < sizeof(cmds) / sizeof(esp_console_cmd_t); count++)
    {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[count]));
    }

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
