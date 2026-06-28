#ifndef LGFX_ILI9341_H
#define LGFX_ILI9341_H

#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_LCD.hpp>

/* SPI pins for external ILI9341 display (参考 ZX Spectrum / 外屏点亮参考项目)
 *
 * 接线 (与外屏点亮参考项目一致):
 *   ILI9341: CS  RST  DC  MOSI  SCK  LED
 *   Cardputer: G5  G3   G6  G14   G40  G4
 *   SD 卡:    CS→G12  MOSI→G14  SCK→G40  MISO→G39
 *   VCC -> 5V_OUT -> AMS1117-3.3 -> 3.3V
 *   GND -> GND
 *
 * 关键原理:
 *   1. G5/G6 接 ILI9341 的 CS/DC, 模块自带上拉电阻.
 *      M5GFX 检测时把 G5/G6 设为 INPUT_PULLDOWN 但读到 HIGH →
 *      误判为 CardputerADV → 键盘切到 TCA8418 I2C (G1/G2) →
 *      G3/G5/G6 从 IOMatrix 释放, 不会与外屏 SPI 冲突.
 *   2. SPI 共享 SPI3_HOST (HSPI), 与 SD 卡和内置 ST7789 共总线,
 *      通过 CS 区分. OPI PSRAM 占用了 SPI2, 不能再用 SPI2_HOST. */
#define EXT_PIN_SCK    40   // SCK  → G40
#define EXT_PIN_MOSI   14   // SD   → G14 (MOSI)
#define EXT_PIN_MISO   39   // SDO  → G39 (MISO, 暂不接但保留配置)
#define EXT_LCD_CS     5    // CS   → G5  (上拉, 触发 ADV 检测)
#define EXT_LCD_DC     6    // DC   → G6  (上拉, 触发 ADV 检测)
#define EXT_LCD_RST    3    // RESET→ G3
#define EXT_LCD_BL     4    // LED  → G4  (PWM 背光)

struct Panel_ILI9341_Local : public lgfx::v1::Panel_LCD {
    Panel_ILI9341_Local(void) {
        _cfg.memory_width  = _cfg.panel_width  = 240;
        _cfg.memory_height = _cfg.panel_height = 320;
    }

protected:
    static constexpr uint8_t CMD_PWCTR1  = 0xC0;
    static constexpr uint8_t CMD_PWCTR2  = 0xC1;
    static constexpr uint8_t CMD_VMCTR1  = 0xC5;
    static constexpr uint8_t CMD_VMCTR2  = 0xC7;
    static constexpr uint8_t CMD_FRMCTR1 = 0xB1;
    static constexpr uint8_t CMD_DFUNCTR = 0xB6;
    static constexpr uint8_t CMD_GMCTRP1 = 0xE0;
    static constexpr uint8_t CMD_GMCTRN1 = 0xE1;
    static constexpr uint8_t CMD_PIXFMT  = 0x3A;

    const uint8_t* getInitCommands(uint8_t listno) const override {
        static constexpr uint8_t list0[] = {
            CMD_PWCTR1,  1, 0x23,
            CMD_PWCTR2,  1, 0x10,
            CMD_VMCTR1,  2, 0x3E, 0x28,
            CMD_VMCTR2,  1, 0x86,
            CMD_PIXFMT,  1, 0x55,       // 16-bit RGB565
            CMD_FRMCTR1, 2, 0x00, 0x18, // 79Hz frame rate
            CMD_DFUNCTR, 3, 0x08, 0x82, 0x27,
            CMD_GMCTRP1,15, 0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,0x37,0x07,0x10,0x03,0x0E,0x09,0x00,
            CMD_GMCTRN1,15, 0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F,
            CMD_SLPOUT , 0 + CMD_INIT_DELAY, 120,
            CMD_IDMOFF , 0,
            CMD_DISPON , 0 + CMD_INIT_DELAY, 100,
            0xFF, 0xFF,
        };
        switch (listno) {
        case 0: return list0;
        default: return nullptr;
        }
    }
};

class LGFX_ILI9341 : public lgfx::v1::LGFX_Device {
    Panel_ILI9341_Local panel;
    lgfx::v1::Bus_SPI bus;
    lgfx::Light_PWM light;

public:
    LGFX_ILI9341() {
        auto b = bus.config();
        b.spi_host   = SPI3_HOST;        // HSPI: 与 SD 卡和内置屏共总线
        b.spi_mode   = 0;
        b.freq_write = 40000000;         // 40 MHz
        b.freq_read  = 16000000;
        b.spi_3wire  = true;
        b.use_lock   = true;
        b.dma_channel = 1;

        b.pin_sclk = EXT_PIN_SCK;        // G40
        b.pin_mosi = EXT_PIN_MOSI;       // G14
        b.pin_miso = EXT_PIN_MISO;       // G39
        b.pin_dc   = EXT_LCD_DC;         // G6

        bus.config(b);
        panel.setBus(&bus);

        auto p = panel.config();
        p.pin_cs    = EXT_LCD_CS;        // G5
        p.pin_rst   = EXT_LCD_RST;       // G3
        p.bus_shared = true;             // 与 SD 卡共享 SPI3
        p.readable   = false;
        p.invert     = false;
        p.rgb_order  = false;
        p.dlen_16bit = false;
        p.memory_width  = 240;
        p.memory_height = 320;
        p.panel_width   = 240;
        p.panel_height  = 320;
        p.offset_x = 0;
        p.offset_y = 0;
        p.offset_rotation = 4;
        p.dummy_read_pixel = 8;
        p.dummy_read_bits = 1;
        panel.config(p);

        auto l = light.config();
        l.pin_bl = EXT_LCD_BL;          // G4
        l.invert = false;               // HIGH = ON
        l.freq = 12000;                 // 10 ~ 20kHz
        l.pwm_channel = 6;              // 使用通道6, 避免与内置背光(通道7)冲突
        light.config(l);
        panel.setLight(&light);

        setPanel(&panel);
    }
};

extern LGFX_ILI9341 externalDisplay;

// SD 访问前调用, 确保外屏释放 SPI 总线
inline void lcd_quiesce() {
    externalDisplay.endWrite();
    externalDisplay.waitDisplay();
    digitalWrite(EXT_LCD_CS, HIGH);
}

#endif // LGFX_ILI9341_H
