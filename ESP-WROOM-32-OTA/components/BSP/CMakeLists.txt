set(src_dirs
            LED
            LCD
            WIFI
            RTOS_DEMO
            IIC
            UART
            HTTP
            MQTT
            BLUE
            KEY
            YMODEM
            OTA_STM32
            )

set(include_dirs
            LED
            LCD
            WIFI
            RTOS_DEMO
            IIC
            UART
            HTTP
            MQTT
            BLUE
            KEY
            YMODEM
            OTA_STM32
            )

set(requires
            main
            esp_http_client
            nvs_flash
            esp_partition
            esp_rom
            json
            mbedtls
            )

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires} EMBED_TXTFILES ${project_dir}/server_certs/root.crt)
component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)
