# References

## Allmess (first supported meter)

* [Data sheet System V and MK](https://www.allmess.de/fileadmin/multimedia/alle_Dateien/DB/DB_System%20V%20und%20MK_01_23.pdf)
  (German): the `+m` module register, shared across the meter family.
* [Tender text EVK/W 3/110-V +m](https://www.allmess.de/fileadmin/multimedia/alle_Dateien/AS/AS_P0850_EVK%20V_TS1221.pdf)
  (German)
* [Tender text communication modules](https://www.allmess.de/fileadmin/multimedia/alle_Dateien/AS/AS_P0821_Kommunikationsmodule_TS1221.pdf)
  (German): PM +m pulse, BM +m M-Bus, EquaScan wMIU RF.
* [Allmess downloads](https://www.allmess.de/download/)

## Components

* [Vishay TCRT5000(L) data sheet](https://www.vishay.com/docs/83760/tcrt5000.pdf):
  dimensions, peak response at 2.5 mm, current transfer ratio.
* [ESP32-C6 Super Mini: pinout and specs](https://www.espboards.dev/esp32/esp32-c6-super-mini/)
* [ESP-IDF: ADC on the ESP32-C6](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/peripherals/adc/index.html)
* [CNY70 reflective coupler explained](https://www.strippenstrolch.de/1-2-12-der-reflexkoppler-cny70.html)
  (German): a good walk-through of how to wire a reflective sensor.

## Similar projects with a reflective sensor

* [hugokernel/esphome-water-meter](https://github.com/hugokernel/esphome-water-meter):
  TCRT5000 on a reflective disc, ESPHome.
* [geertmeersman/energie-meter](https://github.com/geertmeersman/energie-meter):
  water and gas, ESPHome.
* [xperseguers/esphome-watermeter](https://github.com/xperseguers/esphome-watermeter)
* [anas-ivs/ESPHome-Water-Meter](https://github.com/anas-ivs/ESPHome-Water-Meter)
* [JN0V/WaterMeter](https://github.com/JN0V/WaterMeter): ESP32 and MQTT without
  ESPHome.

## Other ways to read a meter

* [jomjol/AI-on-the-edge-device](https://github.com/jomjol/AI-on-the-edge-device)
  ([documentation](https://jomjol.github.io/AI-on-the-edge-device-docs/)):
  an ESP32-CAM reads the number rollers.
* [wmbusmeters](https://github.com/wmbusmeters/wmbusmeters): wireless M-Bus
  decoding ([drivers](https://github.com/wmbusmeters/wmbusmeters/tree/master/drivers/src)).
  * [Issue #346](https://github.com/wmbusmeters/wmbusmeters/issues/346): Itron/Allmess
    EquaScan, closed without a driver.
  * [Home Assistant forum: EquaScan wMIU RF](https://community.home-assistant.io/t/equascan-wmiurf-water-meter/826448)
* [libmbus](https://github.com/rscada/libmbus): wired M-Bus.
