# probe

Небольшой пример на C: UDP discovery и проверка получения кадров.

```powershell
..\build\spacedesk_sdk\Release\sd_probe.exe --discover
..\build\spacedesk_sdk\Release\sd_probe.exe 192.168.1.5 28252 3
..\build\spacedesk_sdk\Release\sd_probe.exe --websocket 192.168.1.5 28252 3
```

Использует `sd_discover`, создание/подключение клиента, FRAME events,
`sd_client_get_frame` и корректное завершение. Ввод не отправляет.

[Общая сборка](../README.md).
