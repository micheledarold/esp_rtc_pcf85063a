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
 * @brief Capacita' di carico del quarzo (bit CAP_SEL del registro Control_1)
 *
 * Deve corrispondere al quarzo montato sulla board: un valore sbagliato non
 * impedisce il funzionamento ma introduce una deriva sistematica dell'orologio,
 * difficile da diagnosticare a posteriori.
 *
 * Per questo la scelta e' obbligatoria ed esplicita: pcf85063a_new() rifiuta
 * PCF85063A_QUARTZ_LOAD_UNSPECIFIED, cosi' una config azzerata o un campo
 * dimenticato falliscono subito e in modo evidente invece di ricadere in silenzio
 * su un valore arbitrario. Il quarzo da 7 pF e' il caso piu' comune ed e' il
 * default del chip, ma va comunque dichiarato.
 */
typedef enum {
    PCF85063A_QUARTZ_LOAD_UNSPECIFIED = 0, ///< Scelta non effettuata: rifiutata da pcf85063a_new()
    PCF85063A_QUARTZ_LOAD_7000FF,          ///< 7 pF (default del chip)
    PCF85063A_QUARTZ_LOAD_12500FF,         ///< 12.5 pF
} pcf85063a_quartz_load_t;

/**
 * @brief Configurazione per la creazione dell'istanza PCF85063A
 */
typedef struct {
    uint32_t scl_speed_hz;                ///< Velocita' I2C per questo device (0 = usa il default 400000 Hz)
    pcf85063a_quartz_load_t quartz_load;  ///< Capacita' di carico del quarzo (obbligatoria)
} pcf85063a_config_t;

/**
 * @brief Valori di default per pcf85063a_config_t
 *
 * @note Lascia deliberatamente @c quartz_load non impostato: l'applicazione deve
 *       valorizzarlo in base al quarzo della board, altrimenti pcf85063a_new()
 *       fallisce con ESP_ERR_INVALID_ARG. Per un quarzo standard da 7 pF:
 *       @code
 *       pcf85063a_config_t cfg = PCF85063A_CONFIG_DEFAULT();
 *       cfg.quartz_load = PCF85063A_QUARTZ_LOAD_7000FF;
 *       @endcode
 */
#define PCF85063A_CONFIG_DEFAULT()                          \
    {                                                       \
        .scl_speed_hz = 400000,                             \
        .quartz_load  = PCF85063A_QUARTZ_LOAD_UNSPECIFIED,  \
    }

/**
 * @brief Crea una nuova istanza PCF85063A su un bus I2C gia' inizializzato
 *
 * @c config->quartz_load e' obbligatorio: se vale
 * PCF85063A_QUARTZ_LOAD_UNSPECIFIED la funzione ritorna ESP_ERR_INVALID_ARG.
 *
 * Verifica la presenza del chip con una lettura (non distruttiva) del registro
 * Seconds. Se il flag OS risulta attivo esegue un software reset preventivo: il
 * datasheet del PCF85063A segnala che una piccola percentuale di esemplari puo'
 * avere i registri corrotti dopo il power-on reset automatico. Applica poi la
 * configurazione del driver su Control_1 (modalita' 24 ore, STOP/EXT_TEST/CIE
 * azzerati, CAP_SEL secondo config->quartz_load).
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
 *
 * Il reset riporta tutti i registri ai valori di default, quindi al termine il
 * driver riapplica la propria configurazione di Control_1 (24 ore + CAP_SEL).
 * Data/ora e allarme vanno riprogrammati dopo la chiamata.
 */
esp_err_t pcf85063a_reset(pcf85063a_handle_t handle);

/**
 * @brief Legge data/ora corrente
 *
 * tm_year e' anni dal 1900 (come da struct tm standard) e tm_mon e' 0-11: il chip
 * memorizza solo le ultime due cifre dell'anno (registro Years, 00-99) e il mese
 * come 1-12, quindi si assume sempre il secolo 2000-2099.
 *
 * @return
 *      - ESP_OK: ora valida
 *      - ESP_ERR_INVALID_STATE: il flag OS e' attivo, l'oscillatore si e' fermato
 *        almeno una volta e il contenuto di @p time NON e' affidabile. La struct
 *        viene comunque popolata con quanto letto dai registri. Va impostata una
 *        nuova ora con pcf85063a_set_time().
 *      - altro: errore di comunicazione I2C
 */
esp_err_t pcf85063a_get_time(pcf85063a_handle_t handle, struct tm *time);

/**
 * @brief Imposta data/ora. time->tm_year deve rappresentare un anno tra 2000 e 2099.
 *
 * La scrittura del blocco data/ora avviene con la catena di divisori bloccata
 * (bit STOP di Control_1), come da procedura consigliata: azzera il prescaler,
 * quindi l'ora impostata e' precisa al secondo, ed evita che un incremento dei
 * secondi cada a meta' scrittura del blocco.
 *
 * Azzera implicitamente anche il flag OS, percio' dopo un set_time riuscito non
 * serve chiamare pcf85063a_clear_oscillator_stop_flag().
 *
 * Tutti i campi vengono validati: tm_sec 0-59, tm_min 0-59, tm_hour 0-23,
 * tm_mday valido per il mese/anno indicati, tm_mon 0-11, tm_wday 0-6.
 */
esp_err_t pcf85063a_set_time(pcf85063a_handle_t handle, const struct tm *time);

/**
 * @brief Legge il flag OS (Oscillator Stop)
 *
 * Se true, l'integrita' dell'ora non e' garantita (l'oscillatore si e' fermato o e'
 * stato interrotto almeno una volta dall'ultimo clear, tipicamente per mancanza di
 * alimentazione/batteria di backup). Va verificato dopo il power-on e, se true,
 * l'ora va reimpostata con pcf85063a_set_time().
 *
 * Lo stesso stato e' segnalato dal valore di ritorno di pcf85063a_get_time().
 */
esp_err_t pcf85063a_is_oscillator_stopped(pcf85063a_handle_t handle, bool *stopped);

/**
 * @brief Azzera il flag OS senza toccare l'ora impostata
 *
 * @note Il flag OS vive nel registro Seconds, quindi l'operazione e' un
 *       read-modify-write su quel registro: si perde la frazione di secondo in
 *       corso e, se il registro incrementa tra la lettura e la scrittura, si
 *       perde un secondo pieno. La via preferibile e' pcf85063a_set_time(), che
 *       azzera OS come effetto collaterale scrivendo un'ora nota. Questa
 *       funzione serve solo quando si vuole dichiarare affidabile l'ora gia'
 *       presente nei registri.
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

/**
 * @brief Legge la frequenza attualmente impostata sul pin CLKOUT
 */
esp_err_t pcf85063a_get_clkout(pcf85063a_handle_t handle, pcf85063a_clkout_freq_t *freq);

/**
 * @brief Estremi ammessi per pcf85063a_set_offset(), in parti per miliardo
 *
 * Corrispondono a -64 e +63 passi del modo "normal" (4340 ppb per passo).
 */
#define PCF85063A_OFFSET_PPB_MIN (-277760)
#define PCF85063A_OFFSET_PPB_MAX (273420)

/**
 * @brief Imposta la correzione della deriva dell'orologio (registro Offset)
 *
 * @param[in] offset_ppb Correzione in parti per miliardo, tra
 *            PCF85063A_OFFSET_PPB_MIN e PCF85063A_OFFSET_PPB_MAX. Valore positivo
 *            se l'orologio e' lento, negativo se e' veloce.
 *
 * Il chip offre due modi di correzione, con passo di quantizzazione diverso:
 * "normal" (correzione applicata ogni 2 ore, passo 4340 ppb) e "course"
 * (ogni 4 minuti, passo 4069 ppb). Il driver sceglie automaticamente il modo che
 * approssima meglio il valore richiesto, percio' l'offset riletto con
 * pcf85063a_get_offset() e' il multiplo del passo piu' vicino a @p offset_ppb,
 * non necessariamente il valore esatto.
 */
esp_err_t pcf85063a_set_offset(pcf85063a_handle_t handle, int offset_ppb);

/**
 * @brief Legge la correzione della deriva attualmente programmata, in ppb
 */
esp_err_t pcf85063a_get_offset(pcf85063a_handle_t handle, int *offset_ppb);

/// Valore per un campo di pcf85063a_alarm_t non usato nella condizione di allarme
#define PCF85063A_ALARM_DISABLED (-1)

/**
 * @brief Configurazione allarme.
 *
 * Ogni campo puo' essere impostato a PCF85063A_ALARM_DISABLED per non partecipare
 * alla condizione di allarme (bit AEN_x corrispondente = 1, campo ignorato). Il
 * campo weekday usa la stessa convenzione 0-6 di tm_wday (0 = domenica), day e'
 * il giorno del mese 1-31.
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
 *
 * Durante la riprogrammazione l'interrupt viene disabilitato e il flag AF azzerato,
 * per non generare un interrupt spurio su un match parziale dei registri in corso
 * di scrittura; lo stato precedente di AIE viene ripristinato al termine.
 *
 * @note La risoluzione effettiva dell'allarme su questo chip e' di 2 secondi.
 */
esp_err_t pcf85063a_set_alarm(pcf85063a_handle_t handle, const pcf85063a_alarm_t *alarm);

/**
 * @brief Rilegge l'allarme programmato
 *
 * I campi non attivi nel confronto (AEN_x = 1) vengono restituiti come
 * PCF85063A_ALARM_DISABLED.
 *
 * @param[out] alarm Allarme programmato
 * @param[out] interrupt_enabled Stato del bit AIE (puo' essere NULL)
 */
esp_err_t pcf85063a_get_alarm(pcf85063a_handle_t handle, pcf85063a_alarm_t *alarm, bool *interrupt_enabled);

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
