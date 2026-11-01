idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES lora_basics_modem nvs_flash
)
