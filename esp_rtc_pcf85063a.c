/*
 * Driver ESP-IDF per l'RTC I2C NXP PCF85063A.
 *
 * Comportamento allineato al driver Linux ufficiale dello stesso chip,
 * drivers/rtc/rtc-pcf85063.c (Søren Andersen, Alexandre Belloni / Micro Crystal AG),
 * da cui sono ripresi la sequenza di set_time con blocco della catena di divisori,
 * il workaround per la corruzione dei registri dopo il power-on reset, la gestione
 * del registro Offset e la semantica dei flag OS/AF.
 *
 * Datasheet di riferimento:
 *   https://www.nxp.com/docs/en/data-sheet/PCF85063A.pdf  (Rev. 7 - 30 marzo 2018)
 */

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_rtc_pcf85063a.h"

static const char *TAG = "pcf85063a";

// Registri PCF85063A (NXP, datasheet rev.7)
#define PCF85063A_REG_CONTROL_1     0x00
#define PCF85063A_REG_CONTROL_2     0x01
#define PCF85063A_REG_OFFSET        0x02
#define PCF85063A_REG_RAM_BYTE      0x03
#define PCF85063A_REG_SECONDS       0x04
#define PCF85063A_REG_SECOND_ALARM  0x0B
#define PCF85063A_REG_MINUTE_ALARM  0x0C
#define PCF85063A_REG_HOUR_ALARM    0x0D
#define PCF85063A_REG_DAY_ALARM     0x0E
#define PCF85063A_REG_WEEKDAY_ALARM 0x0F

#define PCF85063A_CONTROL1_EXT_TEST_BIT (1 << 7)
#define PCF85063A_CONTROL1_STOP_BIT     (1 << 5)
#define PCF85063A_CONTROL1_CIE_BIT      (1 << 2)
#define PCF85063A_CONTROL1_12_24_BIT    (1 << 1) // 0 = 24 ore (forzato da questo driver)
#define PCF85063A_CONTROL1_CAP_SEL_BIT  (1 << 0) // 0 = 7 pF, 1 = 12.5 pF
#define PCF85063A_CONTROL1_SOFT_RESET   (0x58)   // 01011000b: bit6 + SR(bit4) + bit3, valore da datasheet

#define PCF85063A_CONTROL2_AIE_BIT  (1 << 7)
#define PCF85063A_CONTROL2_AF_BIT   (1 << 6)
#define PCF85063A_CONTROL2_COF_MASK (0x07)

#define PCF85063A_SECONDS_OS_BIT (1 << 7)
#define PCF85063A_SECONDS_MASK   (0x7F)
#define PCF85063A_MINUTES_MASK   (0x7F)
#define PCF85063A_HOURS_MASK     (0x3F) // modalita' 24h (forzata da questo driver)
#define PCF85063A_DAYS_MASK      (0x3F)
#define PCF85063A_WEEKDAYS_MASK  (0x07) // valore binario, non BCD
#define PCF85063A_MONTHS_MASK    (0x1F)

#define PCF85063A_OFFSET_MODE_BIT    (1 << 7) // 0 = normal (ogni 2 ore), 1 = course (ogni 4 minuti)
#define PCF85063A_OFFSET_VALUE_MASK  (0x7F)   // 7 bit in complemento a due
#define PCF85063A_OFFSET_SIGN_BIT    (6)
#define PCF85063A_OFFSET_STEP_NORMAL (4340)   // ppb per passo, modo normal
#define PCF85063A_OFFSET_STEP_COURSE (4069)   // ppb per passo, modo course

#define PCF85063A_ALARM_AEN_BIT      (1 << 7) // 0 = campo attivo nel confronto, 1 = campo ignorato
#define PCF85063A_ALARM_SEC_MIN_MASK (0x7F)
#define PCF85063A_ALARM_HOUR_MASK    (0x3F)
#define PCF85063A_ALARM_DAY_MASK     (0x3F)
#define PCF85063A_ALARM_WEEKDAY_MASK (0x07)

#define PCF85063A_I2C_TIMEOUT_MS (1000)

struct pcf85063a_dev_t {
    i2c_master_dev_handle_t i2c_dev;
    uint8_t control_1; ///< Valore di Control_1 applicato dal driver (24h + CAP_SEL), da riapplicare dopo un reset
};

static inline uint8_t bcd2dec(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10) + (bcd & 0x0F));
}

static inline uint8_t dec2bcd(uint8_t dec)
{
    return (uint8_t)(((dec / 10) << 4) | (dec % 10));
}

// Divisione intera con arrotondamento al valore piu' vicino; d sempre positivo
static inline int div_round_closest(int n, int d)
{
    return (n >= 0) ? ((n + d / 2) / d) : ((n - d / 2) / d);
}

static inline bool is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int month_days(int month /* 0-11 */, int year)
{
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 1 && is_leap_year(year)) {
        return 29;
    }
    return days[month];
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

/**
 * Read-modify-write su Control_2.
 *
 * Il flag AF ha semantica asimmetrica: scrivere 0 lo azzera, scrivere 1 non ha
 * effetto (datasheet, Table 9). Un RMW ingenuo riscriverebbe AF col valore letto,
 * perdendo un allarme scattato nella finestra tra lettura e scrittura: percio' se
 * AF non e' nella maschera del chiamante viene forzato a 1 ("nessun effetto").
 * L'unico chiamante che include AF nella maschera e' chi lo vuole azzerare.
 */
static esp_err_t pcf85063a_update_control_2(pcf85063a_handle_t handle, uint8_t mask, uint8_t value)
{
    uint8_t control2 = 0;
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_CONTROL_2, &control2, 1), TAG, "Read Control_2 failed");
    control2 = (uint8_t)((control2 & ~mask) | (value & mask));
    if (!(mask & PCF85063A_CONTROL2_AF_BIT)) {
        control2 |= PCF85063A_CONTROL2_AF_BIT;
    }
    return pcf85063a_write_reg8(handle, PCF85063A_REG_CONTROL_2, control2);
}

esp_err_t pcf85063a_new(i2c_master_bus_handle_t bus_handle, const pcf85063a_config_t *config, pcf85063a_handle_t *out_handle)
{
    esp_err_t ret = ESP_OK;
    pcf85063a_handle_t handle = NULL;

    ESP_RETURN_ON_FALSE(bus_handle && config && out_handle, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    // Scelta obbligatoria: una capacita' di carico sbagliata si manifesta solo come
    // deriva lenta dell'orologio, quindi e' meglio fallire subito che indovinare.
    ESP_RETURN_ON_FALSE(config->quartz_load == PCF85063A_QUARTZ_LOAD_7000FF ||
                        config->quartz_load == PCF85063A_QUARTZ_LOAD_12500FF,
                        ESP_ERR_INVALID_ARG, TAG,
                        "config->quartz_load non impostato: indicare esplicitamente "
                        "PCF85063A_QUARTZ_LOAD_7000FF o PCF85063A_QUARTZ_LOAD_12500FF "
                        "secondo il quarzo montato sulla board");

    handle = calloc(1, sizeof(struct pcf85063a_dev_t));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "No memory for device");

    // Modalita' 24 ore (bit 12_24 = 0), STOP/EXT_TEST/CIE azzerati, capacita' di
    // carico del quarzo secondo configurazione.
    handle->control_1 = (config->quartz_load == PCF85063A_QUARTZ_LOAD_12500FF) ? PCF85063A_CONTROL1_CAP_SEL_BIT : 0;

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063A_I2C_ADDRESS,
        .scl_speed_hz = config->scl_speed_hz ? config->scl_speed_hz : 400000,
    };
    ret = i2c_master_bus_add_device(bus_handle, &dev_config, &handle->i2c_dev);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "Failed to add I2C device");

    // Presence check non distruttivo: una lettura del registro Seconds, che serve
    // anche a leggere il flag OS.
    uint8_t seconds_reg = 0;
    ret = pcf85063a_read_reg(handle, PCF85063A_REG_SECONDS, &seconds_reg, 1);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "Chip non risponde su I2C");

    // Se OS e' attivo il chip ha perso alimentazione: il datasheet segnala che una
    // piccola percentuale di esemplari puo' avere i registri corrotti dopo il
    // power-on reset automatico, quindi si manda un software reset preventivo.
    // Un reset fallito non e' fatale: si prosegue comunque (come fa il driver Linux).
    if (seconds_reg & PCF85063A_SECONDS_OS_BIT) {
        ESP_LOGW(TAG, "Power loss rilevato (flag OS): software reset preventivo");
        esp_err_t reset_ret = pcf85063a_write_reg8(handle, PCF85063A_REG_CONTROL_1, PCF85063A_CONTROL1_SOFT_RESET);
        if (reset_ret != ESP_OK) {
            ESP_LOGW(TAG, "Software reset fallito (%s), si prosegue", esp_err_to_name(reset_ret));
        }
    }

    ret = pcf85063a_write_reg8(handle, PCF85063A_REG_CONTROL_1, handle->control_1);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "Failed to init Control_1");

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
    ESP_RETURN_ON_ERROR(pcf85063a_write_reg8(handle, PCF85063A_REG_CONTROL_1, PCF85063A_CONTROL1_SOFT_RESET),
                        TAG, "Software reset failed");
    // Il reset riporta Control_1 al default: riapplica la config del driver
    // (modalita' 24 ore + capacita' di carico del quarzo).
    return pcf85063a_write_reg8(handle, PCF85063A_REG_CONTROL_1, handle->control_1);
}

esp_err_t pcf85063a_get_time(pcf85063a_handle_t handle, struct tm *time)
{
    ESP_RETURN_ON_FALSE(handle && time, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");

    // Durante la lettura i registri data/ora sono congelati e non vengono
    // aggiornati fino alla fine dell'accesso: per non perdere un incremento dei
    // secondi il blocco Seconds(0x04)..Years(0x0A) va letto in una sola volta.
    uint8_t raw[7];
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_SECONDS, raw, sizeof(raw)), TAG, "Read time failed");

    memset(time, 0, sizeof(*time));
    time->tm_sec  = bcd2dec(raw[0] & PCF85063A_SECONDS_MASK);
    time->tm_min  = bcd2dec(raw[1] & PCF85063A_MINUTES_MASK);
    time->tm_hour = bcd2dec(raw[2] & PCF85063A_HOURS_MASK);
    time->tm_mday = bcd2dec(raw[3] & PCF85063A_DAYS_MASK);
    time->tm_wday = raw[4] & PCF85063A_WEEKDAYS_MASK; // valore binario, non BCD
    time->tm_mon  = bcd2dec(raw[5] & PCF85063A_MONTHS_MASK) - 1;
    time->tm_year = bcd2dec(raw[6]) + 2000 - 1900;
    time->tm_isdst = -1;

    // Se l'oscillatore si e' fermato l'ora appena letta non ha senso. La struct
    // resta popolata (utile per debug), ma il chiamante viene avvisato dal codice
    // di ritorno. Log a livello debug: get_time puo' essere chiamata a ripetizione
    // da una UI e un warning per chiamata sarebbe solo rumore.
    if (raw[0] & PCF85063A_SECONDS_OS_BIT) {
        ESP_LOGD(TAG, "Flag OS attivo: ora non affidabile");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t pcf85063a_set_time(pcf85063a_handle_t handle, const struct tm *time)
{
    ESP_RETURN_ON_FALSE(handle && time, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");

    int year = time->tm_year + 1900;
    ESP_RETURN_ON_FALSE(year >= 2000 && year <= 2099, ESP_ERR_INVALID_ARG, TAG, "Year out of range 2000-2099");
    ESP_RETURN_ON_FALSE(time->tm_mon >= 0 && time->tm_mon <= 11, ESP_ERR_INVALID_ARG, TAG, "Invalid month");
    ESP_RETURN_ON_FALSE(time->tm_mday >= 1 && time->tm_mday <= month_days(time->tm_mon, year),
                        ESP_ERR_INVALID_ARG, TAG, "Invalid day of month");
    ESP_RETURN_ON_FALSE(time->tm_hour >= 0 && time->tm_hour <= 23, ESP_ERR_INVALID_ARG, TAG, "Invalid hour");
    ESP_RETURN_ON_FALSE(time->tm_min >= 0 && time->tm_min <= 59, ESP_ERR_INVALID_ARG, TAG, "Invalid minute");
    ESP_RETURN_ON_FALSE(time->tm_sec >= 0 && time->tm_sec <= 59, ESP_ERR_INVALID_ARG, TAG, "Invalid second");
    ESP_RETURN_ON_FALSE(time->tm_wday >= 0 && time->tm_wday <= 6, ESP_ERR_INVALID_ARG, TAG, "Invalid weekday");

    uint8_t raw[7] = {
        // Il bit 7 dei secondi e' il flag OS: scriverlo a 0 lo azzera, dichiarando
        // l'ora affidabile. La maschera e' ridondante per tm_sec <= 59 ma rende
        // esplicito l'effetto.
        (uint8_t)(dec2bcd((uint8_t)time->tm_sec) & PCF85063A_SECONDS_MASK),
        dec2bcd((uint8_t)time->tm_min),
        dec2bcd((uint8_t)time->tm_hour),
        dec2bcd((uint8_t)time->tm_mday),
        (uint8_t)(time->tm_wday & PCF85063A_WEEKDAYS_MASK), // valore binario, non BCD
        dec2bcd((uint8_t)(time->tm_mon + 1)),
        dec2bcd((uint8_t)(year - 2000)),
    };

    // Per impostare l'ora con precisione si azzera la catena di divisori e la si
    // tiene bloccata (bit STOP) finche' tutti i registri data/ora sono scritti.
    // Scrivere il valore cached invece di un read-modify-write azzera anche
    // EXT_TEST, come fa il driver Linux.
    ESP_RETURN_ON_ERROR(pcf85063a_write_reg8(handle, PCF85063A_REG_CONTROL_1,
                                             handle->control_1 | PCF85063A_CONTROL1_STOP_BIT),
                        TAG, "Failed to stop clock");

    esp_err_t ret = pcf85063a_write_reg(handle, PCF85063A_REG_SECONDS, raw, sizeof(raw));

    // La catena di divisori va riavviata anche se la scrittura e' fallita,
    // altrimenti l'orologio resterebbe fermo.
    esp_err_t start_ret = pcf85063a_write_reg8(handle, PCF85063A_REG_CONTROL_1, handle->control_1);
    ESP_RETURN_ON_ERROR(ret, TAG, "Set time failed");
    ESP_RETURN_ON_ERROR(start_ret, TAG, "Failed to restart clock");

    return ESP_OK;
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
    // Read-modify-write sul registro Seconds: vedi il caveat documentato in header
    // (si perde la frazione di secondo, eventualmente un secondo intero).
    uint8_t seconds_reg = 0;
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_SECONDS, &seconds_reg, 1), TAG, "Read seconds failed");
    seconds_reg &= (uint8_t)~PCF85063A_SECONDS_OS_BIT;
    return pcf85063a_write_reg8(handle, PCF85063A_REG_SECONDS, seconds_reg);
}

esp_err_t pcf85063a_set_clkout(pcf85063a_handle_t handle, pcf85063a_clkout_freq_t freq)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(freq <= PCF85063A_CLKOUT_OFF, ESP_ERR_INVALID_ARG, TAG, "Invalid CLKOUT frequency");
    return pcf85063a_update_control_2(handle, PCF85063A_CONTROL2_COF_MASK, (uint8_t)freq);
}

esp_err_t pcf85063a_get_clkout(pcf85063a_handle_t handle, pcf85063a_clkout_freq_t *freq)
{
    ESP_RETURN_ON_FALSE(handle && freq, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    uint8_t control2 = 0;
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_CONTROL_2, &control2, 1), TAG, "Read Control_2 failed");
    *freq = (pcf85063a_clkout_freq_t)(control2 & PCF85063A_CONTROL2_COF_MASK);
    return ESP_OK;
}

esp_err_t pcf85063a_set_offset(pcf85063a_handle_t handle, int offset_ppb)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(offset_ppb >= PCF85063A_OFFSET_PPB_MIN && offset_ppb <= PCF85063A_OFFSET_PPB_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "Offset out of range");

    // Si calcolano i passi nei due modi e si tiene quello che approssima meglio il
    // valore richiesto, purche' rappresentabile nei 7 bit del registro.
    int steps_normal = div_round_closest(offset_ppb, PCF85063A_OFFSET_STEP_NORMAL);
    int steps_course = div_round_closest(offset_ppb, PCF85063A_OFFSET_STEP_COURSE);
    int error_normal = abs(offset_ppb - steps_normal * PCF85063A_OFFSET_STEP_NORMAL);
    int error_course = abs(offset_ppb - steps_course * PCF85063A_OFFSET_STEP_COURSE);

    uint8_t reg;
    if (steps_course > 63 || steps_course < -64 || error_normal < error_course) {
        reg = (uint8_t)steps_normal & PCF85063A_OFFSET_VALUE_MASK;
    } else {
        reg = (uint8_t)((uint8_t)steps_course & PCF85063A_OFFSET_VALUE_MASK) | PCF85063A_OFFSET_MODE_BIT;
    }

    return pcf85063a_write_reg8(handle, PCF85063A_REG_OFFSET, reg);
}

esp_err_t pcf85063a_get_offset(pcf85063a_handle_t handle, int *offset_ppb)
{
    ESP_RETURN_ON_FALSE(handle && offset_ppb, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");

    uint8_t reg = 0;
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_OFFSET, &reg, 1), TAG, "Read Offset failed");

    // Valore a 7 bit in complemento a due, bit di segno = bit 6
    int steps = reg & PCF85063A_OFFSET_VALUE_MASK;
    if (steps & (1 << PCF85063A_OFFSET_SIGN_BIT)) {
        steps -= (1 << (PCF85063A_OFFSET_SIGN_BIT + 1));
    }

    *offset_ppb = steps * ((reg & PCF85063A_OFFSET_MODE_BIT) ? PCF85063A_OFFSET_STEP_COURSE
                                                             : PCF85063A_OFFSET_STEP_NORMAL);
    return ESP_OK;
}

// AEN_x=1 (bit7) -> campo ignorato nel confronto; AEN_x=0 -> campo attivo
// (polarita' invertita rispetto al nome)
static uint8_t pcf85063a_encode_alarm_field(int8_t value, uint8_t mask, bool bcd)
{
    if (value < 0) {
        return PCF85063A_ALARM_AEN_BIT;
    }
    return (uint8_t)((bcd ? dec2bcd((uint8_t)value) : (uint8_t)value) & mask);
}

static int8_t pcf85063a_decode_alarm_field(uint8_t reg, uint8_t mask, bool bcd)
{
    if (reg & PCF85063A_ALARM_AEN_BIT) {
        return PCF85063A_ALARM_DISABLED;
    }
    uint8_t raw = reg & mask;
    return (int8_t)(bcd ? bcd2dec(raw) : raw);
}

esp_err_t pcf85063a_set_alarm(pcf85063a_handle_t handle, const pcf85063a_alarm_t *alarm)
{
    ESP_RETURN_ON_FALSE(handle && alarm, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    ESP_RETURN_ON_FALSE(alarm->second  < 60, ESP_ERR_INVALID_ARG, TAG, "Invalid alarm second");
    ESP_RETURN_ON_FALSE(alarm->minute  < 60, ESP_ERR_INVALID_ARG, TAG, "Invalid alarm minute");
    ESP_RETURN_ON_FALSE(alarm->hour    < 24, ESP_ERR_INVALID_ARG, TAG, "Invalid alarm hour");
    ESP_RETURN_ON_FALSE(alarm->day     < 32 && alarm->day != 0, ESP_ERR_INVALID_ARG, TAG, "Invalid alarm day");
    ESP_RETURN_ON_FALSE(alarm->weekday <  7, ESP_ERR_INVALID_ARG, TAG, "Invalid alarm weekday");

    uint8_t raw[5] = {
        pcf85063a_encode_alarm_field(alarm->second,  PCF85063A_ALARM_SEC_MIN_MASK, true),
        pcf85063a_encode_alarm_field(alarm->minute,  PCF85063A_ALARM_SEC_MIN_MASK, true),
        pcf85063a_encode_alarm_field(alarm->hour,    PCF85063A_ALARM_HOUR_MASK,    true),
        pcf85063a_encode_alarm_field(alarm->day,     PCF85063A_ALARM_DAY_MASK,     true),
        pcf85063a_encode_alarm_field(alarm->weekday, PCF85063A_ALARM_WEEKDAY_MASK, false),
    };

    // Si memorizza lo stato di AIE per ripristinarlo al termine, poi si disabilita
    // l'interrupt e si azzera AF: durante la riscrittura dei registri i valori sono
    // momentaneamente incoerenti e un match parziale genererebbe un allarme spurio.
    uint8_t control2 = 0;
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_CONTROL_2, &control2, 1), TAG, "Read Control_2 failed");
    bool interrupt_was_enabled = (control2 & PCF85063A_CONTROL2_AIE_BIT) != 0;

    ESP_RETURN_ON_ERROR(pcf85063a_update_control_2(handle, PCF85063A_CONTROL2_AIE_BIT | PCF85063A_CONTROL2_AF_BIT, 0),
                        TAG, "Failed to disable alarm interrupt");

    // I 5 registri di allarme (0x0B..0x0F) sono contigui: una sola transazione I2C
    ESP_RETURN_ON_ERROR(pcf85063a_write_reg(handle, PCF85063A_REG_SECOND_ALARM, raw, sizeof(raw)),
                        TAG, "Write alarm registers failed");

    if (interrupt_was_enabled) {
        ESP_RETURN_ON_ERROR(pcf85063a_update_control_2(handle, PCF85063A_CONTROL2_AIE_BIT, PCF85063A_CONTROL2_AIE_BIT),
                            TAG, "Failed to restore alarm interrupt");
    }

    return ESP_OK;
}

esp_err_t pcf85063a_get_alarm(pcf85063a_handle_t handle, pcf85063a_alarm_t *alarm, bool *interrupt_enabled)
{
    ESP_RETURN_ON_FALSE(handle && alarm, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");

    uint8_t raw[5];
    ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_SECOND_ALARM, raw, sizeof(raw)),
                        TAG, "Read alarm registers failed");

    alarm->second  = pcf85063a_decode_alarm_field(raw[0], PCF85063A_ALARM_SEC_MIN_MASK, true);
    alarm->minute  = pcf85063a_decode_alarm_field(raw[1], PCF85063A_ALARM_SEC_MIN_MASK, true);
    alarm->hour    = pcf85063a_decode_alarm_field(raw[2], PCF85063A_ALARM_HOUR_MASK,    true);
    alarm->day     = pcf85063a_decode_alarm_field(raw[3], PCF85063A_ALARM_DAY_MASK,     true);
    alarm->weekday = pcf85063a_decode_alarm_field(raw[4], PCF85063A_ALARM_WEEKDAY_MASK, false);

    if (interrupt_enabled) {
        uint8_t control2 = 0;
        ESP_RETURN_ON_ERROR(pcf85063a_read_reg(handle, PCF85063A_REG_CONTROL_2, &control2, 1), TAG, "Read Control_2 failed");
        *interrupt_enabled = (control2 & PCF85063A_CONTROL2_AIE_BIT) != 0;
    }

    return ESP_OK;
}

esp_err_t pcf85063a_enable_alarm_interrupt(pcf85063a_handle_t handle, bool enable)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    return pcf85063a_update_control_2(handle, PCF85063A_CONTROL2_AIE_BIT,
                                      enable ? PCF85063A_CONTROL2_AIE_BIT : 0);
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
    // Scrivere 0 su AF lo azzera; scrivere 1 lo lascia invariato (vedi datasheet Table 9)
    return pcf85063a_update_control_2(handle, PCF85063A_CONTROL2_AF_BIT, 0);
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
