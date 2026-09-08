# Общий код коротких примеров

`sample_client.h` — небольшая C++-обёртка **для samples**, не отдельный API DLL.
Откройте её, чтобы увидеть точные вызовы публичного C API:

| В примере | C API SDK |
|---|---|
| `find_host` | `sd_discover`, выбор по имени/номеру на стороне примера |
| `client.connect` | `sd_client_connect` |
| `client.poll` | `sd_client_poll` |
| `client.frame` | `sd_client_get_frame` |
| `client.mouse_move` | `sd_client_send_mouse` / `sd_client_send_mouse_absolute` |
| `client.touch` | `sd_client_send_touch` |
| `client.key_press` | `sd_client_send_keyboard`, DOWN + UP |
| `client.text` | `sd_client_send_text_utf16` |
| `client.start_audio` | `sd_client_audio_start` |
| `client.receive_audio` | `sd_client_audio_poll` |

`ConnectionLoop` задаёт длительность обработки событий. `Connection` владеет
SDK handle, останавливает аудио и отключает дисплей при выходе из области видимости.
Все вызовы выполняются в одном потоке.

`sample_support.h` содержит обработку ошибок и файловые handles.
`media_files.h` сохраняет BMP/WAV. Протокол и кодеки остаются внутри DLL.
Здесь нет разбора параметров запуска. Эти helpers не используются viewer/probe/settings.
