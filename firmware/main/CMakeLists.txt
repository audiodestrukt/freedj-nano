idf_component_register(
    SRCS app.cpp audio.cpp tracks.cpp inputs.cpp midi_uart.cpp display_u8g2.cpp display_tft.cpp
    INCLUDE_DIRS .
    REQUIRES nano esp_driver_i2s esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_pcnt
             esp_driver_uart esp_adc esp_lcd esp_partition esp_timer spi_flash)
