# app_display

Adapts the BSP panel and optional touch to GSP's display configuration. It owns
application display policy (RGB565, byte order, rotation and TE), not UI behavior.
The public dependency is ESP-GSP; BSP, display-present, LCD and logging are private
implementation dependencies. There is no dependency on `app_hello` or `main`.

```c
esp_gsp_esp_lcd_config_t display;
esp_err_t err = app_display_init(&display);
/* On ESP_OK, pass &display to esp_gsp_esp_lcd_start with the bundle config. */
```

Call once from boot, before GSP starts; do not call from ISR or concurrently.
Configuration is copied to caller storage only on success. Hardware handles are
borrowed from the singleton BSP and remain alive until reboot. Optional touch
failure yields `ESP_OK` with `display.touch == NULL`; display failure propagates.
The caller owns configuration storage, not hardware handles; do not free those
handles or reinitialize the BSP while GSP uses them. Resolution, pixel clock (Hz),
rotation and TE wiring come from BSP settings. NULL output returns
`ESP_ERR_INVALID_ARG`; missing panel/io returns `ESP_ERR_INVALID_STATE`; other
display initialization errors propagate from the BSP.
Hardware initialization may block using driver timeouts; there is no additional
application deadline. Partial hardware initialization is not rolled back. Keep
Recovery running if UI startup cannot proceed.

Private helpers remain static in the source file. If private headers become
necessary, put them under `private/` and declare only `PRIV_INCLUDE_DIRS`.
