/*
 * The two things on this board that look like software bugs until you read the
 * schematic:
 *
 *  - The speaker amplifier (NS4150B) has its CTRL pin on EXIO7 with a 10K
 *    pulldown, so it powers up muted. Every layer above can report success -
 *    codec configured, I2S clocking at the right rate, writes accepted - and
 *    still produce silence, because the last analogue stage is switched off.
 *
 *  - On battery, VBAT only reaches VSYS while the power button is physically
 *    held, unless firmware raises SYS_EN (EXIO6) to latch the path on. Letting
 *    go of the button before that happens simply powers the board down.
 */
#include "expander.h"
#include "driver/i2c_master.h"
#include "board_bsp.h"
#include "codec_init.h"
#include "esp_log.h"

static const char *TAG = "expander";

#define TCA9554_ADDR   0x20
#define REG_OUTPUT     0x01
#define REG_CONFIG     0x03   /* 1 = input, 0 = output */

static esp_err_t rd(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

static esp_err_t wr(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, 2, pdMS_TO_TICKS(100));
}

esp_err_t expander_set(int pin, bool high)
{
    if (pin < 0 || pin > 7) return ESP_ERR_INVALID_ARG;

    bool borrowed = false;
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)get_i2c_bus_handle(0);
    if (!bus) {
        /* Called before the codec exists - stand up the bus just for this. */
        i2c_master_bus_config_t cfg = {
            .clk_source                   = I2C_CLK_SRC_DEFAULT,
            .i2c_port                     = I2C_NUM_0,
            .scl_io_num                   = BSP_PIN_SYS_SCL,
            .sda_io_num                   = BSP_PIN_SYS_SDA,
            .glitch_ignore_cnt            = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
            ESP_LOGE(TAG, "no i2c bus for EXIO%d", pin);
            return ESP_FAIL;
        }
        borrowed = true;
    }

    i2c_master_dev_handle_t dev;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TCA9554_ADDR,
        .scl_speed_hz    = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &dev);
    if (err != ESP_OK) goto done;

    /* Read-modify-write both registers: the other pins carry interrupts and
     * the backlight enable, and stamping over them would break unrelated
     * things in ways that are miserable to track down. */
    uint8_t cfg_reg = 0xFF, out_reg = 0x00;
    if ((err = rd(dev, REG_CONFIG, &cfg_reg)) != ESP_OK) goto rm;
    if ((err = rd(dev, REG_OUTPUT, &out_reg)) != ESP_OK) goto rm;

    if (high) out_reg |=  (1 << pin);
    else      out_reg &= ~(1 << pin);
    cfg_reg &= ~(1 << pin);                   /* 0 = output */

    if ((err = wr(dev, REG_OUTPUT, out_reg)) != ESP_OK) goto rm;
    if ((err = wr(dev, REG_CONFIG, cfg_reg)) != ESP_OK) goto rm;

    ESP_LOGI(TAG, "EXIO%d -> %s (cfg=0x%02x out=0x%02x)",
             pin, high ? "high" : "low", cfg_reg, out_reg);

rm:
    i2c_master_bus_rm_device(dev);
done:
    if (borrowed) i2c_del_master_bus(bus);
    if (err != ESP_OK) ESP_LOGE(TAG, "EXIO%d failed: %s", pin, esp_err_to_name(err));
    return err;
}
