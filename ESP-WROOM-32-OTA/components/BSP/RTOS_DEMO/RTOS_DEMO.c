#include "RTOS_DEMO.h"

QueueHandle_t xMQTTQueue = NULL;

typedef struct {
    uint16_t ir;
    uint16_t ps;
    uint16_t als;
} SensorData_t;

// void LED_Task(void *pvParameter) {
    
//     while (1) {
//         xEventGroupWaitBits(xCreatedEventGroup_WifiConnect, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
//         LED_TOGGLE(); // 翻转LED            
//         vTaskDelay(500); // 延时500毫秒
//     } 
// }


void uart2_send_task(void *pvParameter) {
	uint8_t i=0;
    uint8_t frame_data[] = {
            0x0A, 0x0B,  // 帧头
            1,2,3,4,  // 动态数据
            0x0C        // 帧尾
        };

    while (1) {
			i++;
			if(i>255) i=0;
			frame_data[2] = i;
			frame_data[3] = i+1;
			frame_data[4] = i+2;
			frame_data[5] = i+3;
			// 发送完整帧数据
			uart_write_bytes(UART_NUM_2, (const char*)frame_data, sizeof(frame_data));
			
			// 可选：打印发送信息以便调试
			 printf("发送数据帧: ");
			 for (int i = 0; i < sizeof(frame_data); i++) {
			 	printf("%02X ", frame_data[i]);
			 }
			 printf("\n");
		      
        // 延时1秒后再次发送
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void uart2_receive_task(void *pvParameter) 
{
	uint8_t rx_buf[128];
    while (1) {
        // 非阻塞读取数据
        int len = uart_read_bytes(UART_NUM_2, rx_buf, sizeof(rx_buf), 20 / portTICK_PERIOD_MS);
        
        // 处理接收到的字节
        for (int i = 0; i < len; i++) {
            process_rx_byte(rx_buf[i]);
        }
        
        // 检测超时
        check_timeout();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void vTaskHttpGet(void *pvParameters) {
    while (1) 
    {
        xEventGroupWaitBits(xCreatedEventGroup_WifiConnect, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        xEventGroupWaitBits(xCreatedEventGroup_BlueConnect, Blue_CONNECTED_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
        if (strlen(received_city) != 0) {


            // HTTP客户端配置
            esp_http_client_config_t config = {
                .url = dynamic_url,
                .event_handler = http_event_handler,
                .buffer_size = 2048, // 增大缓冲区避免分段
                .timeout_ms = 5000   // 设置超时时间
            };

            esp_http_client_handle_t client = esp_http_client_init(&config);
            esp_err_t err = esp_http_client_perform(client);

            // 校验响应状态
            if (err == ESP_OK) {
                int status_code = esp_http_client_get_status_code(client);
                if (status_code == 200) {
                    weather_parse(http_response_buffer); // 替换原有weather_get函数
                } else {
                    ESP_LOGE(TAG, "HTTP状态码异常: %d", status_code);
                }
            }

            // 清理资源
            esp_http_client_cleanup(client);
            free(http_response_buffer);
            http_response_buffer = NULL;
            http_response_len = 0;
        }
        vTaskDelay(500);
    }
}



void MQTT_Task(void *pvParameters) {
    SensorData_t received_data;
    int i=0;

 
    printf("  [MQTT 任务] 等待 MQTT 客户端初始化完成...\n");
    // 等待 MQTT 客户端创建完成
    vTaskDelay(pdMS_TO_TICKS(2000));

    printf("  [MQTT 任务] 开始发送测试数据到阿里云\n");
    printf("  [MQTT 任务] ========================================\n\n");

    uint16_t test_counter = 0;
    while(1) {
        // 生成模拟传感器数据（用于测试）
        test_counter++;
        received_data.ir = test_counter;   // 温度值，每次 +1
        received_data.ps = test_counter;    // 湿度值，每次 +1
        received_data.als = test_counter;   // 光照值，每次 +1
        i++;
        // 打印当前连接状态
        printf("[MQTT 状态] mqtt_connected = %s\n", mqtt_connected ? "true" : "false");

        // 检查 MQTT 是否已连接
        if (mqtt_connected) {
            printf("[MQTT 发送] 第%d次发送数据 -> IR:%d, ps:%d, als:%d\n",
                   test_counter, received_data.ir, received_data.ps, received_data.als);

            // 发送数据到阿里云物联网平台
            publish_sensor_data(received_data.ir, received_data.ps, received_data.als);

        } else {
            printf("[MQTT 等待] MQTT 尚未连接，等待中... (%d)\n", test_counter);
            
            // 即使没连接上也尝试发送一次（用于调试）
            printf("[MQTT 调试] 尝试直接调用 publish_sensor_data...\n");
            if (client != NULL) {
                printf("[MQTT 调试] client 指针有效，尝试发布...\n");
                publish_sensor_data(received_data.ir, received_data.ps, received_data.als);
            } else {
                printf("[MQTT 错误] client 为 NULL，无法发送!\n");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(3000));  // 每 3 秒发送一次数据
    }
}

// static void nimble_host_task(void *param)
// {
//     /* Task entry log */
//     ESP_LOGI(TAG, "nimble host task has been started!");

//     /* This function won't return until nimble_port_stop() is executed */
//     nimble_port_run();

//     /* Clean up at exit */
//     vTaskDelete(NULL);
// }

void Task_Create(void) {
    xCreatedEventGroup_WifiConnect = xEventGroupCreate();  // 创建事件组
    //xTaskCreate(LED_Task, "LED_Task", 2048, NULL, 7, NULL);  // 创建LED任务
    //xTaskCreate(MLX90640_Task, "MLX90640_Task", 48192, NULL, 5, NULL);
	//xTaskCreate(uart2_send_task, "uart2_send_task", 4096, NULL, 5, NULL);
	//xTaskCreate(uart2_receive_task, "uart2_receive_task", 4096, NULL, 6, NULL);
	xTaskCreate(MQTT_Task, "MQTT_Task", 8192, NULL, 7, NULL);  // ★★★ 启用MQTT任务 ★★★
	//xTaskCreate(vTaskHttpGet, "vTaskHttpGet", 8192, NULL, 8, NULL);  // 创建HTTP任务
//	xTaskCreate(nimble_host_task, "NimBLE Host", 4 * 1024, NULL, 5, NULL);
	//xTaskCreate(DeepSeek_Task, "DeepSeek_Task", 8192, NULL, 8, NULL);  // 创建DeepSeek任务
    //xTaskCreate(stm32_ota_task, "stm32_ota_task", 8192, NULL, 8, NULL);  // 创建STM32 OTA任务
}
