#include "MQTT.h"


esp_mqtt_client_handle_t client;
bool mqtt_connected = false;
double temp = 12.3;    // 温度值
double humi = 23.4;    // 湿度值
double gas = 34.5;    // 湿度值

void handle_event(const char *payload);


/* MQTT事件处理 */
void mqtt_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    // 获取事件数据
    esp_mqtt_event_handle_t event = event_data;
    int msg_id; //消息ID号

    printf("\n[MQTT事件] 收到MQTT事件: %d\n", (int)event_id);

    // 根据事件ID进行判断
    switch ((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:
            // MQTT连接成功
            printf("✓✓✓ [MQTT连接成功] 已连接到阿里云物联网平台! ✓✓✓\n");
            printf("[MQTT连接] client指针地址: %p\n", client);
            ESP_LOGI(MQTT_TAG, "MQTT Connected");

            // 订阅属性设置主题
            msg_id = esp_mqtt_client_subscribe(client, TOPIC_PROP_SET, 0);
            printf("[MQTT订阅] 正在订阅属性设置主题: %s\n", TOPIC_PROP_SET);
            printf("[MQTT订阅] 订阅消息ID: %d\n", msg_id);

            // 设置连接标志
            mqtt_connected = true;
            printf("[MQTT状态] mqtt_connected 已设置为 true!\n");
            break;

        case MQTT_EVENT_DISCONNECTED:
            // MQTT连接断开
            printf("✗✗✗ [MQTT断开] 与阿里云平台连接已断开! ✗✗✗\n");
            ESP_LOGI(MQTT_TAG, "MQTT Disconnected");
            mqtt_connected = false;
            break;

        case MQTT_EVENT_SUBSCRIBED: // MQTT订阅成功事件
            printf("✓ [MQTT订阅成功] 主题订阅成功! msg_id=%d\n", event->msg_id);
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            //publish_sensor_data(255, 650, 850);  // 发送测试数据：温度25.5℃, 湿度65.0%, 光照850lx
            
            break;

        case MQTT_EVENT_DATA: {
            // 处理平台下发数据
            // 获取主题
            char topic[event->topic_len + 1];
            memcpy(topic, event->topic, event->topic_len);
            topic[event->topic_len] = '\0';

            // 获取数据
            char data[event->data_len + 1];
            memcpy(data, event->data, event->data_len);
            data[event->data_len] = '\0';

            // 打印接收到的数据（串口0输出）
            printf("\n[MQTT接收] ★★★ 收到阿里云下发的消息 ★★★\n");
            printf("[MQTT接收] Topic(主题): %s\n", topic);
            printf("[MQTT接收] Data(数据): %s\n", data);
            ESP_LOGI(MQTT_TAG, "Received: Topic=%s, Data=%s", topic, data);

            //判断是否为LED事件主题
            if (strstr(event->topic, TOPIC_PROP_SET) != NULL) {
                cJSON *root = cJSON_Parse(event->data);
                cJSON *params = cJSON_GetObjectItem(root, "params");
                cJSON *led_status = cJSON_GetObjectItem(params, "LED");
                gpio_set_level(LED_GPIO_PIN, led_status->valueint ? 0 : 1);

                cJSON_Delete(root);
            }

            break;
        }

        case MQTT_EVENT_ERROR:
            // MQTT错误处理
            printf("✗ [MQTT错误] 发生错误!\n");
            ESP_LOGE(MQTT_TAG, "MQTT Error");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                // 打印传输错误信息
                printf("[MQTT错误详情] Transport error: %s\n",
                        strerror(event->error_handle->esp_transport_sock_errno));
                ESP_LOGE(MQTT_TAG, "Transport error: %s",
                        strerror(event->error_handle->esp_transport_sock_errno));
            }
            break;

        default:
            printf("[MQTT事件] 其他事件: %d\n", (int)event_id);
            break;
    }
}


/* 初始化MQTT客户端 */
void mqtt_app_init(void)
{
    printf("\n========================================\n");
    printf("  开始初始化MQTT客户端...\n");
    printf("========================================\n\n");


    // 定义MQTT客户端配置
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.hostname = HOST_NAME,                   /* MQTT地址 */
        .broker.address.port = HOST_PORT,                       /* MQTT端口号 */
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,    /* TCP模式 */
        .credentials.client_id = CLIENT_ID,                     /* 设备名称 */
        .credentials.username = (char*)USER_NAME,               /* 产品ID */
        .credentials.authentication.password = PASSWORD,        /* 计算出来的密码 */
        };

    // 初始化MQTT客户端
    printf("[MQTT初始化] 正在创建MQTT客户端实例...\n");
    client = esp_mqtt_client_init(&mqtt_cfg);

    if (client == NULL) {
        printf("✗ [MQTT错误] MQTT客户端创建失败!\n");
        return;
    }

    printf("✓ [MQTT初始化] MQTT客户端创建成功!\n");

    // 注册MQTT事件处理函数
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    printf("✓ [MQTT初始化] 事件处理函数已注册!\n");

    // 启动MQTT客户端
    esp_mqtt_client_start(client);
    printf("✓ [MQTT初始化] MQTT客户端已启动!\n");
    printf("✓ [MQTT初始化] 正在连接阿里云物联网平台...\n\n");
    ESP_LOGI(MQTT_TAG,"MQTT start");
}


/* 发布传感器数据 */
void publish_sensor_data(uint16_t ir, uint16_t ps, uint16_t als)
{
    printf("\n[数据发布] ════════════════════════════\n");
    printf("[数据发布] 开始发布传感器数据\n");
    printf("[数据发布] 参数值：IR=%d, ps=%d, als=%d\n", ir, ps, als);

    // 按照阿里云物模型格式构建 JSON 数据
    cJSON *root = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();

    // 添加传感器数据
    cJSON_AddNumberToObject(params, "IR", ir);  // 温度属性
    cJSON_AddNumberToObject(params, "ps", ps);     // 湿度属性
    cJSON_AddNumberToObject(params, "als", als); // 光照强度

    cJSON_AddItemToObject(root, "params", params);
    cJSON_AddStringToObject(root, "method", "thing.event.property.post");

    char *json_str = cJSON_PrintUnformatted(root);

    printf("[数据发布] Topic: %s\n", TOPIC_PROP_POST);
    printf("[数据发布] Payload: %s\n", json_str);
    printf("[数据发布] 客户端状态：client=%p, mqtt_connected=%s\n", 
           client, mqtt_connected ? "true" : "false");

    // 发布消息到阿里云物联网平台（QoS=1，确保消息到达）
    int msg_id = esp_mqtt_client_publish(client, TOPIC_PROP_POST, json_str, 0, 1, 0);

    if (msg_id > 0) {
        printf("✓ [数据发布成功] 消息 ID=%d\n", msg_id);
    } else {
        printf("✗ [数据发布失败] msg_id=%d\n", msg_id);
    }
    printf("[数据发布] ════════════════════════════\n\n");

    // 释放内存
    cJSON_Delete(root);
    free(json_str);
}



