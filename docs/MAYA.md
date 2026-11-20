# OpenFrame в Maya

Адаптер расположен в `maya/openframe_maya`. Начальная цель — Maya 2024 / Python 3.10 на Windows; код использует стандартную библиотеку Python и публичный Maya API. Окно автоматически выбирает PySide2 либо PySide6 из установленной Maya. Простое импортирование пакета не создаёт соединений, callbacks или изменений сцены.

## Запуск

Запустите OpenFrame. В Python-вкладке Script Editor добавьте каталог `OpenFrame/maya` в `sys.path` для текущей сессии и вызовите:

```python
import sys
sys.path.insert(0, r"D:\path\to\OpenFrame\maya")
import openframe_maya
window = openframe_maya.show()
```

Окно содержит выбор endpoint, API timeout, Connect/Disconnect, направление синхронизации, Offset/Multiplier/From Range Start, перенос диапазона, display timecode offset, переключение viewer layout, Swap A/B и playblast в A/B/проект. Подключение по умолчанию имеет направление Off. Не устанавливаются shelves, startup scripts, commandPort или глобальные настройки Maya. Пути к endpoint можно получить из окна Maya Tools приложения OpenFrame.

Программный вариант:

```python
from openframe_maya import Adapter, SyncMapping

adapter = Adapter(timeout=0.75).start(
    direction="bidirectional",
    mapping=SyncMapping(offset=0, multiplier=1, from_range_start=True),
)
# В конце работы, в основном потоке Maya:
adapter.stop()
```

`timeout` — конечное число 0.05..120 секунд, default 0.75. Его можно изменить через `adapter.configure(timeout=2.0)`, Maya UI или Maya Tools приложения OpenFrame. Число передаётся как `timeoutSeconds` в конфигурации этой сессии; неверное значение отклоняется до изменения других параметров. Локальная смена timeout прерывает текущий RPC и переподключает worker с новым сроком; изменение из приложения применяется к следующему RPC. Ожидающие callbacks предыдущего соединения не меняют сцену после reconnect.

Deadline один на весь RPC, включая ожидание занятого клиента, connect/auth и получение частичных байтов. Отдельный `Client` сохраняет default 2 секунды. `stop()` отменяет socket wait, закрывает соединение и дожидается worker; выбранные 120 секунд не означают 120 секунд ожидания Disconnect. Сервер учитывает timeout при heartbeat TTL: `max(30,2*timeout+5)` секунд; idle TCP — `max(120,2*timeout+5)`. Эти сроки не ограничивают продолжительность playblast/import/export. Пользовательский default OpenFrame применяется к новой регистрации без явного timeout; явно заданный адаптером timeout имеет приоритет и существующие сессии не переписываются одной сменой preference.

Направления: `off`, `maya_to_openframe`, `openframe_to_maya`, `bidirectional`. API-сервер никогда не соединяется с commandPort Maya. Каждый экземпляр Maya сам создаёт исходящее соединение только с `127.0.0.1`; список экземпляров и их настройки видны OpenFrame. Сессии имеют независимые UUID, имя и конфигурацию.

## Соответствие кадров

Нативная модель OpenFrame считает кадры от нуля. Для Maya frame `m`, multiplier `k`, offset `o`, range start `r` используются формулы:

- From Range Start выключен: `f = roundAwayFromZero(m*k + o)`.
- From Range Start включён: `f = roundAwayFromZero((m-1)*k + r + o)`.
- Обратная синхронизация использует точную обратную формулу; Maya может получить дробный кадр.

При включённом From Range Start, multiplier=1 и offset=0 Maya frame 1 соответствует началу диапазона OpenFrame. Offset применяется после умножения. Нулевой/нечисловой multiplier запрещён. Отрицательный multiplier допустим. Отрицательные результирующие OpenFrame-кадры ограничиваются допустимой длительностью источника; отрицательные Maya-кадры поддерживаются там, где формула их создаёт.

Это самостоятельный однозначный контракт. Публичные описания подтверждают наличие offset/multiplier/range options, но не достаточны для подтверждения всех сочетаний и пограничных случаев оригинала: [Maya → приложение](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-maya-to-keyframe-pro-2-overview), [приложение → Maya](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-maya-tools-overview). Проверка точного паритета формул остаётся сравнительным сценарием.

## Потоки и обратные события

Все `maya.cmds` и OpenMaya-вызовы выполняются на главном потоке. Обработчик `MEventMessage.timeChanged` сохраняет небольшой snapshot текущего кадра и диапазона. Сетевой поток читает snapshot и выполняет составную команду `maya.sync`: сервер применяет свою актуальную конфигурацию к исходному Maya frame и возвращает status одним ответом. Для обратного изменения времени адаптер ставит callable через `maya.utils.executeDeferred`; строковый Python/MEL не принимается и не выполняется.

Каждое изменение кадра имеет `frameRevision` и `frameOrigin`. Собственные отражённые события пропускаются; при применении удалённого кадра локальный callback не создаёт новое исходящее изменение. Очередь входящих изменений сворачивается до последнего кадра. При одновременном локальном и удалённом изменении в одном цикле отправки локальное изменение обрабатывается первым. Несколько Maya-сессий могут следовать за одним приложением без циклического эха.

Частота сетевого цикла ограничена 60 Hz; более частые timeChanged-события, например 120 fps, сворачиваются до последнего доступного кадра. Лимит сервера 120 запросов/секунду не повышается. Составная команда устраняет прежние три round trips на кадр и исключает применение устаревшего offset/range после изменения настроек в UI. Если направление на сервере уже выключено, запоздавший исходящий кадр игнорируется и `syncFrameApplied=false` возвращается вместе с актуальной конфигурацией.

После разрыва соединения выполняются повторное обнаружение/аутентификация/регистрация с задержкой 0.1..5 секунд. При неоднозначном выборе нескольких перезапущенных экземпляров требуется выбор endpoint. Конфигурация живой регистрации сохраняется на сервере; неактивные записи удаляются через 30 секунд. `stop()` удаляет callback, завершает поток и делает отложенные старые вызовы недействительными. Закрытие окна вызывает stop автоматически. Программно созданный Adapter требуется останавливать явно перед его выгрузкой.

Реализация следует ограничениям [Maya Python threading](https://help.autodesk.com/cloudhelp/2023/ENU/Maya-Scripting/files/GUID-9B5AECBB-B212-4C92-959A-22599760E91A.htm) и lifecycle [MEventMessage](https://help.autodesk.com/cloudhelp/2024/ENU/MAYA-API-REF/py_ref/class_open_maya_1_1_m_event_message.html). GUI idle нужен для `executeDeferred`; автономный mayapy без GUI event loop подходит для клиента API и тестов FakeMaya, но не является доказательством работы интерактивного потока обратной синхронизации.

## Диапазон и playblast

Диапазон изменяется только явными `send_range()` / `receive_range()` или соответствующими кнопками. Получение диапазона меняет playback min/max, но не animation range. Синхронизация времени сама диапазон сцены не меняет.

Playblast отдельно читает выделение Time Slider через публичные [`lsUI`](https://help.autodesk.com/cloudhelp/2024/ENU/Maya-Tech-Docs/CommandsPython/lsUI.html) и [`timeControl`](https://help.autodesk.com/cloudhelp/2024/ENU/Maya-Tech-Docs/CommandsPython/timeControl.html), без MEL-eval. При `rangeVisible=True` границы UI трактуются как полуоткрытые: `[5, 10)` даёт playblast кадров 5..9. Без выделения используется playback min/max. Сам playback range и контракт Send/Receive Range не меняются. Неоднозначный slider, нечисловая/нефинитная или короче одного кадра выделенная область отклоняются; точное поведение эталона на этих ошибках не установлено.

```python
adapter.playblast(
    r"D:\local\output\new_capture.avi",
    viewer="B", width=960, height=540,
    format_name="avi", timecode_offset=True,
)
```

Без пути создаётся уникальный локальный файл в `%LOCALAPPDATA%/OpenFrame/playblasts/<maya-session>/`. Существующие файлы не перезаписываются. Размеры кратны четырём. AVI использует несжатый режим; WebM доступен только в поддерживающей его версии Maya. `viewer=False`, `clearCache=False`: системный проигрыватель не запускается, чужие временные файлы не удаляются. Метод не меняет глобальные playblast preferences.

`playblast(...)` сохраняет прежний возврат `str` — имя movie. Необязательный `include_sound=True` передаёт выбранный звуковой узел Time Slider; `False` запрашивает movie без звука. Отдельный typed API доступен для явных настроек:

```python
from openframe_maya import CaptureSettings

result = adapter.capture(
    settings=CaptureSettings(
        format_name="image", compression="png", width=128, height=72,
        percent=100, quality=70, padding=4,
        show_ornaments=False, off_screen=True, include_sound=False,
    ),
    viewer="B", timecode_offset=True,
    callback=lambda source, error: print(error if error else source["name"]),
)
# Более короткая форма:
# result = adapter.playblast_sequence(width=128, height=72, viewer="B")
```

`CaptureSettings` неизменяемый снимок явно переданных параметров. `capture(path=None, *, settings=None, viewer="A", first=None, last=None, timecode_offset=False, callback=None)` возвращает `CaptureResult(kind, paths, first, last, fps_num, fps_den, sound)`: файлы уже сняты, импорт ещё выполняется. Для movie `path` — точное имя файла; для image — **новый, ещё не существующий каталог**. `first/last` либо отсутствуют вместе и читаются из Time Slider, либо задают явный включённый диапазон, не меняя playback min/max. Размеры 4..8192 кратны четырём; percent 10..100, quality 0..100, padding 1..12. Movie formats: avi/webm; image encoding: png/jpg/bmp/tif. Явное имя movie compression можно передать через `CaptureSettings`; недоступный codec вызывает ошибку Maya без подмены.

Image capture требует 1..100000 целых последовательных кадров. Публичный [`playblast(indexFromZero=True)`](https://help.autodesk.com/cloudhelp/2024/ENU/Maya-Tech-Docs/CommandsPython/playblast.html) задаёт ожидаемые имена `frame.0000.png`, `frame.0001.png` и далее, независимо от исходного Maya frame. Номера не извлекаются из произвольного glob. Полный ожидаемый список и 16 MiB бюджет путей вычисляются до создания выходного каталога; после capture проверяются наличие, ненулевой размер и локальная политика каждого файла. Неполная последовательность не импортируется. Частота считывается через публичный [`MTime.uiUnit/asUnits`](https://help.autodesk.com/cloudhelp/2024/ENU/MAYA-API-REF/py_ref/class_open_maya_1_1_m_time.html) и передаётся рациональной парой. `media.sequenceDescriptor` асинхронно создаёт **один source**, в том числе для 257+ кадров, без большого массива paths в JSON-запросе.

Сохранённые настройки Playblast UI сейчас **не читаются**. Официальный [`playblast`](https://help.autodesk.com/cloudhelp/2024/ENU/Maya-Tech-Docs/CommandsPython/playblast.html) помечен как NOT queryable; публичное соответствие конкретных optionVar ключей UI-настройкам не подтверждено. Поля окна OpenFrame являются явными настройками этого capture, а не отражением Maya preferences. Контролируемое наблюдение current settings остаётся отдельной незавершённой проверкой; никаких guessed preference keys или чтения внутренних Maya scripts нет.

Для movie `MayaHost.sound_snapshot()` на главном потоке получает sound node через `timeControl(query=True, sound=True)`, а его файл, offset, sourceStart/sourceEnd, endTime и mute — через [`sound(query=True, ...)`](https://help.autodesk.com/cloudhelp/2026/ENU/Maya-Tech-Docs/CommandsPython/sound.html). Путь проходит local-only policy; в `playblast` передаётся **имя узла**, поэтому его смещение и trim обрабатывает Maya. `TimelineSound` сохраняет также displaySound как визуальное состояние, отдельно от mute. Автоматического выбора всех сценовых дорожек или TRAX mixing нет. Image sequence не содержит и не импортирует аудио. Руководство Autodesk описывает включение звука Time Slider в movie: [Playblast an animation](https://help.autodesk.com/cloudhelp/2026/ENU/Maya-Animation/files/GUID-1C6EDC8D-DA67-490E-81F1-1205336DEBD9.htm); эквивалентность всех mute/hidden-waveform/TRAX случаев эталону не установлена.

Если звук не выбран, выключен или `include_sound=False`, аргумент `sound` опускается. Реальная Maya 2024 отклоняет сочетание `sound=""` и `throwOnError=True` с ошибкой загрузки звука; `throwOnError=True` сохранён. Отдельный синтетический probe подтвердил, что опущенный аргумент не подхватывает даже активный sound Time Slider: AVI содержит только видео; явный узел добавляет ненулевой PCM. Команды probe: `tests/integration/run_maya_playblast_options_probe.py`; фактические результаты: `test-results/maya-playblast-sound-probe-20261120-105626/independent-validation.json` (4 кадра, звук −17.764 dB RMS). Это проверка Maya 2024, не сравнительный PASS эталона.

На время захвата синхронизация приостановлена. Успешный захват устанавливает Maya на первый кадр снятого диапазона: при исходном кадре 17 и диапазоне 1..24 результат — кадр 1. Затем асинхронный импорт дожидается source ID, назначает A/B либо оставляет файл только в проекте и запрашивает активацию OpenFrame. Эти действия описаны в [публичных release notes, v1.4.4](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-release-notes); это основание реализации, а не доказательство сравнительного прогона установленной 1.7.1. При ошибке захвата OpenFrame восстанавливает исходный кадр в `finally` и возобновляет синхронизацию; поведение эталона при ошибке отдельно не установлено. `timecode_offset=True` задаёт source display offset равным первому кадру Maya. Текстовая callback-функция не принимается; callback вызывается на основном потоке как `callback(source, error)`. Публичная основа capture API: [Maya playblast](https://help.autodesk.com/cloudhelp/2026/ENU/Maya-Tech-Docs/CommandsPython/playblast.html).

Open Temp Folder открывает собственное окно Maya со списком имён и размеров файлов текущей adapter-сессии, не запускает Explorer, browser или shell extensions. Clear Temp Folder сначала выполняет компактные `media.pathReferenced` запросы для своих capture-групп. Сервер учитывает все пути sequence, включая не первый кадр и mixed-directory source, без выдачи большого списка путей клиенту. Удалению подлежат только созданные этим адаптером в своём временном каталоге файлы, которые не загружены в проект и не были изменены после создания. Конкретные каталог и имена показываются в диалоге подтверждения с Cancel по умолчанию; большие списки показывают первые 128 имён и точное число остальных в том же scope. После подтверждения ссылки запрашиваются повторно, затем проверяются локальный путь, отсутствие reparse и идентичность файла. Любая ошибка запроса ссылок прекращает очистку без удаления. Пользовательские выходные пути, файлы других сессий, изменённые/загруженные/неучтённые файлы сохраняются; рекурсивного удаления каталога нет.

Автоматический image capture создаёт уникальный дочерний каталог. Загруженный sequence source защищает всю группу кадров, хотя compact metadata содержит только первый путь. После подтверждения можно удалить неизменённые собственные файлы и затем пустой собственный каталог через `rmdir`; неизвестные/изменённые файлы сохраняют каталог. Частичные файлы неудачного capture также учитываются для явной очистки. Нет автоматического удаления при stop/выходе. Если capture завершён, но последующий импорт отвергнут декодером, callback получает ошибку, файлы сохраняются, assign/activate не выполняются. Поздний import failure не меняет время Maya повторно: первый captured frame уже установлен, пользователь мог продолжить работу. Это наша failure policy, а не подтверждённое поведение эталона.

## Проверки

`python -m unittest discover -s tests/integration -p test_maya.py -v` запускает синтетический localhost API и FakeMaya. Проверяются числовые преобразования, направления, Off без изменения времени, несколько сессий, отсутствие эха, reconnect, отмена отложенных callbacks, явный перенос диапазона, playblast в B, переход 17→1 после capture диапазона 1..24, восстановление кадра после ошибки, активация player, отказ перезаписи и запрет DNS/proxy/чужих адресов. Все вызовы FakeMaya проверяют thread identity. Эти тесты не заменяют фактический GUI smoke в Maya; результаты настоящей Maya фиксируются отдельно и должны использовать новую синтетическую сцену, не пользовательскую открытую сцену.

Текущие собственные проверки **0.4**: [Python 48/48](../test-results/python-04-timeouts.txt) и [native API 1800 assertions](../test-results/api-04-tests-final.txt). Delayed/drip server проверяет общий deadline, включая ожидание RPC lock; остановка прерывает запрос с timeout 120 секунд, а callbacks старого поколения не меняют сцену после reconnect. Native suite проверяет strict bounds/rollback, приоритет explicit timeout перед default и session TTL с внедрёнными монотонными часами и настоящими localhost сокетами. Это результаты тестовых targets; реальная Maya и packaged 0.4 в этих отчётах ещё не проверялись. Проверенный пакет 0.3 ниже остаётся отдельным checkpoint.

Checkpoint **0.3.0**: [финальный CTest](../test-results/ctest-0.3.0-final.log) — 5/5 suites PASS, core 60/UI 31/API 1448 assertions/Python 38. FakeMaya проверяет sequence 300 файлов → один source, выделение/negative-frame capture, preflight без создания каталога, partial failure, cleanup группы и unknown files, typed sound snapshot, async import error и отказ Maya от пустого sound аргумента. Нативный `ApiTests.cpp` проверяет настоящий импорт 300 BMP, compact responses, project persistence, first/last pixels, missing/reparse atomic rejection и audio offset/stretch API.

Реальный `tools/run_maya_gui_test.ps1 -ExtendedCapture` с packaged adapter в отдельной Maya 2024 прошёл **38/38**: [maya-report.json](../test-results/packaged-0.3.0-wave2/maya-final/maya-report.json). Собственная сцена дала movie24 и PNG sequence24/257, размеры 128×72, точные имена, один source на sequence, назначение B, timecode и переход 17→1; также проверены movie sound on/off. Независимая проверка завершённых файлов прошла **12/12**: [capture-file-report.json](../test-results/packaged-0.3.0-wave2/maya-files-opaque/capture-file-report.json). В movie со звуком 24 video frames и 48001 PCM samples при 48 kHz; измерены 439.9979 Hz и RMS 0.1414. Silent movie не содержит audio stream.

Для first/last кадров обеих sequences RGB экспортированных BMP совпал с независимо декодированными PNG после композиции на одинаковом чёрном фоне. Использован Windows System.Drawing, потому что system FFmpeg отказался читать EXIF этих Maya PNG. Оригиналы не изменялись и EXIF не удалялся. Первоначальный oracle сравнивал RGB без учёта alpha и был исправлен; исходные отчёты `maya-files` и `maya-files-final` сохранены. Этот pixel test охватывает четыре граничных кадра, а не каждый кадр каждой последовательности. Сравнительный результат эталона 1.7.1 по-прежнему не установлен; saved Maya Playblast UI preferences и Maya 2026 остаются отдельными границами.

`tests/integration/run_live_contract.py` проверяет независимый Python-фасад через настоящий C++ API. Скрипт требует endpoint **отдельного принадлежащего тесту экземпляра** и его instance UUID через `--owned-instance`, поскольку заменяет проект этого экземпляра. Весь корпус создаётся самим тестом: WAV с импульсами, заметки и экспортированные кадры. Отчёт завершённого прогона хранится в `test-results/integration-live/report.json`; он содержит 55 успешных проверок, включая activate_window, SHA-256 использованного exe и не содержит token. Фактический Maya GUI smoke фиксируется отдельно в `test-results/maya-gui-report.json`.

Реальные API-прогоны с FakeMaya при входных 60 fps (180 событий) и 120 fps (360 событий) записаны в `test-results/integration-rate/rate-report-60.json` и `rate-report-120.json`. Ошибок/переподключений нет, итоговый кадр совпал точно, максимум — 36 запросов/с в конкретном offscreen Windows-прогоне. Это проверка устойчивости, ограничения частоты и сворачивания очереди; она не утверждает, что на каждом событии отрисовывался отдельный кадр или что достигнуты 60 обновлений viewer в секунду.
