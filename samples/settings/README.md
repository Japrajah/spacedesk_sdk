# settings

Console-пример C++ wrapper `sd::SASettings`: перечисление метаданных,
экспорт и предварительная проверка профиля без сетевого подключения.

```powershell
..\build\spacedesk_sdk\Release\sd_settings.exe
..\build\spacedesk_sdk\Release\sd_settings.exe --export client.sasettings
..\build\spacedesk_sdk\Release\sd_settings.exe --validate client.sasettings
```

Экспорт не включает секреты. В отличие от новых BMP/WAV samples, существующий
файл экспорта заменяется — это прежнее поведение данного примера.

[Общая сборка](../README.md).
