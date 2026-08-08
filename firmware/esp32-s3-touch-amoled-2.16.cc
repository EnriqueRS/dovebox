#include "wifi_board.h"
#include "display/lcd_display.h"
#include "esp_lcd_co5300.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_timer.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include "esp_io_expander_tca9554.h"
#include "settings.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>

#include <esp_lcd_touch_cst9217.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#define TAG "WaveshareEsp32s3TouchAMOLED2inch16"

// Factory del dashboard DoveBox (Pieza B, definido en dovebox_dashboard.cc)
extern "C" void* dovebox_dashboard_create(void* display, void* screen);
// Notificación de emoción del servidor → la cara reacciona (Opción B)
extern "C" void dovebox_dashboard_on_emotion(const char* emotion);
// Notificación de mensaje de chat (user/assistant) → label de estado en la cara
extern "C" void dovebox_dashboard_on_chat_message(const char* role, const char* content);
// Evento del giroscopio (QMI8658): "shake" / "bottom_up" / "top_up" → la cara
// reacciona (sobresalto / dormirse / despertar)
extern "C" void dovebox_dashboard_on_imu(const char* event);

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);  // hold 4s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x01);

        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x08); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

// QMI8658: IMU de 6 ejes (acelerómetro + giroscopio) de la placa. Driver
// mínimo sobre el bus I2C existente (mismo I2C_NUM_0 que PMIC/touch/códec).
// Registros clave (datasheet QMI8658A + driver de Waveshare/SensorLib):
//   0x00 WHO_AM_I = 0x05 · 0x60 RESET = 0xB0 · 0x4D RST_RESULT = 0x80
//   0x02 CTRL1 bit6 = ADDR_AI (auto-increment) → 0x40
//   0x03 CTRL2 bits[6:4]=aFS (±4g=001) bits[3:0]=AODR (500Hz=0100) → 0x14
//   0x08 CTRL7 bit0=A_EN bit1=G_EN → 0x03
//   0x35..0x3A = AX_L..AZ_H (int16 little-endian, 4096 LSB/g a ±4g)
class Imu8658 : public I2cDevice {
public:
    Imu8658(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {}

    bool Init() {
        if (ReadReg(0x00) != 0x05) return false;   // WHO_AM_I
        WriteReg(0x60, 0xB0);                      // soft reset
        vTaskDelay(pdMS_TO_TICKS(20));
        WriteReg(0x02, 0x40);                      // ADDR_AI (ráfaga de lectura)
        WriteReg(0x03, 0x14);                      // accel ±4g @ 500Hz
        WriteReg(0x08, 0x03);                      // accel + gyro enable
        return true;
    }

    // Aceleración en g (convención: +g hacia abajo)
    bool ReadAccel(float& ax, float& ay, float& az) {
        uint8_t buf[6];
        ReadRegs(0x35, buf, 6);
        int16_t rx = (int16_t)((buf[1] << 8) | buf[0]);
        int16_t ry = (int16_t)((buf[3] << 8) | buf[2]);
        int16_t rz = (int16_t)((buf[5] << 8) | buf[4]);
        ax = rx / 4096.0f;
        ay = ry / 4096.0f;
        az = rz / 4096.0f;
        return true;
    }
};

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600}, // Sleep out

    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x36, (uint8_t[]){0xA0}, 1, 0},
    {0x29, (uint8_t[]){0x00}, 0, 600},
};

// 在waveshare_amoled_1_75类之前添加新的显示类
class CustomLcdDisplay : public SpiLcdDisplay {
public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t* )lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        uint16_t y1 = area->y1;
        uint16_t y2 = area->y2;

        // round the start of coordinate down to the nearest 2M number
        area->x1 = (x1 >> 1) << 1;
        area->y1 = (y1 >> 1) << 1;
        // round the end of coordinate up to the nearest 2N+1 number
        area->x2 = ((x2 >> 1) << 1) + 1;
        area->y2 = ((y2 >> 1) << 1) + 1;
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width,
                     int height,
                     int offset_x,
                     int offset_y,
                     bool mirror_x,
                     bool mirror_y,
                     bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle,
                        width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES*  0.1, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES*  0.1, 0);
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

        // Pieza B: panel DoveBox (5 vistas swipe, polling al agregador)
        dovebox_dashboard_create(this, lv_screen_active());
    }

    // Opción B: las emociones del servidor (llm emotion) van al dashboard para
    // que la cara de DoveBox las represente. El SetEmotion() base sigue
    // ejecutándose (emoji stock) pero la vista Face tapa la zona central.
    virtual void SetEmotion(const char* emotion) override {
        dovebox_dashboard_on_emotion(emotion);
        LcdDisplay::SetEmotion(emotion);
    }

    // Mensajes de chat (stt user / llm assistant) → label de estado de la cara
    // ("escuchando…", texto transcrito, respuesta del asistente).
    virtual void SetChatMessage(const char* role, const char* content) override {
        dovebox_dashboard_on_chat_message(role, content);
        LcdDisplay::SetChatMessage(role, content);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255*  brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED2inch16 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    esp_io_expander_handle_t io_expander = NULL;
    PowerSaveTimer* power_save_timer_;

    // IMU (QMI8658): acelerómetro + giroscopio
    Imu8658* imu_ = nullptr;
    bool imu_ok_ = false;
    float imu_baseline_az_ = 1.0f;  // gravedad Z de reposo (g)
    bool imu_baseline_set_ = false;
    int imu_shake_hits_ = 0;
    int64_t imu_shake_window_ms_ = 0;
    int64_t imu_last_event_ms_ = 0;

    void InitializeImu() {
        // QMI8658 a 0x6B (ADDR=L, default), fallback 0x6A (ADDR=H)
        const uint8_t kAddrs[] = {0x6B, 0x6A};
        for (uint8_t addr : kAddrs) {
            auto* imu = new Imu8658(i2c_bus_, addr);
            if (imu->Init()) {
                imu_ = imu;
                imu_ok_ = true;
                ESP_LOGI(TAG, "QMI8658 init OK (addr 0x%02X)", addr);
                break;
            }
            delete imu;
        }
        if (!imu_ok_) {
            ESP_LOGE(TAG, "QMI8658 no encontrado — reacciones de giroscopio desactivadas");
            return;
        }
        xTaskCreate(ImuTask, "dovebox_imu", 4096, this, 5, nullptr);
        ESP_LOGI(TAG, "IMU task creado (shake / boca abajo → dashboard)");
    }

    // Lee el acelerómetro (~25 Hz) y detecta gestos: shake (3 picos de
    // magnitud en 1.5s) y orientación (boca abajo = gravedad Z con signo
    // opuesto al reposo). Los eventos van al dashboard vía
    // dovebox_dashboard_on_imu() — el dashboard los aplica en el hilo LVGL.
    void ImuLoop() {
        if (!imu_ || !imu_ok_) return;
        float ax, ay, az;
        if (!imu_->ReadAccel(ax, ay, az)) return;

        float mag = sqrtf(ax * ax + ay * ay + az * az);
        int64_t now_ms = esp_timer_get_time() / 1000;

        // Orientación: el eje dominante es Z cuando el dispositivo está
        // apoyado plano. El signo de la gravedad en Z define cara arriba/abajo.
        if (!imu_baseline_set_ && mag > 0.7f && mag < 1.3f) {
            imu_baseline_az_ = az;
            imu_baseline_set_ = true;
        }
        if (imu_baseline_set_ && fabsf(az) > 0.6f &&
            fabsf(az) > fabsf(ax) && fabsf(az) > fabsf(ay)) {
            if (now_ms - imu_last_event_ms_ > 2000 && az * imu_baseline_az_ < 0.0f) {
                imu_last_event_ms_ = now_ms;
                if (az < 0.0f) {
                    ESP_LOGI(TAG, "IMU: boca abajo → a dormir");
                    dovebox_dashboard_on_imu("bottom_up");
                } else {
                    ESP_LOGI(TAG, "IMU: boca arriba → despierto");
                    dovebox_dashboard_on_imu("top_up");
                }
            }
        }

        // Shake: picos de |magnitud − 1g| > 0.9g, 3 en 1.5s
        float dev = fabsf(mag - 1.0f);
        if (dev > 0.9f) {
            if (imu_shake_window_ms_ == 0) imu_shake_window_ms_ = now_ms;
            imu_shake_hits_++;
        } else if (now_ms - imu_shake_window_ms_ > 1500) {
            imu_shake_hits_ = 0;
            imu_shake_window_ms_ = 0;
        }
        if (imu_shake_hits_ >= 3 && now_ms - imu_last_event_ms_ > 2000) {
            imu_shake_hits_ = 0;
            imu_shake_window_ms_ = 0;
            imu_last_event_ms_ = now_ms;
            ESP_LOGI(TAG, "IMU: shake → sobresalto");
            dovebox_dashboard_on_imu("shake");
        }
    }

    static void ImuTask(void* arg) {
        auto* self = static_cast<WaveshareEsp32s3TouchAMOLED2inch16*>(arg);
        while (true) {
            self->ImuLoop();
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20); });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness(); });
        power_save_timer_->OnShutdownRequest([this](){ 
            pmic_->PowerOff(); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, I2C_ADDRESS, &io_expander);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");
        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT);
        ESP_ERROR_CHECK(ret);
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
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

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void* )&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = PIN_NUM_TOUCH_RST,
            .int_gpio_num = PIN_NUM_TOUCH_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
        tp_io_config.scl_speed_hz = 400*  1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    // 初始化工具
    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });
    }

public:
    WaveshareEsp32s3TouchAMOLED2inch16() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_75
        InitializeTca9554();
#endif
        InitializeAxp2101();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeImu();
        InitializeButtons();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging)
        {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    // Despierta el hardware: sale del power-save (pantalla) y resetea el
    // timer de reposo de 60s. Lo invoca el dashboard en cualquier toque
    // (dovebox_board_wake_screen) para que un tap despierto al DoveBox.
    void WakeScreen() {
        power_save_timer_->WakeUp();
        GetDisplay()->SetPowerSaveMode(false);
        GetBacklight()->RestoreBrightness();
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED2inch16);

// Wake del hardware desde el dashboard (tap del usuario). Definido aquí para
// no exponer la clase completa; el dashboard lo declara extern "C".
extern "C" void dovebox_board_wake_screen(void) {
    auto& board = Board::GetInstance();
    auto* wb = dynamic_cast<WaveshareEsp32s3TouchAMOLED2inch16*>(&board);
    if (wb) wb->WakeScreen();
}
