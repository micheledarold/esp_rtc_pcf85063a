# esp_rtc_pcf85063a

Driver ESP-IDF per l'RTC esterno I2C **NXP PCF85063A** (variante TSSOP: PCF85063ATT),
basato sul driver I2C master standard (`driver/i2c_master.h`). Non dipende da
`esp_lcd_touch`/`esp_lcd_panel_io`: si collega direttamente a un `i2c_master_bus_handle_t`,
quindi puo' condividere lo stesso bus I2C di un touch controller o altri periferici.

## Cosa fa

- Get/set di data e ora tramite `struct tm` (formato 24 ore, forzato in fase di init).
  Il chip memorizza solo le ultime due cifre dell'anno: si assume sempre il secolo
  2000-2099.
- Lettura/clear del flag **OS** (Oscillator Stop): indica se l'integrita' dell'ora
  non e' garantita (es. batteria di backup scarica o mai inserita).
- Configurazione della frequenza del pin **CLKOUT** (da 32768 Hz a 1 Hz, o spento).
- Allarme (secondo/minuto/ora/giorno/giorno della settimana, ognuno abilitabile
  singolarmente), con lettura/clear del flag **AF** e abilitazione dell'interrupt
  sul pin INT (bit AIE).
- Lettura/scrittura del byte libero di RAM del chip (registro `0x03`).
- Software reset del chip.

Non implementa il countdown timer a 8 bit del chip (registri `Timer_value`/`Timer_mode`),
non necessario per l'uso come semplice RTC di calendario.

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
pcf85063a_handle_t rtc = NULL;
ESP_ERROR_CHECK(pcf85063a_new(bus_handle, &rtc_config, &rtc));

// All'avvio: se l'oscillatore si e' fermato, l'ora salvata non e' affidabile
bool os_stopped = false;
ESP_ERROR_CHECK(pcf85063a_is_oscillator_stopped(rtc, &os_stopped));
if (os_stopped) {
    struct tm t = {
        .tm_year = 2026 - 1900, .tm_mon = 0, .tm_mday = 1,
        .tm_hour = 0, .tm_min = 0, .tm_sec = 0, .tm_wday = 4,
    };
    ESP_ERROR_CHECK(pcf85063a_set_time(rtc, &t));
    ESP_ERROR_CHECK(pcf85063a_clear_oscillator_stop_flag(rtc));
}

struct tm now;
ESP_ERROR_CHECK(pcf85063a_get_time(rtc, &now));
ESP_LOGI(TAG, "RTC: %04d-%02d-%02d %02d:%02d:%02d",
         now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
         now.tm_hour, now.tm_min, now.tm_sec);
```

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
direttamente.

## Note

- L'indirizzo I2C `0x51` e' fisso sul PCF85063A: non serve/e' previsto un Kconfig
  per configurarlo.
- Se il chip non risponde su I2C in fase di init (`pcf85063a_new`), la funzione
  ritorna errore invece di proseguire silenziosamente: a differenza di un touch
  controller, un RTC che non risponde non ha un fallback sensato lato applicazione.
