# OpenFrame: каталог и покрытие публичного интерфейса

Дата сверки публичных материалов: **20 ноября 2026**. Целевая установленная версия пользователя — **Keyframe Pro 2 1.7.1**; версия установлена отдельной проверкой metadata/SHA установщика. Этот UI создан по публичным материалам и собственным синтетическим данным, без чтения бинарников, ресурсов или исходного кода эталона. Ни одна строка ниже не означает сравнительно подтверждённого паритета.

Источники требований:

- [Главное меню](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-main-menu)
- [Настройки](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-preferences)
- [Навигация](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-scrubbing-and-frame-stepping)
- [Воспроизведение по закладкам](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-stepped-playback)
- [Перемещение и масштаб](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-pan-and-zoom)
- [Аннотации](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-annotations)
- [Заметки](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-notes)
- [Экспорт](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-export-to-file)
- [Горячие клавиши](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-hotkey-manager)
- [Нумерация кадров](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-time-control-frame-offset)
- [История Pro 2, включая 1.7.1](https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-release-notes)

## Сверка дополнений 1.7.1

Публичный release log задаёт следующие дополнения текущей волны: custom/image overlays и сохранение последнего overlay (1.7.1), dynamic symmetry/phi grid (1.6.2), выбор audio track источника и empty hold во время loop delay (1.6.1), сама задержка повтора (1.4.8). Страница Pan and Zoom отдельно задаёт общий transform всего viewer и хранение по primary timeline. Страница главного меню отстаёт от release log: перечисляет только safe/grid/center. Поэтому её короткий список не используется как доказательство отсутствия новых overlays.

Реализуемые пробелы этого этапа сведены в четыре группы: единый selector и редактор overlays; global pan/zoom с корректной обратной картой ввода; delay/empty controls общего transport; source audio-track selector. HDR/10–12-bit pipeline — отдельный последующий этап. Формат custom overlay, состав его примитивов и геометрия редактора в публичном описании не раскрыты; OpenFrame использует собственную открыто описанную модель линий, прямоугольников, эллипсов, полигонов и текста. Точное совпадение редактора не заявляется.

## Что означает статус

«Реализовано» означает наличие рабочего пути в собственном коде. Самостоятельные Qt-тесты проверяют интеграцию этого пути на синтетических данных. «Сравнительно проверено» требует отдельного прогона полного эталона и пока не заявляется ни для одного пункта. Точные исходные размеры, цвета, последовательность всех контекстных команд и недокументированные детали остаются предметом сравнительной проверки.

`OpenFrame --render-preview <локальный каталог>` формирует `ui-report.json` с **commandCatalog**: стабильный ID каждой зарегистрированной команды, подпись, все сочетания, возможность переключения и доступность в записанном состоянии. Там же сохраняются снимки пустого окна, собственного синтетического проекта, основных диалогов, всех вкладок Preferences и нижних областей прокручиваемых форм. JSON содержит размеры диалогов, devicePixelRatio, проверку доступности кнопок и неизменности проекта после отмены диалогов. Этот реестр дополняет таблицу ниже конкретными командами текущей сборки; он не является реестром команд эталона.

## Главное меню и основные панели

| ID группы | Рабочий путь | Реализация / самостоятельная проверка |
|---|---|---|
| UI-FILE-IMPORT | Файлы, папка, последовательность изображений | Стандартные Windows file/folder dialogs через общий wrapper; начальное расположение и выбранные результаты проверяются LocalPathPolicy. Offscreen и явно non-native QA используют собственный локальный picker. Импорт в движок, отдельный рациональный FPS последовательности. Drag-and-drop использует тот же импорт. |
| UI-FILE-PROJECT | Новый, открыть, импортировать, сохранить, сохранить как | `.oframe`/JSON, dirty-state, политика закрытия, отдельные recent files/projects. Qt-тест повторного открытия. |
| UI-FILE-CAPTURE | Clipboard и файл изображения viewer | Используется реальное изображение общего compositor. Выходной путь проверяется до записи. |
| UI-FILE-INSTANCE | Новое окно | Запускается только собственный executable. Настройка импорта в отдельные окна использует тот же путь. |
| UI-PROJECT-TREE | Источники, папки, несколько timelines | Докируемый фильтруемый список, перемещение источников/timelines по папкам, назначение A–D, переименование, удаление, relink. |
| UI-PROJECT-CLIPS | Монтаж timeline | Append, перестановка раньше/позже, удаление, source in/out включительно, gain/mute, подмена аудио и offset. |
| UI-PROJECT-PROPERTIES | Свойства источника/timeline и проекта | Имена, FPS, in/out для новых вставок, default import timeline, display offset, output resolution. |
| UI-EDIT-HISTORY | Undo/redo | Отдельная annotation history для owner/frame/FG–BG при видимой панели; иначе scene history. Тесты независимости notes и annotations. |
| UI-EDIT-RANGE | Начало/конец по текущему кадру, сброс | Общая модель диапазона и drag handles; начальный `rangeOut=-1` трактуется как конец. Тест initial-range action. |
| UI-EDIT-TOP | Поверх других окон | Реальный Qt window flag. |
| UI-PLAYBACK | Вперёд/назад, пауза, скорость, шаг | Общий session transport. Стрелки/кнопки — один кадр, Alt/Shift+стрелки — десять. Смена playback step относится к воспроизведению и stepped export. |
| UI-BOOKMARK-PLAYBACK | Воспроизведение по закладкам | Ctrl+Space или Ctrl+Play; держит изображение до следующей закладки во времени timeline, показывает Stepped. |
| UI-PLAYBACK-NAV | Начало/конец, диапазон, соседний edit | Действия работают через session; отдельная настройка транспортных кнопок для range start/end. |
| UI-PLAYBACK-LOOP | Once/loop/ping-pong, loop/lock scrub, autoplay, delay/empty hold | Delay в секундах (0–600, точность 1 ms) и отдельный empty hold привязаны к общему движку. В UI нет отдельной имитации transport таймером. |
| UI-AUDIO | Volume, mute, scrub, source track | Общий аудиовыход; popup volume; backward-audio flag; отдельные условия scrub для viewer/global/range. Source Properties и Audio menu выбирают default либо AVStream index с codec/language/title/rate/channels; выбор действует на source во всех его clips и в export. |
| UI-AUDIO-OFFSET | Source audio offset, preserve pitch | Offset редактируется в ms в Source Properties, передаётся в core в us и сохраняется с project undo. Positive advances / negative delays — явно заданная семантика OpenFrame. Audio menu и Preferences меняют настоящий pitch-preserving playback stretch; состояние хранится в локальных настройках. |
| UI-VIEW-COMPARE | A/B, wipe, blend, horizontal/vertical, 2×2 | Общий compositor; назначения A–D, swap A/B, wipe/transparency slider. |
| UI-WIPE-ROTATE | Поворот, pivot, snap, guides | Общая pixel-aspect геометрия полуплоскости используется UI и compositor. Divider drag перемещает границу по нормали, pivot drag меняет X/Y, handle вращает; Ctrl временно отключает snap, Alt позволяет жест при открытых annotation tools. Диалог точных значений, reset, center X/Y, 0/90/180/270 входят в реестр команд. Preview не меняет project/undo; release/OK создаёт один edit, Escape/focus loss/Cancel отменяют, внешняя project mutation инвалидирует token. Guides — только UI. |
| UI-VIEW-COLOR | HDR / Display Color, picture type | Реальные настройки tone mapping, source peak, reference white, display peak и Mobius transition с временным preview/Cancel. Picture Type независимо показывает декодированный тип I/P/B; source color metadata видна в Details. QWidget остаётся 8-bit sRGB, это не native HDR display. |
| UI-VIEW-TRANSFORM | Fit/zoom/pan, размеры, rotate/flip/fullscreen | Ctrl+wheel, Ctrl+MMB и Ctrl+Shift+MMB меняют весь viewer; global transform хранится по primary owner. Toggle отключает transform, сохраняя значения. Числовые zoom/pan fields доступны в properties; rotate/flip остаются per pane. Annotation/wipe hit tests применяют inverse global transform. |
| UI-VIEW-OVERLAY | Off/safe/grid/center/dynamic symmetry/phi/custom/image, media info | Единственный выбранный overlay, toggle last и независимый HUD. Общий compositor используется для viewer, export и preview редактора. Настраиваются цвет/opacity/width, grid dimensions, safe margins, center size, custom shapes/points/text/styles, local raster image bounds/aspect/opacity. Cancel не меняет project/undo; последний выбор и настройки сохраняются между запусками. |
| UI-ANNOTATION | Рисунок, стрелка, rectangle/ellipse, text, erase | Координаты нормализованы внутри owner-pane, цвет/толщина/opacity, FG/BG, заливка фона, отдельный undo owner/frame, hold, directional ghosts. AB, split/grid выбирают owner показываемой панели; короткий source использует фактически показанный крайний кадр. Draft и финальный кадр используют общий painter. |
| UI-ANNOTATION-TEXT | Dialog или inline text | Настраиваемый размер диалога; inline Ctrl+Enter/потеря фокуса сохраняет, Escape отменяет. Только plain text. |
| UI-ANNOTATION-TABLET | Pressure, smoothing, pen eraser, Ink/WinTab | Настоящие QTabletEvent: capability-aware pressure задаёт сохранённые widthScales, eraser pointer временно выбирает ластик, selected tool сохраняется. Сглаживаются входящие точки/pressure; финальная точка точна, сохранённая геометрия не зависит от будущих prefs. Ластик — mask только своего owner, FG keyframe/BG и named layer. Один штрих заполняется один раз без накопления alpha на самопересечении. RGBA64 premultiplied sRGB layer общий для preview и export, видео не приводится этим renderer к 8-bit. |
| UI-ANNOTATION-EYEDROPPER | Цвет готового кадра, RGB/hex | Shift+I, hover readout над всеми overlays, click выбирает текущий annotation color, Escape/right-click/F8 hide выключают инструмент. Измеряется physical pixel через общий QWidget painter после pan/zoom/comparison/overlays, сглаживания масштаба и смешивания с фоном; transient cursor/readout исключены. Инструмент не меняет owner, кадр, project или undo. |
| UI-ANNOTATION-LASER | Временный указатель | Shift+L, drag рисует локальный trail в QWidget; после release он затухает. Escape и скрытие toolbar очищают stroke. Laser не попадает в Project, shared compositor, сохранение или export. Цвет, ширина и время затухания настраиваются в Preferences с Cancel. |
| UI-ANNOTATION-GHOST | Previous/next и tint | Диалог по правой кнопке Ghost: кадры либо annotation keyframes, раздельные количества и цвета. |
| UI-BOOKMARKS | Frame/range, copy/cut/paste, previous/next | Реальные bookmark/annotation данные; переходы учитывают owner и loop. |
| UI-BOOKMARK-FILTER | All/empty/notes/annotations, remove empty | Единый классификатор для sidebar, timeline markers и previous/next bookmark. Note text и actual foreground keyframe внутри inclusive range делают запись непустой; background, ghost и held appearance не учитываются. Remove Empty действует только на текущий owner, включает named/range bookmarks и имеет project undo; сами notes/annotations остаются. Filter — UI preference, не изменение проекта. |
| UI-NOTES | Создать/изменить/удалить, label, навигация | Правая панель, редактор, цвет фона номера кадра; тест label rendering. |
| UI-NOTES-CSV | Import/export | Выбор frame/text/author columns, preview и валидация всех строк, quoted multiline CSV, необязательные owner/color. Явный выбор displayed/raw numbering. |
| UI-EXPORT | Видео/аудио/images, range/full, source/viewer | Фоновый snapshot, progress/cancel, реальные codec/format/quality controls, audio/annotation flags. Видео имеет явный rational output FPS (0.001–1000), по умолчанию owner FPS; source duration в диалоге вычисляется по PTS. |
| UI-EXPORT-CAPTURE | All/stepped/bookmarks-only | Bookmarks-only для sequence; настраиваемые numbering/base name/padding/start, background override. Для sparse images audio явно недоступен; для непрерывной sequence доступен audio.wav. FPS управляет только video и отключён для images/audio. |
| UI-EXPORT-OVERWRITE | Подтверждение или force для файла/sequence | Existing directory проверяется до перечисления; подтверждение учитывает существующие имена sequence и непрерывное audio.wav. Force пропускает подтверждение. Core сначала готовит весь результат, сохраняет резерв только коллизий, откатывает их при ошибке/отмене публикации и сохраняет посторонние файлы. |
| UI-EXPORT-COLOR | Display converted / preserve source, depth | Отдельные controls color mode и automatic/8/10/12/16 component bits. UI предлагает FFV1/MKV для high-depth и PNG16; несовместимые комбинации отключают Export с пояснением. Preserve требует Timeline Sources и проходит дополнительную preflight проверку source profiles/codec в core. Unsupported не понижается молча до 8-bit. |
| UI-LAYOUT | Global/range time, viewer/playback, sidebar, notes, console | Докинг, скрытие, перенос sidebar, reset; раскладка и геометрия сохраняются в локальном INI. |
| UI-HOTKEYS | Несколько сочетаний, конфликт, defaults | Назначение удаляет конфликтующее сочетание из предыдущей команды; Cancel не меняет bindings. |
| UI-MAYA | Состояние API, endpoint, sessions, sync options | Аутентифицированный локальный adapter, направления/offset/multiplier/range и timeout 0.05–120 s; Apply меняет выбранную session. Сохранённый default применяется сервером только при регистрации без явного timeout, существующие sessions не переписываются. Окно активируется через API без shell. |
| UI-HELP | Справка и About | Локальные страницы; никаких HTTP/HTTPS-переходов. Отсутствие активации согласовано в ТЗ. |

## Настройки: рабочая привязка

| Настройка | Наблюдаемое действие в OpenFrame |
|---|---|
| Runtime temporary directory | Guarded scratch-directory при следующем запуске; atomic export staging остаётся на целевом filesystem. Оригинальные intermediate-media файлы новый in-process engine не создаёт. |
| Cache MiB | Меняет реальный лимит RAM cache media engine. |
| Recent files / projects | Независимые лимиты двух меню. |
| Confirm close | Always / saved project changes / disabled. |
| Media in new players | Каждый выбранный локальный файл передаётся новому экземпляру собственного приложения. |
| Resize to fit | После завершения импорта применяется natural-size viewer command. |
| Pause on seek / scrub | Отдельно управляет остановкой при time-control seek и viewer scrub. |
| Endless scrub | Переносит курсор между краями текущего экрана без сброса накопленного движения. |
| Pan/zoom mouse | Отключает соответствующие mouse gestures. |
| Annotation toolbar on launch | Учитывается только при запуске, не при каждом Apply preferences. |
| Current-source highlight | Включает/выключает подсветку текущего clip в Timeline panel. |
| Transport range start/end | Меняет действия только start/end кнопок playback toolbar. |
| Backward audio | Управляет реальным reverse audio output. |
| Popup volume | Заменяет постоянный slider на рабочий popup slider. |
| Source root | Настраивает local relative-source root и начальную папку picker; путь проходит local policy. |
| Sequence FPS | Рациональные numerator/denominator используются по умолчанию при импорте. |
| Default displayed frame | Общий owner-aware conversion; core/API индексы остаются 0-based. |
| Default timeline / activate | Применяется к следующим новым проектам. |
| Restore pan/zoom / hold | Применяется при загрузке до формирования clean project state. |
| Text dialog / size | Переключает inline/dialog mode и реальный размер окна. |
| Export defaults | Directory, output kind, codec, image format, resolution, CRF, input, audio/annotations/range/overwrite. |
| API enable/port | Перезапускает сервер на числовом 127.0.0.1, с ошибкой при неуспехе. |
| Scrub speeds | Normal/Shift/Alt pixels-per-frame. |
| Audio on scrub | Viewer, global time и range time независимо; audio flag не отключает loop/range navigation. |
| Project undo | Отключает scene history, сохраняя отдельную annotation history и dirty state. |

## Явные архитектурные различия и ограничения сравнительной проверки

По явному выбору пользователя все файловые диалоги обычного Windows запуска используют Windows Shell: открыть проект/медиа/CSV/overlay, сохранить проект/CSV/кадр/export, relink и выбор папок. Shell может обращаться к настроенным сетевым расположениям, превью и shell extensions; это разрешённое исключение OS UI, а не снятие ограничений приложения. OpenFrame принимает только локальные результаты, сохраняя backend/API/libav path guards. Не прошедший проверку множественный выбор отклоняется целиком. `--render-preview` явно снимает собственный fallback и записывает `nativeFileDialogsValidated=false`: автоматические Qt проверки и эти снимки не подтверждают внешний вид или взаимодействие с нативным Windows диалогом.

1. **Внешний `ffmpeg.exe` не выбирается.** Приложение использует встроенные libav-библиотеки из фиксированной сборки без сети. Произвольный executable не позволил бы обеспечить заданное ограничение сети. Реальные codec/format/quality options доступны; неиспользуемого поля пути нет.
2. **Legacy Maya commandPort не эмулируется неаутентифицированным исполнением MEL/Python.** Вместо него используется согласованный authenticated adapter на 127.0.0.1. Это явное отличие wire protocol; сходство Python facade не доказывает wire compatibility. Timeout адаптера рассматривается отдельно в integration coverage.
3. **Viewer уже использует software raster rendering.** Переключатель, который ничего не менял бы, не добавлен. Производительность и вывод GPU backend эталона ещё не сравнивались.
4. **Открытая `.oframe` схема не объявляется совместимой с неизвестным оригинальным форматом.** Для сравнения нужны разрешённые образцы и наблюдаемый полный эталон.
5. **Sequence overwrite имеет транзакционную публикацию.** Можно выбрать новый или существующий локальный каталог; заменяются только выбранные output-файлы. Core-тесты проверяют коллизии, force, отмену в ходе публикации, откат и сохранение посторонних файлов. Сравнение точных окон подтверждения с эталоном ещё требуется.
6. **Точная геометрия, палитра, tool defaults и поведение ошибок** должны сравниваться в фиксированной версии эталона, Windows, DPI и размере окна. Собственные synthetic screenshots показывают работоспособность нового UI, но не заменяют такое сравнение.
7. **Default display numbering** выбран 1-based по публичной странице frame offset; custom offset трактуется как номер первого отображаемого кадра. Общий conversion находится в Core, его используют timeline, spin controls, notes/bookmarks, overlay и match-frame export.
8. **Есть конфликт неверсированных публичных материалов об annotations.** Страница Annotations в Limitations говорит об A-only и отключении рисунков в split при pan/zoom. OpenFrame сохраняет существующее независимое поведение owner-local аннотаций в A/B/split/grid, включая общий pan/zoom. Без наблюдения установленной 1.7.1 нельзя заключить, что ограничение осталось или было снято. Это явное расширение OpenFrame, не сравнительно подтверждённый паритет.
9. **Custom overlay editor имеет собственную геометрию и модель.** Поддерживаемые primitives и нормализованные координаты не объявляются совместимыми с закрытым форматом оригинальных custom overlays. Image overlay допускает только локальный raster через path guard и принудительный raster decoder; SVG/HTML/remote resources не загружаются.
10. **Параметры transient tools и аудио не объявляются original defaults.** В OpenFrame laser по умолчанию красный, 3 logical px, fade 350 ms; временные режимы не восстанавливаются при запуске. Eyedropper выбирает displayed 8-bit RGB, не исходные HDR samples. Time stretching включён по умолчанию как самостоятельное решение. Знак source audio offset и default stretch установленного эталона ещё не наблюдались.
11. **Tablet backend привязан к Qt 6.8.3.** Реальный private interface `QNativeInterface::Private::QWindowsApplication::setWinTabEnabled` проверен по pinned Qt source и SDK header; после вызова дополнительно проверяется `isWinTabEnabled`, потому что успешный return сам по себе не доказывает загрузку driver. На других платформах остаются Qt tablet events; несуществующих environment flags нет. События неактивного provider принимаются без рисования, синтетические mouse events во время/сразу после tablet контакта подавляются. Физическое перо, реальная сила нажима, eraser и дублирование событий конкретного драйвера требуют отдельного hardware прогона; synthetic Qt tests его не заменяют. Default OpenFrame — Windows Ink, pressure включён, smoothing 0%; это собственные решения.
12. **Wipe и HDR имеют явно собственную семантику.** Wipe angle задаёт нормаль в pixel aspect до общего pan/zoom: 0° оставляет A слева, 90° сверху; pivot нормализован. Snap tolerance 6 logical px по центрам и 4° у прямых углов, UI gestures/defaults не объявляются эталонными. HDR curve и luminance defaults документирует core; точного совпадения с tone mapper оригинала не заявляем. Обычный Windows QWidget не переключает монитор в HDR.

## Самостоятельные UI проверки

`tests/ui/UiTests.cpp` использует только собственные синтетические изображения. Проверяет меню/keyboard, frame/range/transform actions, annotation authoring и scoped undo, сохранность notes и annotations при независимых undo, note label cell, owner offset 1001, timeline mapping, рабочее применение preferences, CSV quoting, отказ UNC/URL/traversal до создания файлов и project roundtrip. Дополнительные регрессии: отмена/пустой export path без изменений проекта, force overwrite существующей sequence с сохранением постороннего файла, выбранный drive локального picker, отсутствие наложения status text, B/split/grid annotation owner и frame clamp; фактический экспорт 24000/2000 FPS и отказ недопустимого FPS без изменений проекта.

Волна 1.7.1 добавляет: отмену overlay editor без изменений проекта, фактический image-overlay pixel output, undo/redo overlay, отказ remote raster path и недопустимых координат, восстановление последнего типа между окнами, общий pan/zoom и inverse input в B, восстановление transform при смене primary owner, loop-delay/empty bindings, выбор audio track в properties и menu. UI-тест track selector использует собственные metadata descriptors и отключённое аудио; реальное различие waveform, export и очистку аппаратной очереди проверяет отдельный core suite. Preview дополнительно снимает все новые overlays, editor scroll, loop delay, timeline и source properties. Эти проверки подтверждают самостоятельную реализацию, а не поведение установленного эталона.

Волна OpenFrame 0.3.0 добавляет regression paths для sampled final-overlay RGB, точного совпадения с `QWidget::grab()` на сглаженном RGBA gradient, Escape/toolbar cancellation и отсутствия project/undo mutation; видимого laser stroke, его исчезновения и фактического PNG export без laser; classification notes/foreground/empty ranges, filter navigation/persistence и undo удаления; source offset conversion/Cancel/grouped undo и stretch preferences Cancel. Проверяются также сохранение точного большого offset при неизменённом поле, отсутствие лишнего Undo при OK без изменений и удаление source через API во время открытого properties dialog. Preview создаёт `eyedropper.png`, `laser-active.png`, `laser-faded.png` и раздел `transientTools` с проверкой неизменности project/history. Реальное сохранение pitch проверяется в core по синтетическим waveforms; UI не имитирует звук своим таймером.

Transient UI regression проходит на offscreen 100/125/150/200% и обычном Windows backend. События наведения в DPI regression направляются в локальных координатах виджета: это устраняет ограничение системного cursor виртуальными границами offscreen и не выдаётся за проверку физического устройства ввода. Пиксельное сравнение остаётся строгим на всех масштабах.

Волна 0.4 добавляет самостоятельные tests для RGBA64/sRGB и taper/нулевого pressure, alpha-once повторного прохода, изоляции eraser по owner/frame/BG/named layer и ghost/hold, реального временного preview eraser без изменения project, QTabletEvent pressure/fallback/eraser override, подавления повторных synthetic mouse events, smoothing/final endpoint/roundtrip, Escape/focus loss, aspect-correct wipe и обратного global pan/zoom, single-undo/Cancel/API mutation invalidation, новых color/export controls и Maya timeout preference. Native backend probe проверяет фактический результат WinTab initialization и возврат исходного backend; отсутствие драйвера записывается как отсутствие возможности, не как успешная проверка WinTab. Preview дополнительно сохраняет pressure/eraser/rotated-wipe сцены, Wipe Options, Display Color и high-depth Export; transient snapshots не меняют project/history. Финальные counts, executable SHA и DPI reports находятся в отдельном validation JSON текущей волны.

`--render-preview` проверяет наличие usable font/glyphs и помечает отчёт неуспешным, если platform backend рисует отсутствующие глифы. В Windows offscreen QA может требовать `QT_QPA_FONTDIR=C:\Windows\Fonts`; обычный Windows backend использует системные шрифты. Ничьи бинарные assets в тестовый корпус не включены.
