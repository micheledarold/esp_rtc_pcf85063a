#include <string.h>
#include "esp_check.h"
#include "esp_rtc_pcf85063a.h"

static const char *TAG = "pcf85063a";

// Registri PCF85063A (NXP, datasheet rev.7)
#define PCF85063A_REG_CONTROL_1     0x00
#define PCF85063A_REG_CONTROL_2     0x01
#define PCF85063A_REG_RAM_BYTE      0x03
#define PCF85063A_REG_SECONDS       0x04
#define PCF85063A_REG_SECOND_ALARM  0x0B
#define PCF85063A_REG_MINUTE_ALARM  0x0C
#define PCF85063A_REG_HOUR_ALARM    0x0D
#define PCF85063A_REG_DAY_ALARM     0x0E
#define PCF85063A_REG_WEEKDAY_ALARM 0x0F

#define PCF85063A_CONTROL1_SOFT_RESET (0x58) // 01011000b: bit SR + bit6/3 come da procedura datasheet

#define PCF85063A_CONTROL2_AIE_BIT  (1 << 7)
#define PCF85063A_CONTROL2_AF_BIT   (1 << 6)
#define PCF85063A_CONTROL2_COF_MASK (0x07)

#define PCF85063A_SECONDS_OS_BIT (1 << 7)
#define PCF85063A_SECONDS_MASK   (0x7F)
#define PCF85063A_MINUTES_MASK   (0x7F)
#define PCF85063A_HOURS_MASK     (0x3F) // modalita' 24h (forzata da questo driver)
#define PCF85063A_DAYS_MASK      (0x3F)
#define PCF85063A_WEEKDAYS_MASK  (0x07)
#define PCF85063A_MONTHS_MASK    (0x1F)

#define PCF85063A_ALARM_AEN_BIT      (1 << 7) // 0 = campo attivo nel confronto, 1 = campo ignorato
#define PCF85063A_ALARM_SEC_MIN_MASK (0x7F)
#define PCF85063A_ALARM_HOUR_MASK    (0x3F)
#define PCF85063A_ALARM_DAY_MASK     (0x3F)
#define PCF85063A_ALARM_WEEKDAY_MASK (0x07)

#define PCF85063A_I2C_TIMEOUT_MS (1000)

struct pcf85063a_dev_t {
    i2c_master_dev_handle_t i2c_dev;
};

static inline uint8_t bcd2dec(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10) + (bcd & 0x0F));
}

static inline uint8_t dec2bcd(uint8_t dec)
{
    return (uint8_t)(((dec / 10) << 4) | (dec % 10));
}

static esp_err_t pcf85063a_read_reg(pcf85063a_handle_t handle, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(handle->i2c_dev, &reg, 1, data, len, PCF85063A_I2C_TIMEOUT_MS);
}

static esp_err_t pcf85063a_write_reg(pcf85063a_handle_t handle, uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[1 + 7]; // il blocco piu' lungo scritto da questo driver e' Seconds..Years (7 byte)
    ESP_RETURN_ON_FALSE(len <= sizeof(buf) - 1, ESP_ERR_INVALID_ARG, TAG, "Write block too long");
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(handle->i2c_dev, buf, len + 1, PCF85063A_I2C_TIMEOUT_MS);
}

static esp_err_t pcf85063a_write_reg8(pcf85063a_handle_t handle, uint8_t reg, uint8_t value)
{
    return pcf85063a_write_reg(handle, reg, &value, 1);
}

esp_err_t pcf85063a_new(i2c_master_bus_handle_t bus_handle, const pcf85063a_config_t *config, pcf85063a_handle_t *out_handle)
{
    esp_err_t ret = ESP_OK;
    pcf85063a_handle_t handle = NULL;

    ESP_RETURN_ON_FALSE(bus_handle && config && out_handle, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");

    handle = calloc(1, sizeof(struct pcf85063a_dev_t));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "No memory for device");

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063A_I2C_ADDRESS,
        .scl_speed_hz = config->scl_speed_hz ? config->scl_speed_hz : 400000,
    };
    ret = i2c_master_bus_add_device(bus_handle, &dev_config, &handle->i2c_dev);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "Failed to add I2C device");

    // Forza sempre la modalita' 24 ore: get_time/set_time assumono questo formato.
    // Azzera anche STOP/EXT_TEST/CIE nel caso il chip fosse gia' stato configurato.
    ret = pcf85063a_write_reg8(handle, PCF85063A_REG_CONTROL_1, 0x00);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "Failed to init Control_1 (chip non risponde su I2C?)");

    *out_handle = handle;
    return ESP_OK;

err:
    if (handle) {
        if (handle->i2c_dev) {
            i2c_master_bus_rm_device(handle->i2c_dev);
        }
        free(handle);
    }
    return ret;
}

esp_err_t pcf85063a_del(pcf85063a_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    esp_err_t ret = i2c_master_bus_rm_device(handle->i2c_dev);
    free(handle);
    return ret;
}

esp_err_t pcf85063a_reset(pcf85063a_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    return pcf85063a_write_reg8(handle, PCF85063A_REG_CONTROL_1, PCF85063A_CONTROL1_SOFT_RESET);
}

esp_err_t pcf85063a_get_time(pcf85063a_handle_t handle, struct tm *time)
{
    ESP_RETURN_ON_FALSE(handle && time, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");

    // Blocco contiguo Seconds(0x04)..Years(0x0A)
    uint8_t raw[7];
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_SECONDS, raw, sizeof(raw)), TAG, "Read time failed");

    memset(time, 0, sizeof(*time));
    time->tm_sec  = bcd2dec(raw[0] & PCF85063A_SECONDS_MASK);
    time->tm_min  = bcd2dec(raw[1] & PCF85063A_MINUTES_MASK);
    time->tm_hour = bcd2dec(raw[2] & PCF85063A_HOURS_MASK);
    time->tm_mday = bcd2dec(raw[3] & PCF85063A_DAYS_MASK);
    time->tm_wday = bcd2dec(raw[4] & PCF85063A_WEEKDAYS_MASK);
    time->tm_mon  = bcd2dec(raw[5] & PCF85063A_MONTHS_MASK) - 1;
    time->tm_year = bcd2dec(raw[6]) + 2000 - 1900;
    time->tm_isdst = -1;

    return ESP_OK;
}

esp_err_t pcf85063a_set_time(pcf85063a_handle_t handle, const struct tm *time)
{
    ESP_RETURN_ON_FALSE(handle && time, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");

    int year = time->tm_year + 1900;
    ESP_RETURN_ON_FALSE(year >= 2000 && year <= 2099, ESP_ERR_INVALID_ARG, TAG, "Year out of range 2000-2099");
    ESP_RETURN_ON_FALSE(time->tm_mon >= 0 && time->tm_mon <= 11, ESP_ERR_INVALID_ARG, TAG, "Invalid month");

    uint8_t raw[7] = {
        dec2bcd((uint8_t)time->tm_sec),
        dec2bcd((uint8_t)time->tm_min),
        dec2bcd((uint8_t)time->tm_hour),
        dec2bcd((uint8_t)time->tm_mday),
        dec2bcd((uint8_t)time->tm_wday),
        dec2bcd((uint8_t)(time->tm_mon + 1)),
        dec2bcd((uint8_t)(year - 2000)),
    };

    return pcf85063a_write_reg(handle, PCF85063A_REG_SECONDS, raw, sizeof(raw));
}

esp_err_t pcf85063a_is_oscillator_stopped(pcf85063a_handle_t handle, bool *stopped)
{
    ESP_RETURN_ON_FALSE(handle && stopped, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    uint8_t seconds_reg = 0;
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_SECONDS, &seconds_reg, 1), TAG, "Read seconds failed");
    *stopped = (seconds_reg & PCF85063A_SECONDS_OS_BIT) != 0;
    return ESP_OK;
}

esp_err_t pcf85063a_clear_oscillator_stop_flag(pcf85063a_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    uint8_t seconds_reg = 0;
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_SECONDS, &seconds_reg, 1), TAG, "Read seconds failed");
    seconds_reg &= (uint8_t)~PCF85063A_SECONDS_OS_BIT;
    return pcf85063a_write_reg8(handle, PCF85063A_REG_SECONDS, seconds_reg);
}

esp_err_t pcf85063a_set_clkout(pcf85063a_handle_t handle, pcf85063a_clkout_freq_t freq)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(freq <= PCF85063A_CLKOUT_OFF, ESP_ERR_INVALID_ARG, TAG, "Invalid CLKOUT frequency");

    uint8_t control2 = 0;
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_CONTROL_2, &control2, 1), TAG, "Read Control_2 failed");
    control2 = (uint8_t)((control2 & ~PCF85063A_CONTROL2_COF_MASK) | (freq & PCF85063A_CONTROL2_COF_MASK));
    return pcf85063a_write_reg8(handle, PCF85063A_REG_CONTROL_2, control2);
}

static esp_err_t pcf85063a_write_alarm_field(pcf85063a_handle_t handle, uint8_t reg, int8_t value, uint8_t mask)
{
    // AEN_x=1 (bit7) -> campo ignorato nel confronto; AEN_x=0 -> campo attivo (polarita' invertita rispetto al nome)
    uint8_t reg_val = (value < 0) ? PCF85063A_ALARM_AEN_BIT : (dec2bcd((uint8_t)value) & mask);
    return pcf85063a_write_reg8(handle, reg, reg_val);
}

esp_err_t pcf85063a_set_alarm(pcf85063a_handle_t handle, const pcf85063a_alarm_t *alarm)
{
    ESP_RETURN_ON_FALSE(handle && alarm, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");

    ESP_RETURN_ON_ERROR(pcf85063a_write_alarm_field(handle, PCF85063A_REG_SECOND_ALARM, alarm->second, PCF85063A_ALARM_SEC_MIN_MASK), TAG, "Set second alarm failed");
    ESP_RETURN_ON_ERROR(pcf85063a_write_alarm_field(handle, PCF85063A_REG_MINUTE_ALARM, alarm->minute, PCF85063A_ALARM_SEC_MIN_MASK), TAG, "Set minute alarm failed");
    ESP_RETURN_ON_ERROR(pcf85063a_write_alarm_field(handle, PCF85063A_REG_HOUR_ALARM, alarm->hour, PCF85063A_ALARM_HOUR_MASK), TAG, "Set hour alarm failed");
    ESP_RETURN_ON_ERROR(pcf85063a_write_alarm_field(handle, PCF85063A_REG_DAY_ALARM, alarm->day, PCF85063A_ALARM_DAY_MASK), TAG, "Set day alarm failed");
    ESP_RETURN_ON_ERROR(pcf85063a_write_alarm_field(handle, PCF85063A_REG_WEEKDAY_ALARM, alarm->weekday, PCF85063A_ALARM_WEEKDAY_MASK), TAG, "Set weekday alarm failed");

    return ESP_OK;
}

esp_err_t pcf85063a_enable_alarm_interrupt(pcf85063a_handle_t handle, bool enable)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    uint8_t control2 = 0;
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_CONTROL_2, &control2, 1), TAG, "Read Control_2 failed");
    if (enable) {
        control2 |= PCF85063A_CONTROL2_AIE_BIT;
    } else {
        control2 &= (uint8_t)~PCF85063A_CONTROL2_AIE_BIT;
    }
    return pcf85063a_write_reg8(handle, PCF85063A_REG_CONTROL_2, control2);
}

esp_err_t pcf85063a_get_alarm_flag(pcf85063a_handle_t handle, bool *triggered)
{
    ESP_RETURN_ON_FALSE(handle && triggered, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    uint8_t control2 = 0;
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_CONTROL_2, &control2, 1), TAG, "Read Control_2 failed");
    *triggered = (control2 & PCF85063A_CONTROL2_AF_BIT) != 0;
    return ESP_OK;
}

esp_err_t pcf85063a_clear_alarm_flag(pcf85063a_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    uint8_t control2 = 0;
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_CONTROL_2, &control2, 1), TAG, "Read Control_2 failed");
    // Scrivere 0 su AF lo azzera; scrivere 1 lo lascia invariato (vedi datasheet Table 9)
    control2 &= (uint8_t)~PCF85063A_CONTROL2_AF_BIT;
    return pcf85063a_write_reg8(handle, PCF85063A_REG_CONTROL_2, control2);
}

esp_err_t pcf85063a_read_ram_byte(pcf85063a_handle_t handle, uint8_t *value)
{
    ESP_RETURN_ON_FALSE(handle && value, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    return pcf85063a_read_reg(handle, PCF85063A_REG_RAM_BYTE, value, 1);
}

esp_err_t pcf85063a_write_ram_byte(pcf85063a_handle_t handle, uint8_t value)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    return pcf85063a_write_reg8(handle, PCF85063A_REG_RAM_BYTE, value);
}
