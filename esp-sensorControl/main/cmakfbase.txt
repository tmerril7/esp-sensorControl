idf_component_register(SRCS "firebase.c"
                    INCLUDE_DIRS "include"
                    REQUIRES "mbedtls" "esp_http_client" "json" "esp-tls")
