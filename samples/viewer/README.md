# viewer

Полный Win32-клиент `sd_viewer`: изображение GDI/D3D11, discovery без адреса,
мышь/клавиатура/touch/pen, Sonar, настройки, fullscreen и управление подключением.
Все его `.cpp`/`.h` теперь находятся в этой папке. Реализация протокола и DSP
остаётся в SDK.

```powershell
..\build\spacedesk_sdk\Release\sd_viewer.exe
..\build\spacedesk_sdk\Release\sd_viewer.exe --gpu 192.168.1.5
..\build\spacedesk_sdk\Release\sd_viewer.exe --edit-settings
```

F5 — подключить/повторить, Shift+F5 — отключить, F8 — ввод, F9 — настройки,
F10 — текстовый ввод, F11 — fullscreen. В корне проекта подробности находятся
в `docs/VIEWER_INPUT.md`, `docs/SETTINGS_GUI.md` и `docs/SONAR_VIEWER.md`.

[Общая сборка](../README.md).
