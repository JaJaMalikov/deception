#include <string.h>
#include <inttypes.h>
#include "esp_random.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_peripherals.h"
#include "esp_gap_bt_api.h"
#include "playlist.h"
#include "sdcard_scan.h"
#include "sdcard_list.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "board.h"
#include "mp3_decoder.h"
#include "a2dp_stream.h"
#include "fatfs_stream.h"
#include "bt_keycontrol.h"

#define BT_CONNECT_TIMEOUT 20000

static audio_pipeline_handle_t pipeline;
static audio_element_handle_t fatfs_stream_reader, mp3_decoder, bt_stream_writer;
static playlist_handle_t playlist;
static playlist_operator_handle_t sdcard_list_handle;

static void my_sdcard_save_cb(void *user_data, char *url)
{
    playlist_operator_handle_t handle = (playlist_operator_handle_t)user_data;
    sdcard_list_save(handle, url);
}

typedef uint8_t esp_peer_bdname_t[ESP_BT_GAP_MAX_BDNAME_LEN + 1];

static const char *TAG = "BLUETOOTH_SOURCE_EXAMPLE";
static esp_peer_bdname_t remote_bt_device_name;
static bool device_found = false;
static esp_bd_addr_t remote_bd_addr = {0};

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }
    snprintf(str, size, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

static bool get_name_from_eir(uint8_t *eir, uint8_t *bdname, uint8_t *bdname_len)
{
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;
    if (!eir) return false;
    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }
    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }
        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }
    return false;
}

static void filter_inquiry_scan_result(esp_bt_gap_cb_param_t *param)
{
    char bda_str[18];
    uint32_t cod = 0;
    int32_t rssi = -129;
    uint8_t *eir = NULL;
    esp_peer_bdname_t peer_bdname = {0};
    esp_bt_gap_dev_prop_t *p;

    ESP_LOGI(TAG, "Scanned device: %s", bda2str(param->disc_res.bda, bda_str, sizeof(bda_str)));
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        p = param->disc_res.prop + i;
        switch (p->type) {
            case ESP_BT_GAP_DEV_PROP_COD:
                cod = *(uint32_t *)(p->val);
                ESP_LOGI(TAG, "--Class of Device: 0x%" PRIx32, cod);
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                rssi = *(int8_t *)(p->val);
                ESP_LOGI(TAG, "--RSSI: %" PRId32, rssi);
                break;
            case ESP_BT_GAP_DEV_PROP_EIR:
                eir = (uint8_t *)(p->val);
                get_name_from_eir(eir, (uint8_t *)&peer_bdname, NULL);
                ESP_LOGI(TAG, "--Name: %s", peer_bdname);
                break;
            default:
                break;
        }
    }

    ESP_LOGI(TAG, "need device name %s", remote_bt_device_name);
    if (eir) {
        get_name_from_eir(eir, (uint8_t *)&peer_bdname, NULL);
        if (strcmp((char *)peer_bdname, (char *)remote_bt_device_name) != 0) return;
        ESP_LOGI(TAG, "Found a target device, address %s, name %s", bda_str, peer_bdname);
        device_found = true;
        memcpy(&remote_bd_addr, param->disc_res.bda, ESP_BD_ADDR_LEN);
        ESP_LOGI(TAG, "Cancel device discovery ...");
        esp_bt_gap_cancel_discovery();
    }
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT:
            filter_inquiry_scan_result(param);
            break;
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                if (device_found) {
                    ESP_LOGI(TAG, "Device discovery stopped.");
                    ESP_LOGI(TAG, "a2dp connecting to peer: %s", remote_bt_device_name);
                    device_found = false;
                    esp_a2d_source_connect(remote_bd_addr);
                } else {
                    ESP_LOGI(TAG, "Device discovery failed, continue to discover...");
                    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
                }
            } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
                ESP_LOGI(TAG, "Discovery started.");
            }
            break;
        case ESP_BT_GAP_PIN_REQ_EVT:
            ESP_LOGI(TAG, "ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
            if (param->pin_req.min_16_digit) {
                esp_bt_pin_code_t pin_code = {0};
                esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
            } else {
                esp_bt_pin_code_t pin_code = {'1', '2', '3', '4'};
                esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
            }
            break;
        default:
            break;
    }
}

static void play_next_track()
{
    char *url = NULL;
    int total = playlist_get_current_list_url_num(playlist);
    if (total > 1) {
        int index = esp_random() % total;
        if (playlist_choose(playlist, index, &url) == ESP_OK && url != NULL) {
            audio_pipeline_stop(pipeline);
            audio_pipeline_wait_for_stop(pipeline);
            ESP_LOGW(TAG, "URL: %s", url);
            audio_element_set_uri(fatfs_stream_reader, url);
            audio_pipeline_reset_ringbuffer(pipeline);
            audio_pipeline_reset_elements(pipeline);
            audio_pipeline_change_state(pipeline, AEL_STATE_INIT);
            audio_pipeline_run(pipeline);
            return;
        }
    }
    ESP_LOGW(TAG, "[⚠️] Impossible de lire la piste suivante");
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_LOGI(TAG, "[ 1 ] Mount sdcard");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    audio_board_sdcard_init(set, SD_MODE_SPI);

    ESP_LOGI(TAG, "[INIT] Scan des fichiers MP3");
    sdcard_list_create(&sdcard_list_handle);
    sdcard_scan(my_sdcard_save_cb, "/sdcard", 1, (const char *[]) {"mp3"}, 1, sdcard_list_handle);

    ESP_LOGI(TAG, "Création de la playlist");
    playlist = playlist_create();
    playlist_add(playlist, sdcard_list_handle, 0);
    while (playlist_get_current_list_url_num(playlist) == 0) {
        ESP_LOGW(TAG, "[WAIT] Attente de la détection des fichiers .mp3...");
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int total = playlist_get_current_list_url_num(playlist);
    int index = esp_random() % total;
    ESP_LOGI(TAG, "[PLAYLIST] Index choisi aléatoirement : %d / %d", index, total);
    char *url = NULL;
    ESP_ERROR_CHECK(playlist_choose(playlist, index, &url));

    ESP_LOGI(TAG, "[ 2 ] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[3.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[3.1] Create fatfs stream to read data from sdcard");
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);

    ESP_LOGI(TAG, "[3.2] Create mp3 decoder to decode mp3 file");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder = mp3_decoder_init(&mp3_cfg);

    ESP_LOGI(TAG, "[3.3] Create Bluetooth stream");

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_dev_set_device_name("ESP_SOURCE_STREAM_DEMO");
    esp_bt_gap_register_callback(bt_app_gap_cb);

    const char *remote_name = CONFIG_BT_REMOTE_NAME;
    if (remote_name) {
        strncpy((char *)remote_bt_device_name, remote_name, sizeof(remote_bt_device_name) - 1);
        remote_bt_device_name[sizeof(remote_bt_device_name) - 1] = '\0';
    } else {
        strncpy((char *)remote_bt_device_name, "ESP_SINK_STREAM_DEMO", sizeof(remote_bt_device_name) - 1);
        remote_bt_device_name[sizeof(remote_bt_device_name) - 1] = '\0';
    }

    a2dp_stream_config_t a2dp_config = {
        .type = AUDIO_STREAM_WRITER,
        .user_callback = {0},
    };
    bt_stream_writer = a2dp_stream_init(&a2dp_config);

    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    ESP_LOGI(TAG, "[3.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, fatfs_stream_reader, "file");
    audio_pipeline_register(pipeline, mp3_decoder, "mp3");
    audio_pipeline_register(pipeline, bt_stream_writer, "bt");

    ESP_LOGI(TAG, "[3.5] Link it together [sdcard]-->fatfs_stream-->mp3_decoder-->bt_stream-->[bt sink]");
    const char *link_tag[3] = {"file", "mp3", "bt"};
    audio_pipeline_link(pipeline, link_tag, 3);

    ESP_LOGI(TAG, "[3.6] Set up uri");
    audio_element_set_uri(fatfs_stream_reader, url);

    ESP_LOGI(TAG, "[3.7] Create bt peripheral");
    esp_periph_handle_t bt_periph = bt_create_periph();
    esp_periph_start(set, bt_periph);

    ESP_LOGI(TAG, "[ 4 ] Set up event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(pipeline, evt);
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    ESP_LOGI(TAG, "[ 6 ] Listen for all pipeline events");
    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void *)mp3_decoder &&
            msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(mp3_decoder, &music_info);
            ESP_LOGI(TAG, "[ * ] Music info: %d Hz, %d bits, %d ch",
                     music_info.sample_rates, music_info.bits, music_info.channels);
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void *)fatfs_stream_reader &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
            (intptr_t)msg.data == AEL_STATUS_STATE_FINISHED) {
            play_next_track();
        }
    }
}

