#include "HTTP.h"
#include "BLUE.h"


char dynamic_url[2048];
const char *TAG = "WIFI_HTTP";
// 事件标志组

// 全局响应缓冲区
char *http_response_buffer = NULL;
size_t http_response_len = 0;

// HTTP事件处理器
esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if(MQTT_OTA==1)
            {
                ESP_LOGI(MQTT_TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            }
            else
            {                            
                if (!esp_http_client_is_chunked_response(evt->client))
                {
                    http_response_buffer = realloc(http_response_buffer, http_response_len + evt->data_len + 1);
                    memcpy(http_response_buffer + http_response_len, evt->data, evt->data_len);
                    http_response_len += evt->data_len;
                    http_response_buffer[http_response_len] = '\0'; // 终止符// 数据分块接收处理
                }
            }

            break;

        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "HTTP错误");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(MQTT_TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(MQTT_TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(MQTT_TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(MQTT_TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(MQTT_TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        default:  // 处理未知事件类型
            break;
    }
    return ESP_OK;
}


void weather_parse(const char *response) {
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        ESP_LOGE(TAG, "JSON解析失败: %s", cJSON_GetErrorPtr());
        return;
    }

    /* 提取results数组 */
    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        ESP_LOGE(TAG, "无效的results结构");
        cJSON_Delete(root);
        return;
    }

    /* 获取第一条天气记录 */
    cJSON *result = cJSON_GetArrayItem(results, 0);
    
    /* 解析城市名称 */
    const char *city = "N/A";
    cJSON *location = cJSON_GetObjectItem(result, "location");
    if (location) {
        cJSON *name_item = cJSON_GetObjectItem(location, "name");
        city = (name_item && cJSON_IsString(name_item)) ? name_item->valuestring : "N/A";
    }

    /* 解析天气状况和温度 */
    const char *weather = "N/A";
    const char *temperature = "N/A";
    cJSON *now = cJSON_GetObjectItem(result, "now");
    if (now) {
        cJSON *text_item = cJSON_GetObjectItem(now, "text");
        weather = (text_item && cJSON_IsString(text_item)) ? text_item->valuestring : "N/A";
        
        cJSON *temp_item = cJSON_GetObjectItem(now, "temperature");
        temperature = (temp_item && cJSON_IsString(temp_item)) ? temp_item->valuestring : "N/A";
    }

    /* 解析更新时间 */
    char formatted_time[20] = "N/A";
    cJSON *update_item = cJSON_GetObjectItem(result, "last_update");
    if (update_item && cJSON_IsString(update_item)) {
        strncpy(formatted_time, update_item->valuestring, 16);
        formatted_time[10] = ' '; // 替换'T'为空格
        formatted_time[16] = '\0';
    }

    /* 格式化输出 */
    ESP_LOGI(TAG, "\n------天气信息------");
    ESP_LOGI(TAG, "城市：%s", city);
    ESP_LOGI(TAG, "天气：%s", weather);
    ESP_LOGI(TAG, "温度：%s℃", temperature);
    ESP_LOGI(TAG, "最后更新时间：%s", formatted_time);
    ESP_LOGI(TAG, "--------------------");

    cJSON_Delete(root);
}



#define DEEPSEEK_URL "https://api.deepseek.com/v1/chat/completions"
#define API_KEY "sk-e201d312132123312341231"  // 替换为实际密钥

// 流式响应处理缓冲区
static char *stream_buffer = NULL;
static size_t stream_capacity = 0;
static size_t stream_index = 0;

// 当前累积的回复内容（用于流式响应）
static char *content_buffer = NULL;
static size_t content_capacity = 0;
static size_t content_index = 0;

// 改进的事件处理器 - 流式版本
esp_err_t http_event_handler_stream(esp_http_client_event_t *e) {
    switch (e->event_id) {
        case HTTP_EVENT_ON_DATA: {
            // 动态扩展流缓冲区
            if (stream_index + e->data_len + 1 > stream_capacity) {
                size_t new_capacity = stream_capacity * 2;
                if (new_capacity < stream_index + e->data_len + 1) {
                    new_capacity = stream_index + e->data_len + 1024;
                }
                
                char *new_buffer = realloc(stream_buffer, new_capacity);
                if (!new_buffer) {
                    ESP_LOGE("HTTP", "流内存分配失败! 需要 %d 字节", new_capacity);
                    free(stream_buffer);
                    stream_buffer = NULL;
                    stream_capacity = 0;
                    stream_index = 0;
                    return ESP_FAIL;
                }
                
                stream_buffer = new_buffer;
                stream_capacity = new_capacity;
            }
            
            // 拷贝数据到流缓冲区
            memcpy(stream_buffer + stream_index, e->data, e->data_len);
            stream_index += e->data_len;
            stream_buffer[stream_index] = '\0'; // 确保字符串终止
            
            // 处理流式数据块
            char *line = stream_buffer;
            while (line < stream_buffer + stream_index) {
                // 查找行结束位置
                char *line_end = strstr(line, "\n");
                if (!line_end) break;
                
                // 提取单行数据
                *line_end = '\0'; // 临时终止字符串
                
                // 检查是否为有效数据行 (以 "data: " 开头)
                if (strncmp(line, "data: ", 6) == 0) {
                    char *json_str = line + 6;
                    
                    // 检查流结束标记
                    if (strcmp(json_str, "[DONE]") == 0) {
                        // 流结束，打印最终回复
                        if (content_buffer && content_index > 0) {
                            printf("\nDEEPSEEK回复: %s\n", content_buffer);
                            
                            // 释放内容缓冲区
                            free(content_buffer);
                            content_buffer = NULL;
                            content_capacity = 0;
                            content_index = 0;
                        }
                    } else {
                        // 解析JSON块
                        cJSON *chunk = cJSON_Parse(json_str);
                        if (chunk) {
                            cJSON *choices = cJSON_GetObjectItem(chunk, "choices");
                            if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                                cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
                                cJSON *delta = cJSON_GetObjectItem(first_choice, "delta");
                                if (delta) {
                                    cJSON *content = cJSON_GetObjectItem(delta, "content");
                                    if (cJSON_IsString(content)) {
                                        const char *text = content->valuestring;
                                        size_t text_len = strlen(text);
                                        
                                        // 扩展内容缓冲区
                                        if (content_index + text_len + 1 > content_capacity) {
                                            size_t new_capacity = content_capacity * 2;
                                            if (new_capacity < content_index + text_len + 1) {
                                                new_capacity = content_index + text_len + 1024;
                                            }
                                            
                                            char *new_buf = realloc(content_buffer, new_capacity);
                                            if (new_buf) {
                                                content_buffer = new_buf;
                                                content_capacity = new_capacity;
                                            }
                                        }
                                        
                                        // 追加内容
                                        if (content_buffer && content_capacity > content_index + text_len) {
                                            memcpy(content_buffer + content_index, text, text_len);
                                            content_index += text_len;
                                            content_buffer[content_index] = '\0';
                                            
                                            // 流式打印内容
                                            printf("%s", text);
                                            fflush(stdout); // 确保及时输出
                                        }
                                    }
                                }
                            }
                            cJSON_Delete(chunk);
                        } else {
                            ESP_LOGW("JSON", "流块解析失败: %s", json_str);
                        }
                    }
                }
                
                // 移动到下一行
                line = line_end + 1;
            }
            
            // 移动剩余数据到缓冲区开头
            size_t remaining = stream_buffer + stream_index - line;
            if (remaining > 0 && line > stream_buffer) {
                memmove(stream_buffer, line, remaining);
            }
            stream_index = remaining;
            break;
        }
            
        case HTTP_EVENT_ON_FINISH:
            // 清理流缓冲区
            if (stream_buffer) {
                free(stream_buffer);
                stream_buffer = NULL;
            }
            stream_capacity = 0;
            stream_index = 0;
            
            // 清理内容缓冲区
            if (content_buffer) {
                free(content_buffer);
                content_buffer = NULL;
            }
            content_capacity = 0;
            content_index = 0;
            break;
            
        case HTTP_EVENT_DISCONNECTED:
            // 清理流缓冲区
            if (stream_buffer) {
                free(stream_buffer);
                stream_buffer = NULL;
            }
            stream_capacity = 0;
            stream_index = 0;
            
            // 清理内容缓冲区
            if (content_buffer) {
                free(content_buffer);
                content_buffer = NULL;
            }
            content_capacity = 0;
            content_index = 0;
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

void call_deepseek_api(void) 
{
        // 1. 创建JSON请求体 - 启用流式
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", "deepseek-chat");
        
        // 添加流式参数
        cJSON_AddBoolToObject(root, "stream", true);
        
        // 创建消息数组
        cJSON *messages = cJSON_AddArrayToObject(root, "messages");
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", "请问英雄联盟S13的S赛总冠军是谁(只用回答是谁即可）!");
        cJSON_AddItemToArray(messages, msg);
        
        char *post_data = cJSON_PrintUnformatted(root);

        // 2. 配置HTTP客户端
        esp_http_client_config_t config = {
            .url = DEEPSEEK_URL,
            .method = HTTP_METHOD_POST,
            .event_handler = http_event_handler_stream,  // 使用流式处理器
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .buffer_size = 4096,
            .disable_auto_redirect = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);

        // 3. 设置请求头
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Authorization", "Bearer " API_KEY);
        esp_http_client_set_header(client, "Accept", "text/event-stream");  // 重要：声明接受事件流

        // 4. 发送请求
        printf("正在向DeepSeek发送请求...\n");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
        esp_err_t err = esp_http_client_perform(client);

        // 5. 检查响应
        if (err == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            printf("\nHTTP状态码: %d\n", status_code);
            
            if (status_code != 200) {
                char error_buf[512];
                int read_len = esp_http_client_read_response(client, error_buf, sizeof(error_buf) - 1);
                if (read_len > 0) {
                    error_buf[read_len] = '\0';
                    printf("错误详情: %s\n", error_buf);
                }
            }
        } else {
            printf("请求失败: %s\n", esp_err_to_name(err));
        }

        // 6. 清理资源
        esp_http_client_cleanup(client);
        cJSON_Delete(root);
        free(post_data);
        
        // 确保所有缓冲区已清理
        if (stream_buffer) {
            free(stream_buffer);
            stream_buffer = NULL;
        }
        stream_capacity = 0;
        stream_index = 0;
        
        if (content_buffer) {
            free(content_buffer);
            content_buffer = NULL;
        }
        content_capacity = 0;
        content_index = 0;
}