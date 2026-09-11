# esp_rtc_pcf85063a

Driver ESP-IDF per l'RTC esterno I2C **NXP PCF85063A** (variante TSSOP: PCF85063ATT),
basato sul driver I2C master standard (`driver/i2c_master.h`). Non dipende da
`esp_lcd_touch`/`esp_lcd_panel_io`: si collega direttamente a un `i2c_master_bus_handle_t`,
quindi puo' condividere lo stesso bus I2C di un touch controller o altri periferici.

Il comportamento e' allineato al driver Linux ufficiale dello stesso chip,
[`drivers/rtc/rtc-pcf85063.c`](https://github.com/torvalds/linux/blob/master/drivers/rtc/rtc-pcf85063.c),
da cui sono ripresi la procedura di scrittura dell'ora, il workaround per la
corruzione dei registri al power-on, la gestione del registro Offset e la
semantica dei flag OS/AF. Datasheet di riferimento: PCF85063A Rev. 7 (30 marzo 2018).

## Cosa fa

- Get/set di data e ora tramite `struct tm` (formato 24 ore, forzato in fase di init).
  Il chip memorizza solo le ultime due cifre dell'anno: si assume sempre il secolo
  2000-2099.
- Lettura/clear del flag **OS** (Oscillator Stop): indica se l'integrita' dell'ora
  non e' garantita (es. batteria di backup scarica o mai inserita). Lo stesso stato
  e' riportato dal valore di ritorno di `pcf85063a_get_time()`.
- Correzione della deriva dell'orologio tramite il registro **Offset**, in ppb,
  con selezione automatica del modo di correzione.
- Selezione della capacita' di carico del quarzo (**CAP_SEL**: 7 pF o 12.5 pF),
  obbligatoria in fase di init.
- Configurazione e rilettura della frequenza del pin **CLKOUT** (da 32768 Hz a 1 Hz,
  o spento).
- Allarme (secondo/minuto/ora/giorno/giorno della settimana, ognuno abilitabile
  singolarmente), con rilettura di quanto programmato, lettura/clear del flag **AF**
  e abilitazione dell'interrupt sul pin INT (bit AIE).
- Lettura/scrittura del byte libero di RAM del chip (registro `0x03`).
- Software reset del chip, con riapplicazione automatica della configurazione.

Non implementa il countdown timer a 8 bit del chip (registri `Timer_value`/`Timer_mode`),
non necessario per l'uso come semplice RTC di calendario: nemmeno il driver Linux
lo espone.

## Dipendenze

```yaml
dependencies:
  idf: ">=5.2"
```

Richiede l'API I2C master "nuova" di ESP-IDF (`driver/i2c_master.h`, disponibile da
IDF 5.2). Nel `CMakeLists.txt` dell'app va aggiunto `esp_rtc_pcf85063a` a
`PRIV_REQUIRES`/`REQUIRES`.

## Come integrarlo

Il PCF85063A ha indirizzo I2C fisso `0x51` (non configurabile). Basta un bus I2C
gia' creato con `i2c_new_master_bus()` — lo stesso bus puo' gia' avere altri device
agganciati (es. un touch controller):

```c
#include "esp_rtc_pcf85063a.h"

pcf85063a_config_t rtc_config = PCF85063A_CONFIG_DEFAULT();
// Obbligatorio: va dichiarato il quarzo montato sulla board. 7 pF e' il caso
// piu' comune, 12.5 pF si indica con PCF85063A_QUARTZ_LOAD_12500FF.
rtc_config.quartz_load = PCF85063A_QUARTZ_LOAD_7000FF;
pcf85063a_handle_t rtc = NULL;
ESP_ERROR_CHECK(pcf85063a_new(bus_handle, &rtc_config, &rtc));

// All'avvio: se l'oscillatore si e' fermato, l'ora salvata non e' affidabile.
// get_time lo segnala con ESP_ERR_INVALID_STATE, quindi il controllo del flag e
// la lettura si fanno in un colpo solo.
struct tm now;
esp_err_t err = pcf85063a_get_time(rtc, &now);
if (err == ESP_ERR_INVALID_STATE) {
    struct tm t = {
        .tm_year = 2026 - 1900, .tm_mon = 0, .tm_mday = 1,
        .tm_hour = 0, .tm_min = 0, .tm_sec = 0, .tm_wday = 4,
    };
    ESP_ERROR_CHECK(pcf85063a_set_time(rtc, &t)); // azzera anche il flag OS
    now = t;
} else {
    ESP_ERROR_CHECK(err);
}

ESP_LOGI(TAG, "RTC: %04d-%02d-%02d %02d:%02d:%02d",
         now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
         now.tm_hour, now.tm_min, now.tm_sec);
```

`pcf85063a_is_oscillator_stopped()` resta disponibile per chi preferisce
controllare il flag separatamente.

### Allarme

```c
pcf85063a_alarm_t alarm = {
    .second  = PCF85063A_ALARM_DISABLED,
    .minute  = 30,                       // scatta a XX:30:00 ogni ora...
    .hour    = PCF85063A_ALARM_DISABLED, // ...tutte le ore...
    .day     = PCF85063A_ALARM_DISABLED, // ...tutti i giorni...
    .weekday = PCF85063A_ALARM_DISABLED,
};
ESP_ERROR_CHECK(pcf85063a_set_alarm(rtc, &alarm));
ESP_ERROR_CHECK(pcf85063a_enable_alarm_interrupt(rtc, true)); // opzionale: pilota il pin INT

bool triggered = false;
ESP_ERROR_CHECK(pcf85063a_get_alarm_flag(rtc, &triggered));
if (triggered) {
    ESP_ERROR_CHECK(pcf85063a_clear_alarm_flag(rtc));
}
```

Il pin INT del chip e' open-drain attivo basso: se cablato a un GPIO, va configurato
lato applicazione (`gpio_config()` + interrupt), il componente non lo gestisce
direttamente. La risoluzione effettiva dell'allarme su questo chip e' di 2 secondi.

### Calibrazione della deriva

```c
// Orologio lento di circa 10 ppm -> correzione positiva di 10000 ppb
ESP_ERROR_CHECK(pcf85063a_set_offset(rtc, 10000));

int applied = 0;
ESP_ERROR_CHECK(pcf85063a_get_offset(rtc, &applied));
// 'applied' e' il multiplo del passo di quantizzazione piu' vicino al valore
// richiesto (4340 ppb nel modo "normal", 4069 ppb nel modo "course"): il driver
// sceglie da se' il modo che approssima meglio.
```

## Note

- L'indirizzo I2C `0x51` e' fisso sul PCF85063A: non serve/e' previsto un Kconfig
  per configurarlo.
- La capacita' di carico del quarzo non ha un default implicito: una scelta
  sbagliata si manifesta solo come deriva lenta dell'orologio, difficile da
  diagnosticare a posteriori, percio' `PCF85063A_CONFIG_DEFAULT()` lascia il campo
  a `PCF85063A_QUARTZ_LOAD_UNSPECIFIED` e `pcf85063a_new()` rifiuta quel valore.
  Va letto lo schematico della board e dichiarato il valore corrispondente.
- Se il chip non risponde su I2C in fase di init (`pcf85063a_new`), la funzione
  ritorna errore invece di proseguire silenziosamente: a differenza di un touch
  controller, un RTC che non risponde non ha un fallback sensato lato applicazione.
  Il controllo di presenza e' una lettura, quindi non altera lo stato del chip.
- Se in fase di init il flag OS risulta attivo il driver manda un software reset
  preventivo, come fa il driver Linux: il datasheet segnala che una piccola
  percentuale di esemplari puo' avere i registri corrotti dopo il power-on reset
  automatico.
- `pcf85063a_set_time()` scrive il blocco data/ora con la catena di divisori
  bloccata, cosi' l'ora impostata e' precisa al secondo, e azzera il flag OS.
  E' quindi la via preferibile rispetto a `pcf85063a_clear_oscillator_stop_flag()`,
  che essendo un read-modify-write sul registro Seconds perde la frazione di
  secondo in corso.

## Changelog

### 0.2.0

Allineamento al driver Linux ufficiale `drivers/rtc/rtc-pcf85063.c`.

**Cambio di comportamento:** la capacita' di carico del quarzo va dichiarata
esplicitamente. Il nuovo campo `quartz_load` di `pcf85063a_config_t` non ha un
default implicito: `PCF85063A_CONFIG_DEFAULT()` lo lascia a
`PCF85063A_QUARTZ_LOAD_UNSPECIFIED` e `pcf85063a_new()` rifiuta quel valore con
`ESP_ERR_INVALID_ARG` e un log che indica cosa impostare. La 0.1.0 forzava
silenziosamente 7 pF: chi era su quel valore (cioe' chiunque, essendo l'unico
comportamento possibile prima) deve aggiungere
`cfg.quartz_load = PCF85063A_QUARTZ_LOAD_7000FF;` per conservare il comportamento
attuale, senza altri effetti.

**Cambio di comportamento:** `pcf85063a_get_time()` ora ritorna
`ESP_ERR_INVALID_STATE` quando il flag OS e' attivo, cioe' quando l'ora letta non
e' affidabile (prima ritornava `ESP_OK` con dati non validi). La `struct tm` viene
comunque popolata. Chi incapsula la chiamata in `ESP_ERROR_CHECK()` senza aver
prima impostato un'ora valida deve gestire il nuovo codice di ritorno.

Correzioni:

- `set_time()` blocca la catena di divisori (bit STOP) durante la scrittura del
  blocco data/ora: azzera il prescaler, quindi l'ora impostata e' precisa al
  secondo, ed evita che un incremento dei secondi cada a meta' scrittura.
- `set_time()` azzera esplicitamente il flag OS e valida tutti i campi
  (compreso il giorno del mese, con gestione degli anni bisestili).
- Le scritture read-modify-write su Control_2 (`set_clkout`,
  `enable_alarm_interrupt`) non possono piu' azzerare per sbaglio un flag AF
  scattato tra la lettura e la scrittura.
- `set_alarm()` disabilita l'interrupt e azzera AF prima di riscrivere i registri,
  per non generare un interrupt spurio su un match parziale, e ripristina lo stato
  precedente di AIE al termine. I 5 registri vengono scritti in un'unica
  transazione I2C invece di cinque.
- `pcf85063a_new()` verifica la presenza del chip con una lettura invece di una
  scrittura, e manda un software reset preventivo se il flag OS e' attivo
  (workaround per la corruzione dei registri al power-on documentata nel datasheet).
- `pcf85063a_reset()` riapplica la configurazione del driver (modalita' 24 ore e
  capacita' di carico del quarzo), che il reset riportava ai valori di default.

Aggiunte:

- `pcf85063a_set_offset()` / `pcf85063a_get_offset()`: calibrazione della deriva in
  ppb tramite il registro Offset, con selezione automatica del modo di correzione.
- Campo `quartz_load` in `pcf85063a_config_t`: capacita' di carico del quarzo,
  7 pF o 12.5 pF, a scelta obbligatoria (vedi sopra).
- `pcf85063a_get_alarm()`: rilettura dell'allarme programmato e dello stato di AIE.
- `pcf85063a_get_clkout()`: rilettura della frequenza di CLKOUT.

### 0.1.0

Prima versione: get/set data e ora, flag OS, CLKOUT, allarme, byte di RAM,
software reset.
