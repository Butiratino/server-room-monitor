# Быстрый старт

Полная инструкция по архитектуре, ошибкам, настройке Wi‑Fi/Zabbix и развёртыванию на другом ПК: [docs/TECHNICAL_NOTE_RU.md](docs/TECHNICAL_NOTE_RU.md).

## Текущая конфигурация

- Производственная прошивка по умолчанию — `main/temperature_monitor.c`. Режим выбирается строкой `set(FIRMWARE "temperature_monitor.c")` в `main/CMakeLists.txt`; параметры `-D OLED_SENSOR_DIAGNOSTIC` и `-D OLED_TEMPERATURE_MONITOR` режим не переключают.
- Зафиксированная среда — ESP-IDF 6.1.0, target `esp32s3`. На этом ПК порт платы исторически был COM4; на другом ПК определите порт платы заново в Диспетчере устройств. `COMx` ниже замените на найденный порт.
- После перевода сетевого интерфейса 10 в профиль Windows **Private** production-устройство подключилось к Wi‑Fi. Повторные ответы Zabbix Sender успешны: `wifi_state=online`, `zabbix_send_state=ok`, `zabbix_stage=none`, `zabbix_error=ESP_OK`. Один reset, вызванный USB monitor, перезагрузил сохранённые настройки и устройство подключилось снова. Интерфейс Zabbix **Monitoring → Latest data** вручную не открывался.

## Сборка, прошивка и монитор

Откройте в VS Code папку `D:\Oled1` и создайте терминал ESP-IDF. На текущем ПК можно активировать его настроенное окружение так:

```powershell
. D:\Espressif\Activate-Oled1.ps1
idf.py --version
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash
idf.py -p COMx monitor
```

Проверьте, что `idf.py --version` показывает 6.1.0. На другом ПК выберите установленное ESP-IDF 6.1.0 и обнаруженный COM-порт; локальные пути VS Code и активации могут быть привязаны к текущему ПК. Выйти из monitor: `Ctrl+]`. Перед прошивкой или serial-проверкой закройте monitor, чтобы освободить COM-порт.

## Wi‑Fi, настройки и проверка

Если сохранённой конфигурации ещё нет, устройство запускает setup AP. SSID, временный пароль и URL показываются на OLED; те же временные данные отправляются один раз отдельной USB setup-записью. Подключитесь к AP и вручную откройте `http://192.168.4.1`, чтобы ввести параметры своей Wi‑Fi сети и Zabbix. Периодические USB JSON-отчёты содержат статусы и ошибки, но не пароли.

Обычный reset или переподключение питания не стирает сохранённые настройки. По исходникам удержание PRG около 5 секунд после старта запрашивает режим настройки; физическое удержание кнопки пока не проверено. Пользовательской команды/комбинации для factory или network erase нет.

Для проверки production-прошивки из корня проекта, при закрытом monitor:

```powershell
python tools/check_temperature.py COMx
python tools/check_raw_measurement.py COMx
python tools/check_zabbix_firmware.py COMx
```

`tools/check_sensor_probe.py COMx` предназначен для отдельной диагностической прошивки `sensor_probe.c`; временный выбор прошивки также делается через `set(FIRMWARE "sensor_probe.c")` в `main/CMakeLists.txt`. После проверки верните `temperature_monitor.c` и пересоберите production-прошивку.

Прежние сообщения о проблемных BMP280 raw-данных относятся к историческому этапу диагностики, а не к текущему состоянию: рабочая прошивка сейчас используется с датчиком и успешной отправкой показаний. Подробности и границы проверок — в [технической записке](docs/TECHNICAL_NOTE_RU.md).
