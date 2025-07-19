idf_component_register(SRCS "firebase.c"
                    INCLUDE_DIRS "include"
                    REQUIRES "mbedtls" "esp_http_client" "json" "esp-tls"
                    EMBED_TXTFILES FIREBASE_SYSTEM_KEY.pem G_ROOT_CA.pem GTS_ROOT_R4.pem)
