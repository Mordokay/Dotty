#pragma once

// Pin map for the Waveshare ESP32-S3-Touch-ePaper-1.54 (V2).
// Source: waveshareteam/ESP32-S3-ePaper-1.54, 02_Example/Arduino/*/user_config.h

// e-Paper (SPI, write-only)
#define PIN_EPD_SCK   12
#define PIN_EPD_MOSI  13
#define PIN_EPD_CS    11
#define PIN_EPD_DC    10
#define PIN_EPD_RST   9
#define PIN_EPD_BUSY  8   // HIGH = busy

// Power switches
#define PIN_EPD_PWR   6   // LOW = e-paper powered
#define PIN_AUDIO_PWR 42  // LOW = audio codec/amp AND touch controller powered
#define PIN_VBAT_PWR  17  // HIGH = hold system power on battery (soft power latch)

// Buttons (pull-up, LOW = pressed)
#define PIN_BTN_BOOT  0
#define PIN_BTN_PWR   18

// Status LED
#define PIN_LED       3

// Battery voltage: ADC1 channel 3, 1:2 divider
#define PIN_VBAT_ADC  4

// I2C bus: SHTC3 (0x70), PCF85063 RTC (0x51), FT6336 touch, ES8311 codec
#define PIN_I2C_SDA   47
#define PIN_I2C_SCL   48
#define PIN_TP_INT    21
#define PIN_TP_RST    7

#define I2C_ADDR_ES8311 0x18
#define I2C_ADDR_FT6336 0x38

// Audio: ES8311 codec over I2S, NS4150-class amplifier enable
// Source: 08_Audio_Test/src/codec_board/board_cfg.h ("S3_ePaper_1_54")
#define PIN_I2S_MCLK  14
#define PIN_I2S_BCLK  15
#define PIN_I2S_WS    38
#define PIN_I2S_DOUT  45  // ESP32 -> codec DAC
#define PIN_I2S_DIN   16  // codec ADC (mic) -> ESP32
#define PIN_PA_EN     46  // HIGH = speaker amplifier on

// TF card (SDMMC, 1-bit)
#define PIN_SD_CLK    39
#define PIN_SD_CMD    41
#define PIN_SD_D0     40
