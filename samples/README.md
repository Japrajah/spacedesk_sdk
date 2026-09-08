# Примеры использования spacedesk SDK

Каждый исполняемый пример находится в собственной папке и использует публичный
API DLL. Короткие примеры запускаются без аргументов: изменяемые значения видны
прямо в `main.cpp`. В `common` находятся небольшая обёртка над C API, discovery
с выбором хоста, управление ресурсами и файловый вывод.

| Папка | Программа | Что показывает |
|---|---|---|
| [MouseMove](MouseMove/README.md) | `sd_mouse_move` | Относительное и абсолютное перемещение мыши |
| [Touch](Touch/README.md) | `sd_touch` | Рисование линии одним touch-контактом: DOWN → MOVE → UP |
| [ReciveImgPacket](ReciveImgPacket/README.md) | `sd_receive_image` | Получение FRAME-событий, decoded framebuffer и сохранение BMP |
| [SendKeyboard](SendKeyboard/README.md) | `sd_send_keyboard` | Windows virtual key DOWN/UP и UTF-16 текст |
| [ReciveAudio](ReciveAudio/README.md) | `sd_receive_audio` | Отдельный аудиоканал, PCM → WAV, необязательное воспроизведение |
| [viewer](viewer/README.md) | `sd_viewer` | Полный оконный клиент и его frontend helpers |
| [probe](probe/README.md) | `sd_probe` | Discovery и короткая проверка получения кадров, пример на C |
| [settings](settings/README.md) | `sd_settings` | Просмотр, экспорт и проверка SASettings |

Названия `ReciveImgPacket` и `ReciveAudio` сохранены в запрошенном виде.

## Сборка вместе с SDK

Из папки `spacedesk_sdk`, в среде с Visual Studio/CMake:

```powershell
cmake --build ../build/spacedesk_sdk --config Release
cmake --install ../build/spacedesk_sdk --config Release --prefix ../dist
```

Программы появляются в `../build/spacedesk_sdk/Release`, затем устанавливаются в `../dist/bin`.
DLL находится рядом с ними. Для выборочной сборки:

```powershell
cmake --build ../build/spacedesk_sdk --config Release --target sd_mouse_move sd_touch sd_receive_image sd_send_keyboard sd_receive_audio
```

## Независимая сборка против готовой DLL

Исходники SDK и его внутренние заголовки не нужны:

```powershell
cmake -S samples -B ../build/sdk-samples -DSPACEDESK_SDK_ROOT="C:/path/to/spd_client/dist"
cmake --build ../build/sdk-samples --config Release
```

Для установленного пакета также можно использовать `cmake -S ../dist/samples`:
его CMake автоматически находит соседние `include`, `lib` и `bin`. При такой
сборке DLL копируется к исполняемым файлам. Каждый каталог примера содержит свой
`CMakeLists.txt`; подключаются они через родительский `samples/CMakeLists.txt`.

## Как читать короткие примеры

```cpp
// Поиск хоста: пустые IP и имя включают discovery с выбором.
const auto host = sample::find_host(L"", L"", 28252);
// По IP:   sample::find_host(L"192.168.1.5", L"", 28252);
// По имени: sample::find_host(L"", L"MY-PC", 28252);

sample::Connection client;
client.connect(host);
client.wait_frame();
client.mouse_move(100, 0);

sample::ConnectionLoop loop(10000); // 10 секунд
while (loop.running()) {
    client.poll(loop.poll_timeout());
}
```

Discovery ждёт ответы 1,5 секунды. Имя сравнивается целиком без учёта регистра.
Один подходящий хост выбирается автоматически; несколько показываются нумерованным
списком. Если ничего не найдено, пример сообщает об ошибке. Явный IP пропускает поиск.
Порт найденного хоста берётся из его ответа.

`client.*` — вспомогательная C++-обёртка примеров, не новый API DLL.
[Таблица соответствия вызовам SDK](common/README.md) и реализация находятся рядом.
`viewer`, `probe` и `settings` сохраняют свой интерфейс и параметры запуска.

## Общие правила коротких примеров

- Handle SDK создаётся, используется и уничтожается в одном потоке.
- Цикл обработки событий длится 10 секунд после подключения и первого кадра;
  ожидание первого кадра имеет отдельный таймаут 10 секунд. Ошибки завершают пример раньше.
- API ввода возвращает результат постановки в очередь. Завершение цикла
  не является подтверждением доставки ввода сервером.
- BMP/WAV создаются в текущей рабочей папке. Существующие файлы не перезаписываются.
- `ReciveImgPacket` получает декодированные данные через публичный SDK; он не
  повторяет разбор compressed wire packets внутри sample.
- Exit codes: `0` — успех, `1` — ошибка.

Проверки `tests/test_samples.py` запускают тестовые сборки этих же исходников
с фиксированным loopback-хостом и укороченным циклом. Выбор хоста проверяется отдельно.
Тесты не отправляют ввод на рабочий spacedesk-хост.
