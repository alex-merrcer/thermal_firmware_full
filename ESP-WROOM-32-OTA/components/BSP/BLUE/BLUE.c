#include "BLUE.h"
#ifdef BT
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include <string.h>
#include <ctype.h>  // for tolower()
#include <stdlib.h> // for malloc()
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"

#include "esp_gatt_common_api.h"
#include <led.h>
#include "HTTP.h"

#define GATTS_TABLE_TAG "GATTS_TABLE_DEMO" // 日志标签
EventGroupHandle_t xCreatedEventGroup_BlueConnect = NULL;
#define PROFILE_NUM 1                     // GATT配置文件数量
#define PROFILE_APP_IDX 0                 // 配置文件索引
#define ESP_APP_ID 0x55                   // 应用程序ID
#define SAMPLE_DEVICE_NAME "esp32-wroom-32e" // 蓝牙设备名称
#define SVC_INST_ID 0                     // 服务实例ID

/* 特征值的最大长度。当GATT客户端执行写入或准备写入操作时，
 * 数据长度必须小于GATTS_DEMO_CHAR_VAL_LEN_MAX。
 */
#define GATTS_DEMO_CHAR_VAL_LEN_MAX 500         // 特征值最大长度
#define PREPARE_BUF_MAX_SIZE 1024               // 准备写入缓冲区最大大小
#define CHAR_DECLARATION_SIZE (sizeof(uint8_t)) // 特征声明大小

#define ADV_CONFIG_FLAG (1 << 0)      // 广播配置标志位
#define SCAN_RSP_CONFIG_FLAG (1 << 1) // 扫描响应配置标志位

static uint8_t adv_config_done = 0; // 广播配置完成标志

uint16_t heart_rate_handle_table[HRS_IDX_NB]; // 心率服务句柄表

// 准备写入环境结构体
typedef struct
{
    uint8_t *prepare_buf; // 准备写入缓冲区指针
    int prepare_len;      // 准备写入数据长度
} prepare_type_env_t;

static prepare_type_env_t prepare_write_env; // 准备写入环境实例

// 手动配置原始广播数据，完全自定义广播包字节流
#ifdef CONFIG_SET_RAW_ADV_DATA
static uint8_t raw_adv_data[] = {
    /* 广播标志 */
    0x02, // 长度：后续2字节
    0x01, // 类型：广播标志（Flags）
    0x06, // 数值：0x06 = 00000110b
          //       表示：普通发现模式（LE General Discoverable Mode）
          //       且不支持经典蓝牙（BR/EDR）

    /* 发射功率 */
    0x02, // 长度：后续2字节
    0x0A, // 类型：发射功率（Tx Power Level）
    0xEB, // 数值：0xEB = -21 dBm（有符号补码形式）
          //       ESP32典型发射功率配置

    /* 服务UUID */
    0x03,       // 长度：后续3字节
    0x03,       // 类型：16位完整UUID列表（Complete List of 16-bit Service UUIDs）
    0xFF, 0x00, // UUID值：0xFF00（自定义服务UUID）

    /* 设备名称 */
    0x0F,                                                                // 长度：后续15字节（设备名长度+类型占1字节）
    0x09,                                                                // 类型：完整设备名（Complete Local Name）
    'E', 'S', 'P', '_', 'G', 'A', 'T', 'T', 'S', '_', 'D', 'E', 'M', 'O' // 设备名称字符串
};

// 原始扫描响应数据
static uint8_t raw_scan_rsp_data[] = {
    /* 广播标志（重复广播包中的内容）*/
    0x02, 0x01, 0x06,

    /* 发射功率（重复）*/
    0x02, 0x0A, 0xEB,

    /* 服务UUID（重复）*/
    0x03, 0x03, 0xFF, 0x00};

#else
// 使用 ESP-IDF提供的结构化配置，通过API自动生成广播包。
//  服务UUID (16字节)，UUID格式：小端字节序（低位在前，高位在后）
static uint8_t service_uuid[16] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    // 第一个UUID, 16bit, [12],[13]是值
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xFF, // UUID低字节0x00, // UUID高字节0x00,0x00,
};

// 广播数据配置
/* The length of adv data must be less than 31 bytes */
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,                    // 配置的是广播数据（非扫描响应）
    .include_name = true,                     // 包含设备名称（自动生成）
    .include_txpower = true,                  // 包含发射功率（自动生成）
    .min_interval = 0x0006,                   // 最小广播间隔：7.5ms (0x0006 * 1.25ms)
    .max_interval = 0x0010,                   // 最大广播间隔：20ms (0x0010 * 1.25ms)
    .appearance = 0x00,                       // 通用设备类型（默认值）
    .manufacturer_len = 0,                    // 不包含制造商数据
    .p_manufacturer_data = NULL,              // 制造商数据指针为空
    .service_data_len = 0,                    // 不包含服务数据
    .p_service_data = NULL,                   // 服务数据指针为空
    .service_uuid_len = sizeof(service_uuid), // 服务UUID长度
    .p_service_uuid = service_uuid,           // 指向服务UUID数据的指针
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    // 广播标志: 通用可发现模式 | 不支持BR/EDR(经典蓝牙)
};

// 扫描响应数据结构体配置
// 作用：当其他设备主动扫描时，补充发送额外的信息（如更长的设备名称或服务数据）。
static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp = true,                     // 设置为扫描响应数据
    .include_name = true,                     // 包含设备名称
    .include_txpower = true,                  // 包含发射功率信息
    .min_interval = 0x0006,                   // 最小连接间隔(0x0006 * 1.25ms = 7.5ms)
    .max_interval = 0x0010,                   // 最大连接间隔(0x0010 * 1.25ms = 20ms)
    .appearance = 0x00,                       // 设备外观类别(通用)
    .manufacturer_len = 0,                    // 制造商数据长度(0表示不包含)
    .p_manufacturer_data = NULL,              // 制造商数据指针(无)
    .service_data_len = 0,                    // 服务数据长度(0表示不包含)
    .p_service_data = NULL,                   // 服务数据指针(无)
    .service_uuid_len = sizeof(service_uuid), // 服务UUID长度
    .p_service_uuid = service_uuid,           // 服务UUID数据指针
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    // 广播标志: 通用发现模式 | 不支持BR/EDR(传统蓝牙)
};

#endif /* CONFIG_SET_RAW_ADV_DATA */

// 广播参数配置
static esp_ble_adv_params_t adv_params = {
    // 间隔越大，功耗越低，但设备被发现所需时间可能更长。
    .adv_int_min = 0x20,                                   // 最小广播间隔：0x20 * 0.625ms = 32ms
    .adv_int_max = 0x40,                                   // 最大广播间隔：0x40 * 0.625ms = 64ms
    .adv_type = ADV_TYPE_IND,                              // 可连接的非定向广播（Indicatable）,允许任何设备发起连接。
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,                 // 使用设备出厂烧录的公共MAC地址
    .channel_map = ADV_CHNL_ALL,                           // 启用所有3个BLE广播信道（37/38/39）,提高被发现概率
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY // 允许任何设备扫描和连接（无白名单）
};

// GATT服务配置结构体
struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;          // GATT服务器事件回调函数指针,处理GATT服务器事件（如连接、断开、读/写请求），需实现具体逻辑
    uint16_t gatts_if;               // GATT接口ID（由ESP_GATTS_REG_EVT事件分配）,在注册服务时由ESP_GATTS_REG_EVT事件返回，用于后续操作（如发送通知）
    uint16_t app_id;                 // 应用ID（用于标识不同服务）
    uint16_t conn_id;                // 当前连接ID（仅在有连接时有效）
    uint16_t service_handle;         // 服务句柄（唯一标识符）
    esp_gatt_srvc_id_t service_id;   // 服务ID（包含UUID和实例号）
    uint16_t char_handle;            // 特征值句柄
    esp_bt_uuid_t char_uuid;         // 特征值UUID（如心率测量值）
    esp_gatt_perm_t perm;            // 特征值访问权限（读/写/加密等）
    esp_gatt_char_prop_t property;   // 特征值属性（通知/指示/读/写等）
    uint16_t descr_handle;           // 描述符句柄（如客户端特征配置描述符CCCD）
    esp_bt_uuid_t descr_uuid;        // 描述符UUID（如0x2902对应CCCD）
};

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, // 事件类型（如连接、读请求）
                                        esp_gatt_if_t gatts_if,     // 触发事件的GATT接口ID
                                        esp_ble_gatts_cb_param_t *param);   // 事件参数（结构体，包含详细信息）

/* One gatt-based profile one app_id and one gatts_if, this array will store the gatts_if returned by ESP_GATTS_REG_EVT */
//GATT服务配置表
static struct gatts_profile_inst heart_rate_profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_IDX] = {
        .gatts_cb = gatts_profile_event_handler, // 事件处理回调
        .gatts_if = ESP_GATT_IF_NONE             // 初始化为无接口
    },
};


char received_city[MAX_CITY_NAME_LEN + 1] = {0};
/* Service */
//自定义UUID：所有UUID均为16位自定义值，表示这是一个私有服务，而非标准服务（标准服务使用16位或128位统一分配的UUID）。
//服务与特征关系：主服务0x00FF包含多个特征（A/B/C/LED/TEMP），每个特征实现不同功能。
static const uint16_t GATTS_SERVICE_UUID_TEST = 0x00FF;   // 主服务UUID
static const uint16_t GATTS_CHAR_UUID_TEST_A = 0xFF01;    // 特征A UUID
static const uint16_t GATTS_CHAR_UUID_TEST_LED = 0xFF04;  // LED控制特征
static const uint16_t GATTS_CHAR_UUID_TEST_TEMP = 0xFF05; // 温度特征
static const uint16_t GATTS_CHAR_UUID_TEST_WEATHER = 0xFF07; // 天气特征

//GATT属性类型与权限常量
static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;    // 主服务UUID (0x2800)
static const uint16_t character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;   // 特征声明UUID (0x2803)
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG; // 客户端特征配置描述符UUID (0x2902)
//static const uint8_t char_prop_read = ESP_GATT_CHAR_PROP_BIT_READ;   // 只读属性
//static const uint8_t char_prop_write = ESP_GATT_CHAR_PROP_BIT_WRITE;    // 只写属性
static const uint8_t char_prop_read_notify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;   // 读+通知属性
static const uint8_t char_prop_read_write_notify = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY; // 读+写+通知属性
static const uint8_t heart_measurement_ccc[2] = {0x00, 0x00};   // 心率测量客户端配置描述符默认值(禁用通知/指示)，0x0000表示客户端未启用通知/指示，需客户端写入0x0001（通知）或0x0002（指示）以启用。
static const uint8_t char_value[4] = {0x11, 0x22, 0x33, 0x44};   // 通用特征值默认数据
static const uint8_t led_value[1] = {0x00};      // LED控制特征默认值(关)
static const uint8_t temp_value[1] = {0x00};      // 温度特征默认值
static const uint8_t WEA_value[4] = {0x11, 0x22, 0x33, 0x44};   // 通用特征值默认数据

/* Full Database Description - Used to add attributes into the database */
static const esp_gatts_attr_db_t gatt_db[HRS_IDX_NB] =
{
        // 这种结构是ESP32 BLE开发中构建GATT服务的标准方式，通过预定义属性表来快速创建完整的BLE服务。
        /* 主服务声明 */
        [IDX_SVC] = {
            {ESP_GATT_AUTO_RSP},                  // 自动响应配置
            {ESP_UUID_LEN_16,                     // UUID长度(16位)
             (uint8_t *)&primary_service_uuid,    // 主服务UUID(0x2800)
             ESP_GATT_PERM_READ,                  // 只读权限
             sizeof(uint16_t),                    // 属性值长度
             sizeof(GATTS_SERVICE_UUID_TEST),     // 实际值长度
             (uint8_t *)&GATTS_SERVICE_UUID_TEST} // 服务UUID值(0x00FF)
        },

        /* 特征A声明 */
        [IDX_CHAR_A] = {
            {ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16,(uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ, 
            CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_notify} // 特征属性:读+写+通知
        },

        /* 特征A的值 */
        [IDX_CHAR_VAL_A] = {
            {ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16,
                                  (uint8_t *)&GATTS_CHAR_UUID_TEST_A,       // 特征A UUID(0xFF01)
                                  ESP_GATT_PERM_READ, // 读写权限
                                  GATTS_DEMO_CHAR_VAL_LEN_MAX,              // 最大长度(500字节)
                                  sizeof(char_value),                       // 实际值长度(4字节)
                                  (uint8_t *)char_value}                    // 初始值{0x11,0x22,0x33,0x44}
        },

        /* 特征A的客户端配置描述符 */
        [IDX_CHAR_CFG_A] = {
            {ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16,
                                  (uint8_t *)&character_client_config_uuid,                        // 客户端配置UUID(0x2902)
                                  ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,                        // 读写权限
                                  sizeof(char_value),                                                // 值长度(2字节)
                                  sizeof(heart_measurement_ccc), (uint8_t *)heart_measurement_ccc} // 初始值{0x00,0x00}(禁用通知)
        },

        /* Characteristic Declaration */
        //客户端可通过写入IDX_CHAR_VAL_LED特征值（如0x01开，0x00关）控制LED，服务端可通过通知主动推送LED状态变化
        [IDX_CHAR_LED] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ, CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write_notify}},

        /* Characteristic Value */
        [IDX_CHAR_VAL_LED] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_LED, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(led_value), (uint8_t *)led_value}},

        /* Characteristic Declaration */
        [IDX_CHAR_WEATHER] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ, CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write_notify}},

        /* Characteristic Value */
        [IDX_CHAR_VAL_WEATHER] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_WEATHER, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(WEA_value), (uint8_t *)WEA_value}},

        /* Characteristic Declaration */
        //特征值权限为只读，客户端只能读取或订阅通知，服务端需定期更新温度值并触发通知（需客户端启用CCCD）。
        [IDX_CHAR_TEMP] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ, CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_notify}},

        /* Characteristic Value */
        [IDX_CHAR_VAL_TEMP] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_TEST_TEMP, ESP_GATT_PERM_READ, GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(temp_value), (uint8_t *)temp_value}},

        /* Client Characteristic Configuration Descriptor */
        [IDX_CHAR_CFG_TEMP] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(uint16_t), sizeof(heart_measurement_ccc), (uint8_t *)heart_measurement_ccc}},


};

/* BLE GAP事件处理函数 - 处理蓝牙广播相关事件 */
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
#ifdef CONFIG_SET_RAW_ADV_DATA
    /* 原始广播数据设置完成事件 */
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= (~ADV_CONFIG_FLAG); // 清除广播配置标志
        if (adv_config_done == 0)              // 如果所有配置都完成
        {
            esp_ble_gap_start_advertising(&adv_params); // 开始广播
        }
        break;

    /* 原始扫描响应数据设置完成事件 */
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= (~SCAN_RSP_CONFIG_FLAG); // 清除扫描响应配置标志
        if (adv_config_done == 0)
        {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
#else
    /* 非原始广播数据设置完成事件 */
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~ADV_CONFIG_FLAG);  // 清除广播配置标志
        if (adv_config_done == 0)    // 所有配置完成
        {
            esp_ble_gap_start_advertising(&adv_params);  // 开始广播
        }
        break;

    /* 扫描响应数据设置完成事件 */
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~SCAN_RSP_CONFIG_FLAG); // 清除广播配置标志
        if (adv_config_done == 0)
        {
            esp_ble_gap_start_advertising(&adv_params); // 开始广播
        }
        break;
#endif

    /* 广播启动完成事件 */
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(GATTS_TABLE_TAG, "advertising start failed"); // 广播启动失败日志
        }
        else
        {
            ESP_LOGI(GATTS_TABLE_TAG, "advertising start successfully"); // 广播启动成功日志
        }
        break;

    /* 广播停止完成事件 */
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(GATTS_TABLE_TAG, "Advertising stop failed"); // 广播停止失败日志
        }
        else
        {
            ESP_LOGI(GATTS_TABLE_TAG, "Stop adv successfully\n"); // 广播停止成功日志
        }
        break;

    /* 连接参数更新事件 */
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        // 打印连接参数更新信息
        ESP_LOGI(GATTS_TABLE_TAG, "update connection params status = %d, min_int = %d, max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                 param->update_conn_params.status,
                 param->update_conn_params.min_int,
                 param->update_conn_params.max_int,
                 param->update_conn_params.conn_int,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;

    default:
        break;
    }
}

/* 准备写入事件处理函数 - 处理GATT客户端准备写入请求 */
void example_prepare_write_event_env(esp_gatt_if_t gatts_if, prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param)
{
    //记录当前写入的GATT属性句柄和写入数据长度
    ESP_LOGI(GATTS_TABLE_TAG, "prepare write, handle = %d, value len = %d", param->write.handle, param->write.len);
    esp_gatt_status_t status = ESP_GATT_OK;

    /* 检查并初始化准备写入缓冲区 */
    if (prepare_write_env->prepare_buf == NULL)
    {
        prepare_write_env->prepare_buf = (uint8_t *)malloc(PREPARE_BUF_MAX_SIZE * sizeof(uint8_t));
        prepare_write_env->prepare_len = 0;
        if (prepare_write_env->prepare_buf == NULL)
        {
            ESP_LOGE(GATTS_TABLE_TAG, "%s, Gatt_server prep no mem", __func__);// 内存不足错误
            status = ESP_GATT_NO_RESOURCES;
        }
    }
    else
    {
        /* 检查写入偏移和长度是否有效 */
        if (param->write.offset > PREPARE_BUF_MAX_SIZE)
        {
            status = ESP_GATT_INVALID_OFFSET;   // 偏移超出缓冲区大小
        }
        else if ((param->write.offset + param->write.len) > PREPARE_BUF_MAX_SIZE)
        {
            status = ESP_GATT_INVALID_ATTR_LEN; // 数据长度超出缓冲区剩余空间
        }
    }

    /* 如果需要响应，则发送写入响应 */
    if (param->write.need_rsp)
    {
        esp_gatt_rsp_t *gatt_rsp = (esp_gatt_rsp_t *)malloc(sizeof(esp_gatt_rsp_t));
        if (gatt_rsp != NULL)
        {
            /* 填充响应数据 */
            gatt_rsp->attr_value.len = param->write.len;
            gatt_rsp->attr_value.handle = param->write.handle;
            gatt_rsp->attr_value.offset = param->write.offset;
            gatt_rsp->attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
            memcpy(gatt_rsp->attr_value.value, param->write.value, param->write.len);

            /* 发送响应 */
            esp_err_t response_err = esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, status, gatt_rsp);
            if (response_err != ESP_OK)
            {
                ESP_LOGE(GATTS_TABLE_TAG, "Send response error");
            }
            free(gatt_rsp);
        }
        else
        {
            ESP_LOGE(GATTS_TABLE_TAG, "%s, malloc failed", __func__);
        }
    }

    /* 如果状态不正常则返回 */
    if (status != ESP_GATT_OK)
    {
        return;
    }

    /* 将数据复制到准备缓冲区 */
    memcpy(prepare_write_env->prepare_buf + param->write.offset,
           param->write.value,
           param->write.len);
    prepare_write_env->prepare_len += param->write.len;
}

/* 执行写入事件处理函数 - 处理GATT客户端执行写入请求 */
void example_exec_write_event_env(prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param)
{
    /* 检查是否执行写入操作 */
    if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC && prepare_write_env->prepare_buf)
    {
        /* 打印准备写入的数据 */
        //使用 esp_log_buffer_hex 将缓冲区中的数据以十六进制形式打印到日志，用于调试或验证数据完整性。
        esp_log_buffer_hex(GATTS_TABLE_TAG, prepare_write_env->prepare_buf, prepare_write_env->prepare_len);
    }
    else
    {
        ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATT_PREP_WRITE_CANCEL");
    }

    /* 释放准备缓冲区 */
    if (prepare_write_env->prepare_buf)
    {
        free(prepare_write_env->prepare_buf);
        prepare_write_env->prepare_buf = NULL;
    }
    prepare_write_env->prepare_len = 0;
}

TaskHandle_t *pTask = NULL;
volatile bool notify_flag = false;
/* 温度数据获取任务 - 定期生成随机温度数据并通过BLE通知发送 */
static void get_temp(void *arg)
{
    while (1) // 无限循环
    {
        if (notify_flag == true) // 检查通知是否已启用
        {
            uint8_t temp = 20 + rand() % 11; // 生成20-30之间的随机温度值
            // 设置温度特征值属性，将温度值写入GATT属性，确保后续读取操作能获取最新数据。
            //用于更新 GATT 特征值（Characteristic Value）的数据，客户端读取该特征时会获取最新值。
            esp_ble_gatts_set_attr_value(heart_rate_handle_table[IDX_CHAR_VAL_TEMP],  // 特征值的句柄（Handle）
                                                1,  // 数据长度
                                                &temp);  // 数据指针
            // 发送温度通知到客户端
            esp_ble_gatts_send_indicate(heart_rate_profile_tab[0].gatts_if, //GATT接口ID
                                        heart_rate_profile_tab[0].conn_id,   // 连接ID
                                        heart_rate_handle_table[IDX_CHAR_VAL_TEMP], // 特征值的句柄
                                        1,   // 数据长度
                                        &temp,  // 数据指针
                                        false    // 是否为指示（true=指示，false=通知）
                                            ); // false表示通知(notification)
        }
        else
        {
            vTaskDelete(NULL); // 如果通知被禁用，则删除当前任务
        }
        vTaskDelay(pdMS_TO_TICKS(2000)); // 每2秒执行一次
    }
}

void send_text_notification(uint8_t *msg, uint16_t len) {
    // UTF-8编码检查（防止中文乱码）
    if (len > 20) { // BLE MTU通常为20字节
        ESP_LOGE(GATTS_TABLE_TAG, "Text too long, truncate to 20 bytes");
        len = 20;
    }
    
    // 更新特征值属性
    esp_ble_gatts_set_attr_value(heart_rate_handle_table[IDX_CHAR_VAL_A], 
                                len, msg);
                                
    // 发送通知
    esp_ble_gatts_send_indicate(
        heart_rate_profile_tab[0].gatts_if,
        heart_rate_profile_tab[0].conn_id,
        heart_rate_handle_table[IDX_CHAR_VAL_A],
        len, 
        msg,
        false // 使用notification而非indication
    );
}

// 添加城市名校验函数
bool is_valid_city_name(const char *city) {
    if (city == NULL || strlen(city) == 0) {
        return false;  // 空指针或空字符串直接拒绝
    }
    
    for (size_t i = 0; i < strlen(city); i++) {
        // 检查是否为A-Z/a-z字符（包含ASCII字母）
        if (!isalpha((unsigned char)city[i])) {
            return false;
        }
    }
    return true;
}


/* GATT配置文件事件处理函数 - 处理GATT服务器各种事件 */
static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event)
    {
    /* 注册事件 - GATT服务注册完成 */
    case ESP_GATTS_REG_EVT:
    {
       
        esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(SAMPLE_DEVICE_NAME); // 设置BLE设备名称
        if (set_dev_name_ret)
        {
            ESP_LOGE(GATTS_TABLE_TAG, "set device name failed, error code = %x", set_dev_name_ret);
        }

#ifdef CONFIG_SET_RAW_ADV_DATA
        // 配置原始广播数据
        esp_err_t raw_adv_ret = esp_ble_gap_config_adv_data_raw(raw_adv_data, sizeof(raw_adv_data));
        if (raw_adv_ret)
        {
            ESP_LOGE(GATTS_TABLE_TAG, "config raw adv data failed, error code = %x ", raw_adv_ret);
        }
        adv_config_done |= ADV_CONFIG_FLAG;

        // 配置原始扫描响应数据
        esp_err_t raw_scan_ret = esp_ble_gap_config_scan_rsp_data_raw(raw_scan_rsp_data, sizeof(raw_scan_rsp_data));
        if (raw_scan_ret)
        {
            ESP_LOGE(GATTS_TABLE_TAG, "config raw scan rsp data failed, error code = %x", raw_scan_ret);
        }
        adv_config_done |= SCAN_RSP_CONFIG_FLAG;
#else
        // 配置广播数据
        esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
        if (ret)
        {
            ESP_LOGE(GATTS_TABLE_TAG, "config adv data failed, error code = %x", ret);
        }
        adv_config_done |= ADV_CONFIG_FLAG;

        // 配置扫描响应数据
        ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
        if (ret)
        {
            ESP_LOGE(GATTS_TABLE_TAG, "config scan response data failed, error code = %x", ret);
        }
        adv_config_done |= SCAN_RSP_CONFIG_FLAG;
#endif

        // 创建属性表
        esp_err_t create_attr_ret = esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, HRS_IDX_NB, SVC_INST_ID);
        if (create_attr_ret)
        {
            ESP_LOGE(GATTS_TABLE_TAG, "create attr table failed, error code = %x", create_attr_ret);
        }
    }
    break;

    /* 读取事件 - 客户端读取特征值 */
    case ESP_GATTS_READ_EVT:
        ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_READ_EVT");
        break;

    /* 写入事件 - 客户端写入特征值 */
    case ESP_GATTS_WRITE_EVT:
        if (!param->write.is_prep) 
    {
        // 创建缓冲区并转换为字符串
        char* received_str = (char*)malloc(param->write.len + 1); // +1 for null terminator
        
        if (received_str) 
        {
            memcpy(received_str, param->write.value, param->write.len);
            received_str[param->write.len] = '\0'; // 添加字符串终止符
            
            // LED控制特征处理
            if (heart_rate_handle_table[IDX_CHAR_VAL_LED] == param->write.handle) 
            {
                ESP_LOGI(GATTS_TABLE_TAG, "String data: %s", received_str);
                // 转换为小写处理（不区分大小写）
                for(int i=0; received_str[i]; i++){
                    received_str[i] = tolower(received_str[i]);
                }

                // 清除尾部换行/空格（适配不同APP的发送习惯）
                char* sanitized_str = strtok(received_str, " \r\n\t");

                // 命令判断
                if (sanitized_str) 
                {
                    if (strcmp(sanitized_str, "on") == 0) 
                    {
                        LED(0);
                        ESP_LOGI(GATTS_TABLE_TAG, "Turn ON LED");
                        const char *msg = "LED已开启";
                        send_text_notification((uint8_t*)msg, strlen(msg)); // UTF-8编码
                    } 
                    else if (strcmp(sanitized_str, "off") == 0) 
                    {
                        LED(1);
                        ESP_LOGI(GATTS_TABLE_TAG, "Turn OFF LED");
                        const char *msg = "LED已关闭";
                        send_text_notification((uint8_t*)msg, strlen(msg));
                    }
                    else 
                    {
                        ESP_LOGW(GATTS_TABLE_TAG, "Unrecognized command: %s", sanitized_str);
                    }
                }
            }
            free(received_str);
        }

            if (param->write.handle == heart_rate_handle_table[IDX_CHAR_VAL_WEATHER] && param->write.len > 0) 
        {
            // 1. 数据长度验证
            size_t copy_len = (param->write.len > MAX_CITY_NAME_LEN) ? MAX_CITY_NAME_LEN : param->write.len;
            
            // 2. 复制数据到缓冲区
            memset(received_city, 0, sizeof(received_city));
            memcpy(received_city, param->write.value, copy_len);
            
            // 3. 编码清理（UTF-8兼容）
            for (int i = 0; i < copy_len; i++) {
                if (received_city[i] < 32 || received_city[i] > 126) { // 过滤非ASCII可打印字符
                    received_city[i] = '_';
                }
            }
            
            if (is_valid_city_name(received_city)) {
                const char *msg = "接收城市成功";
                send_text_notification((uint8_t*)msg, strlen(msg));
                if (app_config_build_weather_now_url(dynamic_url, sizeof(dynamic_url), received_city)) {
                    xEventGroupSetBits(xCreatedEventGroup_BlueConnect, Blue_CONNECTED_BIT);
                } else {
                    const char *config_msg = "天气未配置";
                    send_text_notification((uint8_t*)config_msg, strlen(config_msg));
                    xEventGroupSetBits(xCreatedEventGroup_BlueConnect, Blue_FAIL_BIT);
                }
            } 
            else {
                const char *error_msg = "名称有错";
                send_text_notification((uint8_t*)error_msg, strlen(error_msg));
                xEventGroupSetBits(xCreatedEventGroup_BlueConnect, Blue_FAIL_BIT);
            }
  
        }
        

            // 温度通知配置处理
            if (heart_rate_handle_table[IDX_CHAR_CFG_A] == param->write.handle && param->write.len == 2)
            {
                uint16_t descr_value = param->write.value[1] << 8 | param->write.value[0];

                if (descr_value == 0x0001)
                { // 启用通知
                    ESP_LOGI(GATTS_TABLE_TAG, "notify enable");
                    //xTaskCreate(get_temp, "get temp", 8192, NULL, 10, pTask);
                    notify_flag = true;
                }
                else if (descr_value == 0x0000)
                { // 禁用通知/指示
                    ESP_LOGI(GATTS_TABLE_TAG, "notify/indicate disable ");
                    notify_flag = false;
                }
                else
                { // 未知配置值
                    ESP_LOGE(GATTS_TABLE_TAG, "unknown descr value");
                    esp_log_buffer_hex(GATTS_TABLE_TAG, param->write.value, param->write.len);
                }
            }

            



            // 如果需要响应则发送响应
            if (param->write.need_rsp)
            {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id, ESP_GATT_OK, NULL);
            }
        }
        else
        { // 处理准备写入
            example_prepare_write_event_env(gatts_if, &prepare_write_env, param);
        }
        break;

    /* 执行写入事件 - 客户端执行准备写入 */
    case ESP_GATTS_EXEC_WRITE_EVT:
        ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_EXEC_WRITE_EVT");
        example_exec_write_event_env(&prepare_write_env, param);
        break;

    /* MTU交换事件 */
    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_MTU_EVT, MTU %d", param->mtu.mtu);
        break;

    /* 确认事件 */
    case ESP_GATTS_CONF_EVT:
        ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_CONF_EVT, status = %d, attr_handle %d",
                 param->conf.status, param->conf.handle);
        break;

    /* 服务启动事件 */
    case ESP_GATTS_START_EVT:
        ESP_LOGI(GATTS_TABLE_TAG, "SERVICE_START_EVT, status %d, service_handle %d",
                 param->start.status, param->start.service_handle);
        break;

    /* 连接事件 */
    case ESP_GATTS_CONNECT_EVT:
        ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_CONNECT_EVT, conn_id = %d", param->connect.conn_id);
        esp_log_buffer_hex(GATTS_TABLE_TAG, param->connect.remote_bda, 6);

        // 更新连接参数
        esp_ble_conn_update_params_t conn_params = {0};
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        conn_params.latency = 0;
        conn_params.max_int = 0x20; // 最大间隔40ms
        conn_params.min_int = 0x10; // 最小间隔20ms
        conn_params.timeout = 400;  // 超时4000ms
        heart_rate_profile_tab[0].conn_id = param->connect.conn_id;
        esp_ble_gap_update_conn_params(&conn_params);
        break;

    /* 断开连接事件 */
    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_DISCONNECT_EVT, reason = 0x%x", param->disconnect.reason);
        esp_ble_gap_start_advertising(&adv_params); // 重新开始广播
        break;

    /* 创建属性表事件 */
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK)
        {
            ESP_LOGE(GATTS_TABLE_TAG, "create attribute table failed, error code=0x%x",
                     param->add_attr_tab.status);
        }
        else if (param->add_attr_tab.num_handle != HRS_IDX_NB)
        {
            ESP_LOGE(GATTS_TABLE_TAG, "create attribute table abnormally, num_handle (%d) doesn't equal to HRS_IDX_NB(%d)",
                     param->add_attr_tab.num_handle, HRS_IDX_NB);
        }
        else
        {
            ESP_LOGI(GATTS_TABLE_TAG, "create attribute table successfully, the number handle = %d\n",
                     param->add_attr_tab.num_handle);
            memcpy(heart_rate_handle_table, param->add_attr_tab.handles, sizeof(heart_rate_handle_table));
            esp_ble_gatts_start_service(heart_rate_handle_table[IDX_SVC]); // 启动服务
        }
        break;

    // 其他未处理事件
    case ESP_GATTS_STOP_EVT:
    case ESP_GATTS_OPEN_EVT:
    case ESP_GATTS_CANCEL_OPEN_EVT:
    case ESP_GATTS_CLOSE_EVT:
    case ESP_GATTS_LISTEN_EVT:
    case ESP_GATTS_CONGEST_EVT:
    case ESP_GATTS_UNREG_EVT:
    case ESP_GATTS_DELETE_EVT:
    default:
        break;
    }
}

/* GATT服务器事件处理函数 - 处理所有GATT服务器事件 */
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    /* 如果是注册事件，存储每个配置文件的gatts_if */
    if (event == ESP_GATTS_REG_EVT)
    {
        if (param->reg.status == ESP_GATT_OK) // 注册成功
        {
            // 存储GATT接口ID到心率配置表中
            heart_rate_profile_tab[PROFILE_APP_IDX].gatts_if = gatts_if;
        }
        else // 注册失败
        {
            ESP_LOGE(GATTS_TABLE_TAG, "reg app failed, app_id %04x, status %d",
                     param->reg.app_id,
                     param->reg.status);
            return;
        }
    }

    /* 遍历所有配置文件，分发事件到对应的回调函数 */
    do
    {
        int idx;
        for (idx = 0; idx < PROFILE_NUM; idx++) // 遍历所有配置文件
        {
            /* 如果gatts_if未指定(ESP_GATT_IF_NONE)或匹配当前配置文件的gatts_if */
            if (gatts_if == ESP_GATT_IF_NONE || gatts_if == heart_rate_profile_tab[idx].gatts_if)
            {
                // 如果配置文件有回调函数，则调用它
                if (heart_rate_profile_tab[idx].gatts_cb)
                {
                    heart_rate_profile_tab[idx].gatts_cb(event, gatts_if, param);
                }
            }
        }
    } while (0); // 使用do-while(0)结构实现代码块
}


void BLUE_Init(void)
{
    // 释放经典蓝牙内存，仅使用BLE模式
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    /* 初始化蓝牙控制器 */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    /* 启用BLE模式蓝牙控制器 */
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    /* 初始化蓝牙协议栈 */
    ret = esp_bluedroid_init();
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "%s init bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    /* 启用蓝牙协议栈 */
    ret = esp_bluedroid_enable();
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    /* 注册GATT服务器回调函数 */
    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "gatts register error, error code = %x", ret);
        return;
    }

    /* 注册GAP回调函数 */
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "gap register error, error code = %x", ret);
        return;
    }

    /* 注册GATT应用程序 */
    ret = esp_ble_gatts_app_register(ESP_APP_ID);
    if (ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "gatts app register error, error code = %x", ret);
        return;
    }

    /* 设置本地MTU大小 */
    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(500);
    if (local_mtu_ret)
    {
        ESP_LOGE(GATTS_TABLE_TAG, "set local MTU failed, error code = %x", local_mtu_ret);
    }
    xCreatedEventGroup_BlueConnect = xEventGroupCreate();  // 创建事件组
}
#endif
