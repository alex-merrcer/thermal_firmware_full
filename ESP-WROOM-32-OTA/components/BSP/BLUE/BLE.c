

#include "BLE.h"
#ifdef BLE
#include "common.h"
#include <ctype.h>  // for tolower()
#include <string.h> // for strlen()


/* Private function declarations */
inline static void format_addr(char *addr_str, uint8_t addr[]);
static void print_conn_desc(struct ble_gap_conn_desc *desc);
static void start_advertising(void);
static int gap_event_handler(struct ble_gap_event *event, void *arg);

EventGroupHandle_t xCreatedEventGroup_BlueConnect = NULL;

/* Private variables */
static uint8_t own_addr_type;
static uint8_t addr_val[6] = {0};
static uint8_t esp_uri[] = {BLE_GAP_URI_PREFIX_HTTPS, '/', '/', 'e', 's', 'p', 'r', 'e', 's', 's', 'i', 'f', '.', 'c', 'o', 'm'};

/* Private functions */
inline static void format_addr(char *addr_str, uint8_t addr[]) {
    sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X", addr[0], addr[1],
            addr[2], addr[3], addr[4], addr[5]);
}

static void print_conn_desc(struct ble_gap_conn_desc *desc) {
    /* Local variables */
    char addr_str[18] = {0};

    /* Connection handle */
    ESP_LOGI(TAG, "connection handle: %d", desc->conn_handle);

    /* Local ID address */
    format_addr(addr_str, desc->our_id_addr.val);
    ESP_LOGI(TAG, "device id address: type=%d, value=%s",
             desc->our_id_addr.type, addr_str);

    /* Peer ID address */
    format_addr(addr_str, desc->peer_id_addr.val);
    ESP_LOGI(TAG, "peer id address: type=%d, value=%s", desc->peer_id_addr.type,
             addr_str);

    /* Connection info */
    ESP_LOGI(TAG,
             "conn_itvl=%d, conn_latency=%d, supervision_timeout=%d, "
             "encrypted=%d, authenticated=%d, bonded=%d\n",
             desc->conn_itvl, desc->conn_latency, desc->supervision_timeout,
             desc->sec_state.encrypted, desc->sec_state.authenticated,
             desc->sec_state.bonded);
}

static void start_advertising(void) {
    /* Local variables */
    int rc = 0;
    const char *name;
    struct ble_hs_adv_fields adv_fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};
    struct ble_gap_adv_params adv_params = {0};

    /* Set advertising flags */
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /* Set device name */
    name = ble_svc_gap_device_name();
    adv_fields.name = (uint8_t *)name;
    adv_fields.name_len = strlen(name);
    adv_fields.name_is_complete = 1;

    /* Set device tx power */
    adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    adv_fields.tx_pwr_lvl_is_present = 1;

    /* Set device appearance */
    adv_fields.appearance = BLE_GAP_APPEARANCE_GENERIC_TAG;
    adv_fields.appearance_is_present = 1;

    /* Set device LE role */
    adv_fields.le_role = BLE_GAP_LE_ROLE_PERIPHERAL;
    adv_fields.le_role_is_present = 1;

    /* Set advertiement fields */
    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set advertising data, error code: %d", rc);
        return;
    }

    /* Set device address */
    rsp_fields.device_addr = addr_val;
    rsp_fields.device_addr_type = own_addr_type;
    rsp_fields.device_addr_is_present = 1;

    /* Set URI */
    rsp_fields.uri = esp_uri;
    rsp_fields.uri_len = sizeof(esp_uri);

    /* Set advertising interval */
    rsp_fields.adv_itvl = BLE_GAP_ADV_ITVL_MS(500);
    rsp_fields.adv_itvl_is_present = 1;

    /* Set scan response fields */
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set scan response data, error code: %d", rc);
        return;
    }

    /* Set non-connetable and general discoverable mode to be a beacon */
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    /* Set advertising interval */
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(500);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(510);

    /* Start advertising */
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                           gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to start advertising, error code: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising started!");
}

/*
 * NimBLE applies an event-driven model to keep GAP service going
 * gap_event_handler is a callback function registered when calling
 * ble_gap_adv_start API and called when a GAP event arrives
 */
static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    /* Local variables */
    int rc = 0;
    struct ble_gap_conn_desc desc;

    /* Handle different GAP event */
    switch (event->type) {

    /* Connect event */
    case BLE_GAP_EVENT_CONNECT:
        /* A new connection was established or a connection attempt failed. */
        ESP_LOGI(TAG, "connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);

        /* Connection succeeded */
        if (event->connect.status == 0) {
            /* Check connection handle */
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc != 0) {
                ESP_LOGE(TAG,
                         "failed to find connection by handle, error code: %d",
                         rc);
                return rc;
            }

            /* Print connection descriptor */
            print_conn_desc(&desc);

            /* Try to update connection parameters */
            struct ble_gap_upd_params params = {.itvl_min = desc.conn_itvl,
                                                .itvl_max = desc.conn_itvl,
                                                .latency = 3,
                                                .supervision_timeout =
                                                    desc.supervision_timeout};
            rc = ble_gap_update_params(event->connect.conn_handle, &params);
            if (rc != 0) {
                ESP_LOGE(
                    TAG,
                    "failed to update connection parameters, error code: %d",
                    rc);
                return rc;
            }
        }
        /* Connection failed, restart advertising */
        else {
            start_advertising();
        }
        return rc;

    /* Disconnect event */
    case BLE_GAP_EVENT_DISCONNECT:
        /* A connection was terminated, print connection descriptor */
        ESP_LOGI(TAG, "disconnected from peer; reason=%d",
                 event->disconnect.reason);

        /* Restart advertising */
        start_advertising();
        return rc;

    /* Connection parameters update event */
    case BLE_GAP_EVENT_CONN_UPDATE:
        /* The central has updated the connection parameters. */
        ESP_LOGI(TAG, "connection updated; status=%d",
                 event->conn_update.status);

        /* Print connection descriptor */
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        if (rc != 0) {
            ESP_LOGE(TAG, "failed to find connection by handle, error code: %d",
                     rc);
            return rc;
        }
        print_conn_desc(&desc);
        return rc;

    /* Advertising complete event */
    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* Advertising completed, restart advertising */
        ESP_LOGI(TAG, "advertise complete; reason=%d",
                 event->adv_complete.reason);
        start_advertising();
        return rc;

    /* Notification sent event */
    case BLE_GAP_EVENT_NOTIFY_TX:
        if ((event->notify_tx.status != 0) &&
            (event->notify_tx.status != BLE_HS_EDONE)) {
            /* Print notification info on error */
            ESP_LOGI(TAG,
                     "notify event; conn_handle=%d attr_handle=%d "
                     "status=%d is_indication=%d",
                     event->notify_tx.conn_handle, event->notify_tx.attr_handle,
                     event->notify_tx.status, event->notify_tx.indication);
        }
        return rc;

    /* Subscribe event */
    case BLE_GAP_EVENT_SUBSCRIBE:
        /* Print subscription info to log */
        ESP_LOGI(TAG,
                 "subscribe event; conn_handle=%d attr_handle=%d "
                 "reason=%d prevn=%d curn=%d previ=%d curi=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle,
                 event->subscribe.reason, event->subscribe.prev_notify,
                 event->subscribe.cur_notify, event->subscribe.prev_indicate,
                 event->subscribe.cur_indicate);

        /* GATT subscribe event callback */
        gatt_svr_subscribe_cb(event);
        return rc;

    /* MTU update event */
    case BLE_GAP_EVENT_MTU:
        /* Print MTU update info to log */
        ESP_LOGI(TAG, "mtu update event; conn_handle=%d cid=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.channel_id,
                 event->mtu.value);
        return rc;
    }

    return rc;
}


/* Public functions */
void adv_init(void) {
    /* Local variables */
    int rc = 0;
    char addr_str[18] = {0};

    /* Make sure we have proper BT identity address set (random preferred) */
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "device does not have any available bt address!");
        return;
    }

    /* Figure out BT address to use while advertising (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to infer address type, error code: %d", rc);
        return;
    }

    /* Printing ADDR */
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to copy device address, error code: %d", rc);
        return;
    }
    format_addr(addr_str, addr_val);
    ESP_LOGI(TAG, "device address: %s", addr_str);

    /* Start advertising. */
    start_advertising();
}

int gap_init(void) {
    /* Local variables */
    int rc = 0;

    /* Call NimBLE GAP initialization API */
    ble_svc_gap_init();

    /* Set GAP device name */
    rc = ble_svc_gap_device_name_set(DEVICE_NAME_BLE);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set device name to %s, error code: %d",
                 DEVICE_NAME_BLE, rc);
        return rc;
    }
    return rc;
}




/* Private function declarations */
static int city_name_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg);

static int result_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);

/* Private variables */
/* City name service */
static const ble_uuid16_t city_svc_uuid = BLE_UUID16_INIT(0xFF00);
char city_name[MAX_CITY_NAME_LEN + 1] = {0};
static uint16_t city_name_chr_val_handle;
static const ble_uuid16_t city_name_chr_uuid = BLE_UUID16_INIT(0xFF01);

/* Result service */
static const ble_uuid16_t result_svc_uuid = BLE_UUID16_INIT(0xFF02);
static char result_msg[RESULT_MSG_LEN] = "等待城市名称";
static uint16_t result_chr_val_handle;
static const ble_uuid16_t result_chr_uuid = BLE_UUID16_INIT(0xFF03);
static uint16_t result_chr_conn_handle = 0;
static bool result_chr_conn_handle_inited = false;
static bool result_ind_status = false;

/* GATT services table */
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    /* City name service */
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &city_svc_uuid.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){
             {/* City name characteristic */
              .uuid = &city_name_chr_uuid.u,
              .access_cb = city_name_chr_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
              .val_handle = &city_name_chr_val_handle},
             {
                 0, /* End of characteristics */
             }}},

    /* Result service */
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &result_svc_uuid.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){
             {/* Result characteristic */
              .uuid = &result_chr_uuid.u,
              .access_cb = result_chr_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_INDICATE,
              .val_handle = &result_chr_val_handle},
             {
                 0, /* End of characteristics */
             }}},

    {
        0, /* No more services. */
    },
};

/* 检查字符串是否全为英文字母或空格 */
static bool is_english_name(const char *name) {
    for (int i = 0; name[i] != '\0'; i++) {
        if (!isalpha((unsigned char)name[i]) && name[i] != ' ') {
            return false;
        }
    }
    return true;
}

/* City name characteristic access */
static int city_name_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    int rc = 0;
    uint16_t city_len;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        /* 获取写入的数据长度 */
        city_len = OS_MBUF_PKTLEN(ctxt->om);
        
        /* 确保不超过最大长度 */
        if (city_len > MAX_CITY_NAME_LEN) {
            city_len = MAX_CITY_NAME_LEN;
        }
        
        /* 复制数据到缓冲区 */
        rc = ble_hs_mbuf_to_flat(ctxt->om, city_name, city_len, &city_len);
        if (rc != 0) {
            ESP_LOGE(TAG, "Error copying city name: %d", rc);
            return BLE_ATT_ERR_UNLIKELY;
        }
        
        /* 确保字符串以NULL结尾 */
        city_name[city_len] = '\0';
        
        /* 验证城市名称 */
        if (is_english_name(city_name)) {
            strcpy(result_msg, "接收成功");
            if (app_config_build_weather_now_url(dynamic_url, sizeof(dynamic_url), city_name)) {
                xEventGroupSetBits(xCreatedEventGroup_BlueConnect, Blue_CONNECTED_BIT);
            } else {
                strcpy(result_msg, "天气未配置");
                xEventGroupSetBits(xCreatedEventGroup_BlueConnect, Blue_FAIL_BIT);
            }
             
            ESP_LOGI(TAG, "Valid city name received: %s", city_name);
        } else {
            strcpy(result_msg, "名称不对");
            ESP_LOGW(TAG, "Invalid city name: %s", city_name);
            xEventGroupSetBits(xCreatedEventGroup_BlueConnect, Blue_FAIL_BIT);
        }
        /* 发送结果通知 */
        if (result_ind_status && result_chr_conn_handle_inited) {
            ble_gatts_indicate(result_chr_conn_handle, result_chr_val_handle);
            ESP_LOGI(TAG, "Result indication sent: %s", result_msg);
        }
        break;

    default:
        ESP_LOGE(TAG, "Unsupported operation on city name characteristic: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }
    
    return rc;
}

/* Result characteristic access */
static int result_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    int rc;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        rc = os_mbuf_append(ctxt->om, result_msg, strlen(result_msg));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        
    default:
        ESP_LOGE(TAG, "Unsupported operation on result characteristic: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* GATT server subscribe event callback */
void gatt_svr_subscribe_cb(struct ble_gap_event *event) {
    if (event->subscribe.attr_handle == result_chr_val_handle) {
        result_chr_conn_handle = event->subscribe.conn_handle;
        result_chr_conn_handle_inited = true;
        result_ind_status = event->subscribe.cur_indicate;
        ESP_LOGI(TAG, "Result characteristic subscribed: %s", 
                 result_ind_status ? "INDICATE" : "unsubscribed");
    }
}


int gatt_svc_init(void) {
    /* Local variables */
    int rc;

    /* 1. GATT service initialization */
    ble_svc_gatt_init();

    /* 2. Update GATT services counter */
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    /* 3. Add GATT services */
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    return 0;
}


void on_stack_reset(int reason)
{
    /* On reset, print reset reason to console */
    ESP_LOGI(TAG, "nimble stack reset, reset reason: %d", reason);
}

void on_stack_sync(void)
{
    /* On stack sync, do advertising initialization */
    adv_init();
}

void nimble_host_config_init(void)
{
    /* Set host callbacks */
    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb = on_stack_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Store host configuration */
    ble_store_config_init();
}



void BLE_Init(void)
{
    xCreatedEventGroup_BlueConnect = xEventGroupCreate();  // 创建事件组
        int rc;
        /* NimBLE stack initialization */
        ret = nimble_port_init();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "failed to initialize nimble stack, error code: %d ",
                    ret);
            return;
        }

        /* GAP service initialization */
        rc = gap_init();
        if (rc != 0)
        {
            ESP_LOGE(TAG, "failed to initialize GAP service, error code: % d", rc);
            return;
        }

        /* GATT server initialization */
        rc = gatt_svc_init();
        if (rc != 0)
        {
            ESP_LOGE(TAG, "failed to initialize GATT server, error code: %d", rc);
            return;
        }

        /* NimBLE host configuration initialization */
        nimble_host_config_init();
        /* Start NimBLE host task thread and return */
        return;

}

#endif
