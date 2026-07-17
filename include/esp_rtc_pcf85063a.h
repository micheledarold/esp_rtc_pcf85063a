#ifndef ESP_RTC_PCF85063A_H_
#define ESP_RTC_PCF85063A_H_

#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCF85063A_I2C_ADDRESS (0x51)

typedef struct pcf85063a_dev_t *pcf85063a_handle_t;

/**
 * @brief Configurazione per la creazione dell'istanza PCF85063A
 */
typedef struct {
    uint32_t scl_speed_hz; ///< Velocita' I2C per questo device (0 = usa il default 400000 Hz)
} pcf85063a_config_t;

#define PCF85063A_CONFIG_DEFAULT() \
    {                               \
        .scl_speed_hz = 400000,     \
    }

/**
 * @brief Crea una nuova istanza PCF85063A su un bus I2C gia' inizializzato
 *
 * @param[in] bus_handle Handle del bus I2C (puo' essere condiviso con altri device, es. il touch)
 * @param[in] config Configurazione dell'istanza
 * @param[out] out_handle Handle restituito
 */
esp_err_t pcf85063a_new(i2c_master_bus_handle_t bus_handle, const pcf85063a_config_t *config, pcf85063a_handle_t *out_handle);

/**
 * @brief Rilascia l'istanza e stacca il device dal bus I2C
 */
esp_err_t pcf85063a_del(pcf85063a_handle_t handle);

/**
 * @brief Esegue un software reset del chip (Control_1 = 0x58, come da datasheet)
 */
esp_err_t pcf85063a_reset(pcf85063a_handle_t handle);

/**
 * @brief Legge data/ora corrente
 *
 * tm_year e' anni dal 1900 (come da struct tm standard) e tm_mon e' 0-11: il chip
 * memorizza solo le ultime due cifre dell'anno (registro Years, 00-99) e il mese
 * come 1-12, quindi si assume sempre il secolo 2000-2099.
 */
esp_err_t pcf85063a_get_time(pcf85063a_handle_t handle, struct tm *time);

/**
 * @brief Imposta data/ora. time->tm_year deve rappresentare un anno tra 2000 e 2099.
 */
esp_err_t pcf85063a_set_time(pcf85063a_handle_t handle, const struct tm *time);

/**
 * @brief Legge il flag OS (Oscillator Stop)
 *
 * Se true, l'integrita' dell'ora non e' garantita (l'oscillatore si e' fermato o e'
 * stato interrotto almeno una volta dall'ultimo clear, tipicamente per mancanza di
 * alimentazione/batteria di backup). Va verificato dopo il power-on e, se true,
 * l'ora va reimpostata con pcf85063a_set_time() e il flag azzerato.
 */
esp_err_t pcf85063a_is_oscillator_stopped(pcf85063a_handle_t handle, bool *stopped);

/**
 * @brief Azzera il flag OS. Da chiamare dopo aver impostato un'ora valida.
 */
esp_err_t pcf85063a_clear_oscillator_stop_flag(pcf85063a_handle_t handle);

/**
 * @brief Frequenze disponibili per il pin CLKOUT (registro Control_2, campo COF)
 */
typedef enum {
    PCF85063A_CLKOUT_32768_HZ = 0,
    PCF85063A_CLKOUT_16384_HZ = 1,
    PCF85063A_CLKOUT_8192_HZ  = 2,
    PCF85063A_CLKOUT_4096_HZ  = 3,
    PCF85063A_CLKOUT_2048_HZ  = 4,
    PCF85063A_CLKOUT_1024_HZ  = 5,
    PCF85063A_CLKOUT_1_HZ     = 6,
    PCF85063A_CLKOUT_OFF      = 7,
} pcf85063a_clkout_freq_t;

/**
 * @brief Imposta la frequenza del pin CLKOUT (default dopo reset: 32768 Hz)
 */
esp_err_t pcf85063a_set_clkout(pcf85063a_handle_t handle, pcf85063a_clkout_freq_t freq);

/// Valore per un campo di pcf85063a_alarm_t non usato nella condizione di allarme
#define PCF85063A_ALARM_DISABLED (-1)

/**
 * @brief Configurazione allarme.
 *
 * Ogni campo puo' essere impostato a PCF85063A_ALARM_DISABLED per non partecipare
 * alla condizione di allarme (bit AEN_x corrispondente = 1, campo ignorato). Il
 * campo weekday usa la stessa convenzione 0-6 di tm_wday (0 = domenica).
 */
typedef struct {
    int8_t second;
    int8_t minute;
    int8_t hour;
    int8_t day;
    int8_t weekday;
} pcf85063a_alarm_t;

/**
 * @brief Programma l'allarme
 *
 * L'allarme scatta (bit AF) quando tutti i campi abilitati (AEN_x = 0) coincidono
 * con l'ora corrente. Non abilita l'interrupt sul pin INT: per quello usare
 * pcf85063a_enable_alarm_interrupt().
 */
esp_err_t pcf85063a_set_alarm(pcf85063a_handle_t handle, const pcf85063a_alarm_t *alarm);

/**
 * @brief Abilita/disabilita la generazione dell'interrupt su INT quando scatta l'allarme (bit AIE)
 */
esp_err_t pcf85063a_enable_alarm_interrupt(pcf85063a_handle_t handle, bool enable);

/**
 * @brief Legge il flag AF (Alarm Flag): true se l'allarme e' scattato
 */
esp_err_t pcf85063a_get_alarm_flag(pcf85063a_handle_t handle, bool *triggered);

/**
 * @brief Azzera il flag AF. L'allarme scattera' di nuovo solo al prossimo match.
 */
esp_err_t pcf85063a_clear_alarm_flag(pcf85063a_handle_t handle);

/**
 * @brief Legge il byte libero di RAM del chip (registro 0x03)
 *
 * Utile ad esempio per rilevare un primo avvio "a freddo" (batteria di backup mai
 * inserita) scrivendo un valore magico e verificandolo al boot successivo.
 */
esp_err_t pcf85063a_read_ram_byte(pcf85063a_handle_t handle, uint8_t *value);

/**
 * @brief Scrive il byte libero di RAM del chip (registro 0x03)
 */
esp_err_t pcf85063a_write_ram_byte(pcf85063a_handle_t handle, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* ESP_RTC_PCF85063A_H_ */
