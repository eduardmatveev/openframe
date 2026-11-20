# OpenFrame API v1

Это собственный открытый протокол OpenFrame. Реализация находится в `src/integration/ApiServer.cpp`, клиент Python 3.10+ — в `maya/openframe_maya/client.py`. Сеть используется только для TCP-соединений с числовым адресом `127.0.0.1`. Сервер не выполняет Python, MEL, shell или переданный пользователем код.

## Подключение и аутентификация

При запуске сервер выбирает свободный TCP-порт и создаёт файл `%LOCALAPPDATA%/OpenFrame/api/endpoint-<pid>-<instance>.json`. JSON содержит `api`, `host`, `port`, `token`, `pid`, `instance`. `token` — 256 случайных бит в виде 64 шестнадцатеричных символов. На Windows каталог защищён DACL только для текущего пользователя и SYSTEM; наследование внешней DACL отключено. На POSIX каталог имеет права 0700, файл 0600. Сервер удаляет собственный endpoint при штатном завершении и генерирует новый token при повторном старте. Token не передаётся через параметры процесса и не показывается в UI.

Клиент выбирает endpoint явно или обнаруживает доступные экземпляры. Он проверяет адрес до создания сокета, использует `AF_INET` и не вызывает DNS или proxy API. Если выбранный endpoint исчез после перезапуска, автоматический переход допустим только к единственному endpoint в том же каталоге. При нескольких кандидатах требуется явный выбор экземпляра.

```python
from openframe_maya import Client

with Client() as client:
    print(client.call("status")["frame"])
    client.call("seek", frame=12, audio=False)
```

Каждый запрос — один UTF-8 JSON-объект и завершающий LF. HTTP, WebSocket, JSON-RPC batch и уведомления без `id` не поддерживаются. Поля конверта обязательны:

```json
{"jsonrpc":"2.0","api":1,"id":1,"token":"<token from endpoint file>","method":"status","params":{}}
```

Ответ содержит `jsonrpc`, совпадающий `id` и либо `result`, либо `error: {code, message}`. Неизвестные поля и неверные типы отклоняются. Нативные кадры — целые, с нуля; границы диапазона включены. `rangeOut=-1` означает конец доступного материала. Нативный клиент возвращает исключение `OpenFrameError` при ошибке API/транспорта. Запросы сериализуются блокировкой клиента; необработанные ответы не перемешиваются между потоками.

Ограничения: 64 KiB на запрос, максимум 8 соединений, 120 запросов/секунду на соединение; явные `paths` содержат не более 256 файлов, `media.sequenceDescriptor` задаёт до 100000 последовательных кадров. Ответ не более 16 MiB, очередь отправки не более 2 MiB. Неаутентифицированное соединение живёт до 10 секунд, простаивающее аутентифицированное — до 120 секунд; для Maya с длинным timeout используется увеличенный предел ниже. После ошибки token, размера или частоты соединение закрывается. Большие проекты следует опрашивать через `objects.list`, `notes.list` и `status`, а не полным `project.get`.

`Client(timeout=2.0)` принимает конечное число 0.05..120 секунд (boolean не число). Один монотонный deadline охватывает ожидание RPC lock, соединение/проверку instance, отправку и весь ответ; частичные байты не продлевают его. `close()` прерывает сокет без ожидания RPC lock. `Client(..., cancel_event=threading.Event())` позволяет отменить также ещё не начатое соединение. Изменение `client.timeout` влияет на следующий запрос; протокольная ошибка/истечение deadline закрывает соединение, schema preflight до отправки не портит его.

Maya `timeoutSeconds` имеет тот же диапазон, исходный default 0.75. Preference приложения меняет server default только для последующих регистраций без явно заданного timeout; explicit client timeout имеет приоритет. Существующие сессии не изменяются от смены default. Сессия хранится без heartbeat `max(30, 2*timeoutSeconds+5)` секунд; idle authenticated TCP для этой Maya — `max(120, 2*timeoutSeconds+5)` секунд. Оба срока измеряются монотонными часами и проверяются раз в секунду; JSON `lastSeen` остаётся информационным wall-clock timestamp. Таймаут RPC не является сроком выполнения playblast или асинхронного import/export.

## Команды

Параметры в таблице названы точно. Ключи после `?` необязательны. Типы проверяются сервером до изменения модели; строки путей проходят локальную политику, включая вложенные ссылки открываемого проекта. Все операции модели выполняются в главном Qt-потоке, декодирование/импорт/экспорт — через рабочие очереди `ReviewSession`.

| Метод | Параметры | Результат |
|---|---|---|
| `status` | `sessionId?` | Кадр, revision, origin, playing, autoplay, rate, диапазон, длительность, busy, activeOwnerId, activeTimelineId, defaultTimelineId, playbackFrameStep, loopDelayMs, loopHoldEmpty, loopDelayActive, путь проекта, dirty, последняя ошибка и состояние экспорта; для собственной Maya-сессии — её конфигурация |
| `echo` | `text?` | Та же строка, до 4096 символов |
| `window.activate` | — | Запрос UI восстановить свёрнутое окно, показать и активировать его; возвращает `true` при передаче запроса |
| `project.get` | — | Снимок собственного JSON-формата проекта |
| `project.new` | `empty?` | `true`; empty очищает стартовый таймлайн |
| `project.open`, `project.import`, `project.save` | `path` | `true` либо ошибка; поддерживается формат OpenFrame |
| `project.defaultTimeline` | `id?` | Назначенный таймлайн; пустой ID отключает автоматическое добавление импортов |
| `media.import` | `paths` | `{accepted, busy}`; импорт асинхронный |
| `media.sequence` | `paths`, `fpsNum?`, `fpsDen?` | Аналогично; заданный порядок файлов сохраняется |
| `media.sequenceDescriptor` | `directory`, `baseName`, `first`, `last`, `extension`, `fpsNum`, `fpsDen`, `padding?` | `{accepted,busy,frameCount}`; один асинхронный source из ожидаемого набора файлов, правила ниже |
| `media.relink` | `id`, `path` | Принятие асинхронной перепривязки существующего источника |
| `media.audioTracks` | `id` | `{selectedAudioTrack, audioTracks}`; список AVStream audio-индексов и метаданных |
| `media.selectAudioTrack` | `id`, `streamIndex` | Обновлённый source; `-1` выбирает default, остальные значения должны присутствовать в audioTracks |
| `media.setAudioOffset` | `id`, `offsetUs` | Обновлённый source; точное signed integer или canonical decimal string, правила ниже |
| `media.pathReferenced` | ровно одно: `path` или `directory` | `{referenced}`; проверяет все `source.path` и `sequencePaths`, без возврата списка файлов |
| `objects.list` | `type?`: source/timeline/folder | Массив объектов без декодированных кадров |
| `object.get` | `id?` | Объект; пустой ID возвращает `{}` |
| `object.update` | `id`, `name?`, `parentId?`, `inFrame?`, `outFrame?` | Обновлённый объект; trim применим к источнику |
| `object.remove` | `id`, `force?` | Удаление; используемый источник требует force, непустая папка отклоняется |
| `folder.add` | `name?`, `parentId?` | Новая папка; циклические parent-ссылки запрещены |
| `timeline.add` | `name?`, `parentId?` | Новый таймлайн |
| `timeline.activate` | `id` | Выбор активного таймлайна |
| `timeline.insert` | `timelineId`, `sourceId`, `index?`, `inFrame?`, `outFrame?` | ID нового клипа; index=-1 добавляет в конец |
| `timeline.removeClip` | `timelineId`, `index` | Удаление клипа |
| `timeline.moveClip` | `timelineId`, `index`, `toIndex?` | Перестановка клипа |
| `timeline.updateClip` | `timelineId`, `index`, `inFrame?`, `outFrame?`, `audioSourceId?`, `audioOffsetUs?`, `gain?`, `muted?` | Изменение trim/аудио клипа |
| `seek` | `frame`, `audio?`, `origin?`, `fromRangeStart?`, `ignoreLoopState?` | Фактически установленный кадр; origin разрешён только собственной зарегистрированной Maya-сессии |
| `play` | `playing?`, `rate?`, `audioTimeStretching?` | Старт/пауза; абсолютное rate от 1/16 до 16 включительно, отрицательное значение означает reverse |
| `audio.timeStretch` | `enabled?` | `{enabled}`; без параметра читает настройку, с boolean меняет её |
| `range.set` | `first?`, `last?`, `loop?`: once/loop/pingpong; `loopDelayMs?`, `loopHoldEmpty?` | Применённая пара границ; пропущенные параметры сохраняются |
| `timecode.get` | `id?` | `{offset, enabled}` активного источника/таймлайна или указанного ID |
| `timecode.set` | `id?`, `offset?`, `enabled?` | Та же структура после изменения |
| `viewer.get` | `pane?` | Объект, назначенный панели 0..3 |
| `viewer.assign` | `pane?`, `id?` | Назначенный объект; пустой ID снимает назначение |
| `viewer.set` | `mode?`: single/ab/wipe/blend/split/grid; `splitVertical?`, `wipe?`, `wipePivotY?`, `wipeAngleDegrees?`, `opacity?`, `showOverlay?`, `showAnnotations?`, `showPictureType?`, `color?`, `pane?`, `zoom?`, `panX?`, `panY?`, `panZoomEnabled?`, `rotation?`, `flipHorizontal?`, `flipVertical?` | Pan/zoom действует на всю композицию; pane выбирает только rotation/flip |
| `viewer.settings` | — | `{mode, activePane, wipe, wipePivotY, wipeAngleDegrees, showPictureType, color, panZoom: {zoom,panX,panY,enabled}, overlay}` |
| `media.frameMetadata` | `id` источника, `frame` | Компактный `{id,frame,pictureType,keyFrame,color}` одного исходного кадра; нумерация presentation order с нуля |
| `overlays.get`, `overlays.set` | get: —; set: параметры ниже | Полные текущие overlay-настройки; set сохраняет пропущенные поля |
| `bookmarks.list` | `id?` | Закладки всех или выбранного источника/таймлайна |
| `bookmarks.add` | `ownerId?`, `frames`, `endFrame?`, `name?`, `text?` | Число добавленных закладок; одинаковые кадры пропускаются |
| `bookmarks.clear` | `ownerId`, `bookmarks?`, `annotations?`, `notes?` | Очистка выбранных категорий |
| `annotations.list` | `ownerId?` | Метаданные аннотаций, включая начало/конец, слой, текст, visible и hold |
| `notes.list` | `ownerId?`, `plainText?` | Заметки; plainText удаляет Markdown-разметку без загрузки внешних ресурсов |
| `notes.add` | `ownerId`, `notes: [{frame,text,color?}]`, `author?`, `overwrite?` | Число добавленных/обновлённых записей; color — QColor-строка, например #ff123456; отсутствие color сохраняет существующий цвет при overwrite |
| `notes.importCsv`, `notes.exportCsv` | `path` | Импорт/экспорт собственного CSV-формата |
| `history.undo`, `history.redo` | — | Операция истории модели |
| `export.start` | См. ниже | `{jobId,state,progress,path}` |
| `export.status`, `export.cancel` | — | Состояние последнего задания / запрос отмены |
| `maya.register` | `sessionId`, `name`, `pid?`, `direction?`, `offset?`, `multiplier?`, `fromRangeStart?`, `mayaRangeStart?`, `timeoutSeconds?` | status с конфигурацией сессии |
| `maya.sync` | `sessionId`, `mayaFrame?` | Один обмен вместо status/seek/status: актуальные direction/offset/multiplier/range применяются сервером атомарно; возвращает status и `syncFrameApplied`. Запись разрешена только владельцу регистрации и при соответствующем направлении |
| `maya.configure` | `sessionId`, `config` | Новая конфигурация указанной сессии |
| `maya.sessions`, `maya.unregister` | — | Список сессий / удаление собственной регистрации |

`export.start`: обязательный `path`, необязательные `kind` (video/audio/images), `timelineId`, `first`, `last`, `width`, `height`, `fpsNum`, `fpsDen`, `viewerComposition`, `includeAnnotations`, `includeAudio`, `videoCodec`, `audioCodec`, `imageFormat`, `crf`, `frameStep`, `bookmarksOnly`, `frameNumbering` (zero_based/one_based/match_frame/custom), `sequenceStart`, `sequencePadding`, `sequenceBaseName`, `publishSequenceIntoExistingDirectory`, `annotationBackground` (четыре RGBA целых 0..255), `colorMode`, `bitDepth`. Размеры 16..8192, frameStep 1..1000000, padding 1..12. Bookmarks-only применяется к последовательностям изображений. Путь images по умолчанию обозначает новый каталог; при publishSequenceIntoExistingDirectory он обозначает существующий локальный каталог. Изменение фона действует только на экспортный снимок проекта. Доступность video encoder и совместимость color/depth проверяются до создания job ID; ошибки последующего чтения/записи возвращаются в результате задания. Отмена проверяется рабочим заданием; финальный результат приходит через `export.status`. Новый экспорт отклоняется, если уже работает задание, в том числе запущенное через UI.

`colorMode` — строго `display-converted` (default) или `preserve-source`, без underscore aliases. `bitDepth` — целое 0 (auto), 8 (default), 10, 12 или 16. Явная неподдерживаемая точность отклоняется до запуска, не уменьшается молча. Preserve-source требует `viewerComposition=false` и однородного кодирования цвета выбранных исходных кадров; video использует FFV1/MKV. Display-converted PNG поддерживает 8/16 bit. Конкретные комбинации кодека, pixel format, глубины, range и metadata проверяет общий core validator.

Дополнительные `viewer.set` поля: `wipePivotY` [0,1], `wipeAngleDegrees` [-360000,360000], `showPictureType` boolean, `color` object с необязательными `hdrToneMapping` boolean, `sourcePeakNits` [0,10000] (0=auto), `referenceWhiteNits` [1,1000], `displayPeakNits` [1,1000], `mobiusTransition` [0,0.95]. Все поля проверяются до одной viewer mutation; неизвестные nested keys отклоняются. `wipe` — X pivot [0,1]. Геометрия, цвет и HUD сохраняются собственным форматом проекта.

Source objects содержат компактный `color`: pixelFormat/componentBits, primaries/transfer/matrix/range/chromaLocation, masteringMinNits/masteringMaxNits/maxCLL/maxFALL и до восьми masteringChromaticities. Полные picture-type/key-frame/color-change индексы возвращаются только в bounded `project.get` и сохраняются в `.oframe`; `status` их не дублирует. `media.frameMetadata` проверяет индекс до чтения; для старого проекта без индекса `pictureType="Unknown"`, `keyFrame=null`. При известном key-frame flag возвращается boolean; у независимого raster кадра возможен `keyFrame=true` при неизвестном picture type. I/P/B не угадывается по key-frame flag.

Pressure strokes сохраняются как `annotations[].widthScales`: отсутствие/пустой массив означает постоянную ширину, иначе требуется по одному конечному числу [0,1] на каждую point. `annotations.list` остаётся кратким списком; новый произвольный drawing/code endpoint не добавлен. `project.open` отклоняет malformed новые поля до замены текущего проекта; отсутствие полей старых проектов получает defaults. Для старого проекта без производных media-индексов после принятия JSON запускается локальный re-probe: `busy=true` до его завершения, обновление color/type/key metadata не меняет dirty/undo. Пока индекс отсутствует, ответ остаётся `Unknown`/`null`; ошибка перепроверки файла не подменяет старый PTS-индекс.

`fpsNum` и `fpsDen` задают частоту выходного видео рациональной парой, например `30000` и `1001`. Поля передаются только вместе; каждое должно быть JSON-числом с целым значением от 1 до 1000000, а отношение — от 0.001 до 1000 fps включительно. Неполная пара, нулевые, дробные, отрицательные значения, строки, bool и null отклоняются с `-32602`. Если оба поля отсутствуют, используется частота экспортируемого источника или таймлайна; пара `0/0` в API не допускается. Видео пересчитывается на выходную частоту по временным меткам кадров, поэтому изменение fps не ускоряет исходный материал. Частота владельца не меняется. Эти параметры не меняют временной диапазон аудио и выбор исходных кадров при экспорте последовательности изображений.

`media.audioTracks` возвращает для каждой дорожки `streamIndex`, `codec`, `title`, `language`, `sampleRate`, `channels`, `startUs`, `durationUs`, `isDefault`. Индекс — AVStream index, а не порядковый номер в audioTracks. Выбор `-1` использует отмеченный default или первую доступную дорожку; это не mute. Смена дорожки сохраняется в проекте и пересоздаёт аудиотранспорт через core. `source`-объекты также содержат `selectedAudioTrack`.

`media.sequenceDescriptor` не выполняет glob или сканирование каталога. Для каждого целого номера от `first` до `last` включительно строится `baseName.<number>.extension`: абсолютное значение номера дополняется нулями до `padding` цифр, знак минус при отрицательном номере стоит перед ними. Например, first=-2, padding=4 даёт `frame.-0002.png`. Число файлов 1..100000; номера в пределах ±1000000000; padding 1..12, по умолчанию 4. Base name — 1..128 ASCII-символов, начинается с буквы или цифры, далее разрешены буквы, цифры, точка, пробел, `_` и `-`; завершающие точка/пробел запрещены. Extension — одно из png/jpg/jpeg/bmp/tif/tiff/webp/tga/ppm/pgm/pbm. Частота задаётся обязательной положительной рациональной парой: компоненты 1..1000000, отношение 0.001..1000. Общая длина ожидаемых абсолютных путей ограничена 16 MiB при подсчёте двух байтов на UTF-16 code unit.

Схема и локальный каталог проверяются до постановки задания. Worker проверяет **каждый** ожидаемый файл, включая отсутствие reparse, до единственного добавления готового source; декодер повторяет guard перед чтением изображения. Missing/reparse приводит к `status.lastError` и не создаёт частичного source. `accepted` означает постановку задания, а не завершённый импорт. При принятии нового импорта предыдущая `lastError` очищается; клиент ждёт source через `objects.list`, проверяя `status.busy` и `lastError`. Метаданные `object.get`/`objects.list` содержат первый путь, count и FPS, без полного массива sequence paths. Собственное сохранение проекта включает весь массив; для больших последовательностей следует использовать `project.save`, а не передавать полный `project.get` по сокету.

`media.setAudioOffset` использует микросекунды в пределах ±9007199254740991 (±(2^53−1)). Положительное значение прибавляется ко времени чтения аудио, то есть звук опережает изображение; отрицательное задерживает его. Это собственный явно заданный знак OpenFrame, не утверждение о знаке эталона или Maya audio node offset. Принимаются точные JSON integers либо строки вида `0`, `123`, `-123`; `+1`, `-0`, ведущие нули, пробелы, exponent, bool, null и дроби отклоняются. Изменение поддерживает undo и project persistence; `source.audioOffsetUs` возвращается числом без потери точности в разрешённом диапазоне.

`audio.timeStretch` и `play.audioTimeStretching` управляют сохранением высоты звука при изменённой скорости воспроизведения. `status.audioTimeStretching` возвращает текущее значение. Начальное значение OpenFrame — true; это наше решение, точный default эталона не установлен. Все параметры `play` проверяются до изменения скорости или настройки. Экспорт сохраняет исходную временную шкалу с source offset и не применяет текущую playback speed.

`media.pathReferenced` сначала проверяет запрошенный локальный файл либо каталог через LocalPathPolicy, затем обходит только уже загруженную модель проекта. `path` требует точного совпадения; `directory` проверяет принадлежность по границе компонента (`foo` не включает соседний `foobar`). На Windows сравнение не зависит от регистра. Учитываются все кадры последовательности, в том числе не первый и наборы из разных каталогов. Ответ остаётся одним boolean, содержимое медиа не читается. Во время импорта или экспорта возвращается `-32010`: рабочее задание может использовать файл, ещё не представленный в текущей модели. Cleanup-клиент при такой ошибке сохраняет файлы и может повторить действие после завершения задания.

`loopDelayMs` — целое 0..600000; `loopHoldEmpty` — bool. Флаг `loopDelayActive` сообщает текущее ожидание на границе воспроизведения. Изменение только задержки не сбрасывает playback range. Global pan/zoom хранится для владельца primary viewer: zoom 0.01..64, panX/panY от -100 до 100 в нормализованных координатах композиции. `panZoomEnabled=false` выключает применение, сохраняя значения. Ранний OpenFrame применял эти три поля к отдельной панели; текущий контракт следует документированной общей композиции, без заявления о совпадении закрытого wire protocol.

`overlays.set` принимает `type` (off/safeArea/grid/center/dynamicSymmetry/phiGrid/custom/image), `color`, `opacity`, `lineWidth`, `gridColumns`, `gridRows`, `safeMargins: [x,y]`, `safeInnerMargins: [x,y]`, `centerSize`, `shapes`, `imagePath`, `imageRect: [x,y,w,h]`, `imageOpacity`, `imageKeepAspect`. Цвета — QColor-строки с alpha, например `#80ff0000`; opacity 0..1, lineWidth 0.1..64, grid dimensions целые 1..128, margins 0..0.49, centerSize 1..1024. Image path проходит локальную input-политику до файловых обращений; для type=image он обязателен. Координаты rect 0..1, ширина/высота положительны; выход за край композиции обрезается. Пустой путь очищает imagePath при другом типе overlay.

`shapes` целиком заменяет список, максимум 256 объектов. Поля фигуры: `id?`, обязательные `type` и `points`, затем `color?`, `fill?`, `width?`, `opacity?`, `text?`, `fontSize?`, `visible?`. Type: line/rectangle/ellipse/polygon/text. Каждая точка — `[x,y]` в 0..1; line/rectangle/ellipse требуют две точки, text одну, polygon минимум три. Максимум 4096 точек на фигуру и 65536 на список; общий framing-лимит 64 KiB также действует. IDs уникальны, отсутствующий ID генерируется. Width 0.1..64, fontSize целое 1..512, text до 16384 символов; текст не интерпретируется как код или HTML. Некорректный вложенный параметр отклоняет всё изменение до мутации проекта. Публичные возможности служат основанием наших типов; названия native методов и JSON-схема принадлежат OpenFrame.

Основные ошибки: -32700 — JSON/framing, -32600 — конверт/версия, -32601 — неизвестный метод, -32602 — схема или путь, -32001 — token, -32003 — размер, -32004 — частота, -32010 — операция модели отклонена, -32603 — внутренняя ошибка. Текст ошибок не содержит token.

Проверка MP4 на синтетическом 120-кадровом источнике: `tests/integration/run_mp4_codec_probe.py`, отчёт `test-results/integration-mp4/codec-report.json`. Через настоящий API проверены явные `mpeg4`/AAC и `h264_mf`/AAC: 120 декодированных кадров, 24 fps, видео 5.000 секунд. AAC после декодирования содержит 240640 сэмплов при 48 kHz, включая 640 завершающих сэмплов padding (13.333 мс), измеренный RMS около -30.49 dBFS. Неопределённый alias `h264` не принимается; `h264_mf` обозначает конкретный Windows Media Foundation encoder, автоматической подмены кодека нет. Исходный выявленный timing failure сохранён отдельно в `codec-report-before-fix.json`.

## Совместимый Python-фасад

`openframe_maya.compatibility.KeyframePro2ClientApi` предоставляет имена и аргументы из [публичного Python Client API Reference](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-python-client-api-reference), полученного 2026-11-20. Исходники оригинального клиента не использовались. Фасад адресуется исключительно к OpenFrame через endpoint/token, возвращает документированные категории bool/dict/list/tuple/None; подробная ошибка доступна в `last_error`.

Включены методы подключения, проекта, импорта, дерева объектов, таймлайнов, frame/range/timecode, закладок/заметок, playback/viewer и queue/query export. `import_file` ожидает завершения индексирования. `queue_export` переводит выбор all/stepped/bookmarks, numbering, временный фон и параметры viewer/audio в нативное задание. Импортированные источники получают переданные trim и parent/name. Экспорт последовательности публикует файлы с заданным base_file_name непосредственно в output_dir_path и не перезаписывает существующие файлы.

Дополнительно реализован `activate_window(self)` из предоставленного исследовательской стороной контракта клиента 1.5.7. Фасад передаёт собственную команду `window.activate`; её UI-обработчик сохраняет максимизированное состояние при восстановлении свёрнутого окна. Исходная реализация клиента не передавалась и не использовалась.

Нативные кадры zero-based; фасад использует display numbering: raw frame + включённый TC offset, иначе raw frame + 1. Это явно заданная конвенция OpenFrame, которую ещё нужно сравнить с зафиксированной версией эталона. Вложенные структуры словарей и неописанные детали исходного протокола не объявляются идентичными оригиналу. Собственный JSON/CSV не означает совместимость с закрытыми проектными форматами. Сравнительных подтверждений взаимозаменяемости original wire protocol или original project files нет.

Для существующих скриптов с `from keyframe_pro_2_client_api import KeyframePro2ClientApi` предусмотрен явно подключаемый shim `maya/openframe_compat/keyframe_pro_2_client_api.py`: добавьте `OpenFrame/maya` и затем `OpenFrame/maya/openframe_compat` в `sys.path`. Он переэкспортирует независимый фасад и подключается только к OpenFrame. Автоматическая подмена установленного оригинального клиента не выполняется.

Источники транспортных решений: [QTcpServer, явный адрес listen](https://doc.qt.io/qt-6.8/qtcpserver.html), [QRandomGenerator::system](https://doc.qt.io/qt-6.8/qrandomgenerator.html), [Win32 security descriptors](https://learn.microsoft.com/en-us/windows/win32/secauthz/security-descriptors).
