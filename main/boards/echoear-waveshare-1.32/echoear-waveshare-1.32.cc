#include "application.h"
#include "button.h"
#include "backlight.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "wifi_board.h"

#include "echoear_display.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_touch_cst820.h"
#include <esp_lvgl_port.h>
#include "lvgl.h"
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>

#define TAG "echoear_waveshare_1_32"

static const co5300_lcd_init_cmd_t lcd_init_cmds[] = {
    // Vendor unlock and analog setup from the official Waveshare 1.32 BSP.
    {0xFE, (uint8_t[]) {0x20}, 1, 0},
    {0x19, (uint8_t[]) {0x10}, 1, 0},
    {0x1C, (uint8_t[]) {0xA0}, 1, 0},
    {0xFE, (uint8_t[]) {0x00}, 1, 0},
    {0xC4, (uint8_t[]) {0x80}, 1, 0},
    {0x3A, (uint8_t[]) {0x55}, 1, 0},
    {0x35, (uint8_t[]) {0x00}, 1, 0},
    {0x53, (uint8_t[]) {0x20}, 1, 0},
    {0x51, (uint8_t[]) {0xFF}, 1, 0},
    {0x63, (uint8_t[]) {0xFF}, 1, 0},
    {0x2A, (uint8_t[]) {0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]) {0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, nullptr, 0, 600},
    {0x29, nullptr, 0, 0},
};

class EchoEarLcdDisplay : public EchoEarDisplay {
public:
    EchoEarLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                      esp_lcd_panel_handle_t panel_handle,
                      int width, int height, int offset_x, int offset_y,
                      bool mirror_x, bool mirror_y, bool swap_xy)
        : EchoEarDisplay(io_handle, panel_handle, width, height, offset_x,
                         offset_y, mirror_x, mirror_y, swap_xy) {
        // Panel orientation is configured by lvgl_port from DISPLAY_MIRROR_X/Y.
        // Do not write MADCTL directly here: it would override the driver's setting.
    }

    // EchoEarDisplay::SetupUI() builds the actual EchoEar home screen, settings,
    // pet panel and local apps. It also installs the CO5300 even-area rounder.
};

class EchoEarBacklight : public Backlight {
public:
    explicit EchoEarBacklight(esp_lcd_panel_io_handle_t panel_io)
        : panel_io_(panel_io) {}

protected:
    void SetBrightnessImpl(uint8_t brightness) override {
        auto* display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        const uint8_t value = static_cast<uint8_t>((255U * brightness) / 100U);
        const uint32_t command = (0x51U << 8) | (0x02U << 24);
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_, command,
                                                   &value, sizeof(value)));
    }

private:
    esp_lcd_panel_io_handle_t panel_io_;
};

class EchoEarWaveshare132Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_{};
    Button boot_button_{BOOT_BUTTON_GPIO};
    Button pwr_button_{PWR_BUTTON_GPIO};
    esp_lcd_panel_handle_t panel_handle_{};
    esp_lcd_panel_io_handle_t io_handle_{};
    EchoEarLcdDisplay* display_{};
    EchoEarBacklight* backlight_{};
    esp_lcd_touch_handle_t touch_{};
    lv_indev_t* touch_indev_{};

    void InitializeI2c() {
        i2c_master_bus_config_t config{};
        config.i2c_port = I2C_NUM_0;
        config.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        config.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        config.clk_source = I2C_CLK_SRC_DEFAULT;
        config.glitch_ignore_cnt = 7;
        config.trans_queue_depth = 0;
        config.flags.enable_internal_pullup = true;
        ESP_ERROR_CHECK(i2c_new_master_bus(&config, &i2c_bus_));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        pwr_button_.OnLongPress([this]() {
            GetDisplay()->SetChatMessage("system", "OFF");
            vTaskDelay(pdMS_TO_TICKS(1000));
            gpio_set_level(PWR_EN_GPIO, 0);
        });
    }

    void InitializeSpi() {
        spi_bus_config_t config{};
        config.data0_io_num = LCD_D0;
        config.data1_io_num = LCD_D1;
        config.sclk_io_num = LCD_PCLK;
        config.data2_io_num = LCD_D2;
        config.data3_io_num = LCD_D3;
        config.max_transfer_sz = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * sizeof(uint16_t);
        config.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &config, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_spi_config_t io_config{};
        io_config.cs_gpio_num = LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 8;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io_handle_));

        co5300_vendor_config_t vendor_config{};
        vendor_config.init_cmds = lcd_init_cmds;
        vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
        vendor_config.flags.use_qspi_interface = true;

        esp_lcd_panel_dev_config_t panel_config{};
        panel_config.reset_gpio_num = LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = &vendor_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(io_handle_, &panel_config, &panel_handle_));
        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle_, 0x06, 0x00));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle_));

        display_ = new EchoEarLcdDisplay(io_handle_, panel_handle_,
            EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new EchoEarBacklight(io_handle_);
        backlight_->RestoreBrightness();
    }

    void InitializeTouch() {
        const esp_lcd_touch_config_t touch_config = {
            .x_max = EXAMPLE_LCD_H_RES,
            .y_max = EXAMPLE_LCD_V_RES,
            .rst_gpio_num = GPIO_NUM_7,
            .int_gpio_num = GPIO_NUM_6,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = false,
                .mirror_x = true,
                .mirror_y = true,
            },
        };
        esp_lcd_panel_io_handle_t touch_io = nullptr;

        // ESP-IDF 6.1 reordered this structure's members. Avoid the
        // CST820 component macro, whose designated initializer follows
        // the older layout and is rejected by the C++ compiler.
        esp_lcd_panel_io_i2c_config_t io_config{};
        io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST820_ADDRESS;
        io_config.scl_speed_hz = 400 * 1000;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 0;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 0;
        io_config.flags.dc_low_on_data = 0;
        io_config.flags.disable_control_phase = 1;

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &touch_io));
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst820(touch_io, &touch_config, &touch_));
        const lvgl_port_touch_cfg_t lvgl_touch_config = {
            .disp = lv_display_get_default(),
            .handle = touch_,
        };
        touch_indev_ = lvgl_port_add_touch(&lvgl_touch_config);
        ESP_ERROR_CHECK(touch_indev_ ? ESP_OK : ESP_FAIL);
    }

    void InitializePower() {
        gpio_config_t config{};
        config.intr_type = GPIO_INTR_DISABLE;
        config.mode = GPIO_MODE_OUTPUT;
        config.pin_bit_mask = 1ULL << PWR_EN_GPIO;
        config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        config.pull_up_en = GPIO_PULLUP_ENABLE;
        ESP_ERROR_CHECK(gpio_config(&config));
        ESP_ERROR_CHECK(gpio_set_level(PWR_EN_GPIO, 1));
        while (!gpio_get_level(PWR_BUTTON_GPIO)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

public:
    EchoEarWaveshare132Board() {
        InitializePower();
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeTouch();
        InitializeButtons();
    }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec codec(i2c_bus_, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &codec;
    }

    Display* GetDisplay() override {
        return display_;
    }

    Backlight* GetBacklight() override {
        return backlight_;
    }
};

DECLARE_BOARD(EchoEarWaveshare132Board);
