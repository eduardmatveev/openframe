# Реестр соответствия публичному контракту Keyframe Pro 2 1.7.1

Дата аудита: 2026-11-20. Пользователь выбрал установленную версию эталоном; координатор подтвердил **1.7.1** по MSI metadata и хешу файла. Исполнитель этого реестра не открывал оригинальные бинарники, исходники, настройки, лицензии или результаты обратного анализа. Реестр использует публичные страницы, новый код OpenFrame и собственные синтетические проверки. На [странице продукта][PRODUCT] приложение обозначено как 1.7.1, а Maya script/Client API — как 1.5.7; эти номера не следует смешивать.

**Сравнительных PASS в этом документе нет.** Успешный тест OpenFrame доказывает только проверенное поведение OpenFrame. Запись в release notes подтверждает существование документированного требования, но не точные алгоритмы, default values, форматы или результат запуска установленного эталона. Большинство guide-страниц не привязаны к версии; их противоречия проверяются на 1.7.1. Видео, на которые ссылаются Quick Start страницы, не заменены предположениями из названия ролика.

Обозначения реализации:

- **T** — есть реализация и указан собственный исполненный тест; сопоставление с эталоном ещё нужно.
- **C** — код/тест добавлен или находится в текущей волне, общий прогон ещё не зафиксирован здесь.
- **M** — обнаружен конкретный пробел реализации, не просто отсутствие эталонного запуска.
- **D** — известное отличие архитектуры или поведения; отдельно указано, намеренное ли оно.
- **V** — функциональность имеется, но указанная граница требует проверки/уточнения.

Сокращения доказательств: **CORE** — `tests/core/CoreTests.cpp`; **UI** — `tests/ui/UiTests.cpp`; **API** — `tests/integration/ApiTests.cpp`; **MAYA** — `tests/integration/test_maya.py`. Проверенный checkpoint первой волны — **OpenFrame 0.2.0**, EXE SHA256 `28ff7123f40832eefd0bbcb18826e39b03ca59cc0d8c9c8727f28e47607a2022`: [ACCEPTANCE_0_2_0.md](ACCEPTANCE_0_2_0.md), [VALIDATION_0_2_0.json](VALIDATION_0_2_0.json). Его результаты не используются как доказательство новых функций.

Checkpoint **OpenFrame 0.3.0** зафиксирован в [ACCEPTANCE_0_3_0.md](ACCEPTANCE_0_3_0.md) и [VALIDATION_0_3_0.json](VALIDATION_0_3_0.json): [CTEST03] — 5/5 suites PASS, core 60/UI 31/API 1448 assertions/Python 38; [GUI03] — 38/38 реальных Maya 2024 checks на собственной сцене; [FILES03] — 12/12 независимых проверок capture-файлов. [FEATURES03] отдельно проверяет packaged source audio offset/export и включение preference time stretch. Метка T ниже означает исполненные собственные сценарии этого checkpoint, а не полную эквивалентность эталону или всеобъемлющую проверку каждого параметра.

Checkpoint **0.4.0** зафиксирован в [ACCEPTANCE_0_4_0.md](ACCEPTANCE_0_4_0.md) и [VALIDATION_0_4_0.json](VALIDATION_0_4_0.json): [CTEST04] — 6/6 suites, включая [PY04] 48 Python tests и [API04] 1800 native assertions; [CORE04] 60, [COLOR04] 22, [UI04] 40 тестов. Готовый EXE `5B753688…F354B4` прошёл [PACKCOLOR04] 55 color checks, [DRAW04] 14 drawing/wipe checks, [GUI04] 38 реальных Maya checks и [FILES04] 12 независимых проверок capture-файлов. HDR CPU 1080p измерен как 137.989 ms/кадр в среднем и не обеспечивает 24 fps. Метки T относятся к собственным исполненным сценариям; эталонное сравнение остаётся **UNVERIFIED**.

## Изменения 1.7.1 и соседних релизов

| ID | Публичное требование / первоисточник | Есть в OpenFrame | Пробел или граница | Проверка |
|---|---|---|---|---|
| R171-OV01 | Редактируемые custom overlays — [RN], 1.7.1 | T (0.2.0): shape-модель, JSON, API, UI | Формат, инструменты редактирования и точные defaults эталона неизвестны | Создать line/rectangle/ellipse/polygon/text; сохранить, открыть, экспортировать; отдельно выполнить такой же сценарий в 1.7.1 |
| R171-OV02 | Image overlay — [RN], 1.7.1 | T (0.2.0): локальный raster image, rect, alpha, aspect; API input policy | Совпадение anchor/scale/alpha эталона не доказано | Synthetic RGBA с цветными углами; размер/сдвиг/export; URL/UNC/reparse/disguised SVG должны отклоняться OpenFrame |
| R171-OV03 | Overlay menu и persistence — [RN], 1.7.1 | T (0.2.0): меню и сохранение новой структуры | Проверить последний выбранный тип после полного перезапуска, не только project round-trip | Изменить тип/параметры, закрыть/открыть приложение с собственными preferences, сравнить 1.7.1 |
| R171-OV04 | Dynamic symmetry / phi grid — [RN], 1.6.2 | T (0.2.0): типы в core/UI/API | Углы, число линий, reference geometry ещё не измерены | Synthetic checkerboard; сравнение относительных координат линий в одинаковом viewport |
| R171-OV05 | Настройки grid/color — [RN], 1.1.4 | T (0.2.0): размеры сетки, цвет, толщина, opacity | Диапазоны нашего API самостоятельные | API schema/round-trip; GUI размеры 1×1, 3×3, прямоугольная сетка |
| R171-OV06 | Eyedropper поверх overlays — [RN], 1.7.1 | T (0.3.0): sampler финального QWidget raster, RGB/hex, click → annotation color, F8/Escape/right-click | Сравнение цвета/порядка UI с эталоном отсутствует | UI eyedropperSamplesComposedPixelsWithoutProjectMutation; overlay/panzoom/comparison/scaling; [CTEST03] |
| R171-PR01 | Перемещение проекта на том же диске — [RN], 1.7.1 | V: `.oframe` поддерживает относительные и абсолютные локальные ссылки | Не установлено, подразумевает ли эталон перенос одного проекта или всего дерева | Два сценария: move project only / move project+media; восстановление источников, сохранение после переноса |
| R171-AU01 | Выбор audio track — [RN], 1.6.1 | T (0.2.0): AVStream metadata/selected track, transport reset, native API | Default и UI-поведение эталона не сравнивались | Собственный многодорожечный fixture; metadata/selection проверены; waveform export/reopen и reference comparison остаются отдельными сценариями |
| R171-PL01 | Loop delay, empty hold — [RN], 1.4.8/1.6.1 | T (0.2.0): delay ms и empty flag, core/UI/API | Семантика blank frame/clock/audio и reverse ещё не сравнена | Forward/reverse/once/ping-pong, диапазон в один кадр, cancel/seek во время ожидания |
| R171-AN01 | Smoothing и tablet eraser — [RN], 1.5.6 | T (0.4.0): QTabletEvent pressure/eraser, smoothing, сохранённые widthScales и общий renderer | Synthetic events не доказывают точность физического пера и parity driver | [UI04] tabletPressureEraserAndSyntheticMouseDeduplication/tabletFallbackSmoothingCancellationAndBackendRouting; [API04] schema/migration; [DRAW04] фактический export pressure/opacity/eraser |
| R171-AN02 | Wintab — [RN], 1.6.1 | T/V (0.4.0): выбор Windows Ink/WinTab через pinned Qt 6.8.3 backend, проверка инициализации | На машине WinTab init=false; проверен отказ с сохранением Ink, физический планшет не проверен | [UI04] backend probe и event routing; реальный аппаратный прогон остаётся pending |
| R171-HUD01 | Picture-type HUD — [RN], 1.4.2 | T (0.4.0): presentation-order type/key index, HUD и compact media.frameMetadata | Неизвестные metadata возвращаются Unknown/null; сравнение с эталонным HUD отсутствует | [COLOR04] pictureTypesMatchDecodeOrderAndReload; [API04] compact response, bounds и missing-index policy; [UI04] HUD |
| R171-BM01 | Bookmark filter / remove-empty — [RN], 1.5.1/1.5.5 | T (0.3.0): All/Empty/Notes/Annotations, sidebar/timeline/navigation и Remove Empty текущего owner с undo | Exact reference filtering/defaults неизвестны | UI bookmarkFiltersClassifyDataAndRemoveEmptyWithUndo; смешанные bookmarks, фильтр/удаление/undo; [CTEST03] |
| R171-WP01 | Wipe rotation/pivot/snap — [RN], 1.0.6 | T (0.4.0): X/Y pivot, rotation, UI gestures/options и native fields | Точная геометрия, snap/defaults эталона не измерены | [API04] bounds/round-trip/rollback; [UI04] gestures/preview/commit/cancel; [DRAW04] шесть математических полуплоскостей 160×96 и round-trip |

## Медиа, воспроизведение, звук и проекты

| ID | Публичное требование / первоисточник | Есть в OpenFrame | Пробел или граница | Проверка |
|---|---|---|---|---|
| R171-MD01 | Media/image sequences, RAM cache — [PRODUCT] | T: индекс PTS, cache, CFR/VFR/sequence decode | Полный набор форматов и длительная работа не доказаны | CORE cfrIndexAndReverse/bframesSeek/vfrIndex/imageSequence; повреждённые/отсутствующие кадры отдельно |
| R171-MD02 | Покадровый scrub и stepping — [SCRUB] | T: мышь, ±1, multistep, optional audio | Требуется сравнить физическую чувствительность и cached/uncached UI latency | UI timelineSeekMapsToFrames; CORE navigationRange; long-GOP отчёт ниже не измеряет GUI completion |
| R171-MD03 | Parallel image decoding — [NEW] | T (0.4.0): отдельный parallel sequence reader, bounded jobs/cache, directional prefetch | Throughput относительно эталона и полный набор форматов не измерены | [COLOR04] sequenceParallelBoundsReverseAndRepair: bounds, reverse, repaired files, empty sequence; [GUI04]/[FILES04] реальные Maya PNG |
| R171-MD04 | HDR tonemapping — [NEW] | T/V (0.4.0): color metadata, PQ/HLG tone mapping, controls | Точная кривая эталона неизвестна; CPU HDR 1080p mean 137.989 ms, не real-time 24 fps; peak 1000 nit по умолчанию и clipping выше peak | [COLOR04] anchors/peak/profiles/1080p; [PACKCOLOR04] HDR display image/video equality, tags и monotonic ramps |
| R171-MD05 | Высокая битность — [RN], 1.3.1 | T (0.4.0): native frame/high-depth image path и explicit export policy | Проверенные FFV1/PNG пути не доказывают каждый codec/format | [COLOR04] composed 4096 codes, profiles/levels; [PACKCOLOR04] 6×24 native 10/12-bit frames bit-exact и точные нормализованные SDR PNG16 |
| R171-PL02 | Скорость, reverse, диапазон, loop — [MENU] | T: транспорт, once/loop/ping-pong, навигационные flags | Не все сочетания clock/device/range сравнены с 1.7.1 | CORE actualAudioTransport/navigationRange; отдельный длительный device/underrun сценарий |
| R171-PL03 | Bookmarks-only playback — [STEP] | T: hold интервалов и seek к отображаемой закладке | Точные правила до первой/после последней закладки требуют эталона | CORE bookmarkIntervalsAndMutedNavigation; range без bookmarks и source/timeline offsets |
| R171-PL04 | Autoplay и mouse preferences — [PREF] | T: session/UI настройки имеются | Проверить весь набор defaults и сохранение на новом профиле | CORE autoplayOnViewerAssignment; UI preferencesApplyToActualControls; reference fresh-profile checklist |
| R171-AU02 | Audio time stretch — [NEW] | T (0.3.0): DSP-ветка core, preference, negotiated device PCM и strict native API | Default=true — наше решение; алгоритм и качество речи/транзиентов не сопоставлены с эталоном | CORE audioTempoSignals/ContinuousAndReverse/ScheduledSpanAndPulses, actualTimeStretchFullSpan/Transitions; epoch/backpressure regressions; [CTEST03], preference round-trip [FEATURES03] |
| R171-AU03 | Source audio offset — [NEW] | T (0.3.0): standalone `audioOffsetUs`, UI/core/API, exact JSON bounds, undo/persistence | Положительный offset опережает звук; знак/единицы эталона не подтверждены | API bounds/undo/save; CORE sourceAudioOffsetsAndPersistence; packaged ±250000 us pulse positions, export duration/direction independence и project round-trip — [FEATURES03] |
| R171-AU04 | Timeline audio override — [NEW] | T: источник замены, offset/gain/mute | Полная совместимость UI/defaults неизвестна | CORE timelineSeamsAndAudioReplacement; waveform на стыках, missing/short replacement |
| R171-AU05 | Reverse/audio scrub — [PREF] | T: PCM reverse, scrub queue, transport tests | Объективная A/V задержка через устройство и смена устройства требуют дополнительной проверки | Loopback measurement собственной flash/pulse сцены; reverse и остановка после scrub |
| R171-PR02 | Sources/folders/editable timelines — [PRODUCT] | T: insert/reorder/trim, folders, default timeline, relink | Native model independent; closed project interchange не реализован | CORE/API project round-trip, mixed clips, duplicate/remove/undo; GUI differential script |
| R171-PR03 | Project save/import/undo — [MENU] | T: `.oframe`, merge, QUndoStack | Оригинальный project format не заявляется; fixtures для межпрограммного обмена отсутствуют | Собственный JSON проверен; closed-format test остаётся отдельным исследовательским blocker |
| R171-PR04 | Переносимые источники и missing media — [PREF] | T/V: source root, relative references, relink | Политика запрещает UNC/reparse и `..`; это сознательное ограничение | CORE projectDefaultsAndDisabledUndo/relinkSequencePreservesMetadata; remote nested input negatives |

## Viewer, аннотации, заметки и UI

| ID | Публичное требование / первоисточник | Есть в OpenFrame | Пробел или граница | Проверка |
|---|---|---|---|---|
| R171-VW01 | A/B, blend, split, grid — [PRODUCT] | T: общая композиция и назначения | Layout/origin/crop/transparency reference pixels не сопоставлены | UI commandsOperateOnSession; synthetic A/B corners в каждом режиме |
| R171-VW02 | Global pan/zoom — [PAN] | T (0.2.0): единый transform композиции вместо прежнего per-pane | Новое поведение ещё требует differential run; rotation/flip остаются per pane | Два разных aspect ratios; split/grid при zoom 2 и pan; API owner restore |
| R171-VW03 | Pan/zoom per timeline, save/load — [PAN] | T (0.2.0): `panZoomByOwner`, primary-owner restore и load preference | Не определена reference granularity для source vs timeline | Сменить A/B/owner, toggle/restore/reset; сохранить и открыть; сравнить 1.7.1 |
| R171-VW04 | Rotate/flip, fullscreen, capture — [MENU] | T: UI actions, capture file/clipboard | Цвет, alpha и metadata требуют отдельного сравнительного корпуса | UI captureAndProjectRoundTrip; цветные углы + прозрачный фон; rotation metadata |
| R171-AN03 | FG/BG, hold, ghost tint — [ANNOT] | T: frame foreground, static background, hold, directional ghosting | Числа/tints/default visibility не объявляются совпадающими | CORE directionalGhostingAndBackground; boundary frames и новые owner IDs |
| R171-AN04 | Scoped annotation undo — [ANNOT] | T: отдельные frame/layer histories | Проверить hide-toolbar переключение и grouped strokes в 1.7.1 | CORE independentAnnotationHistory; UI annotationHistoryIsScopedAndPreservesNotes |
| R171-AN05 | Только annotations viewer A — [ANNOT], Limitations | D: OpenFrame рисует owner annotations также в B/grid | **Это самостоятельное расширение, не подтверждённое исправление parity.** Неверсионированная страница может отставать | Разные рисунки A/B в установленной1.7.1; сохранить screenshots/результат по каждому layout. До проверки рабочий код не удалять |
| R171-AN06 | Split pan/zoom скрывает annotations — [ANNOT], Limitations | D: OpenFrame сохраняет аннотации в композиции | Документированное ограничение не совпадает с текущим OpenFrame; поведение1.7.1 ещё не установлено | Split + zoom/pan, FG/BG/hold и playback; отдельный reference differential case |
| R171-AN07 | Laser pointer — [PRODUCT] | T (0.3.0): временный Shift+L pointer, цвет/width/fade, Escape; без export/save/history | Red/3px/350ms — собственные defaults, reference values не измерены | UI laserIsTransientAndAbsentFromExport; исчезновение, export/undo отсутствие следа; [CTEST03] |
| R171-AN08 | Background fill / visibility — [ANNOT] | T: alpha fill и режимы отображения | Default Off During Playback и startup-reset ещё надо сравнить | CORE/UI цвета; play/pause, restart, project load |
| R171-NT01 | Notes с label — [NOTES] | T: text editor, colored frame label, storage | Reference formatting и navigation while playing не измерены | UI noteLabelsAppearInFrameCell; edit/save/discard/delete; click vs double-click during play |
| R171-NT02 | CSV mapping/preview — [NOTES] | T: UI Column Select/preview, quoted multiline CSV | Наш native CSV с ID-полями не объявляется форматом эталона | UI csvParsesQuotedMultilineNotes; column reorder, Unicode, embedded newline; импорт CSV из1.7.1 только разрешённым fixture |
| R171-UI01 | Несколько hotkeys/action, conflicts — [KEYS] | T/V: список shortcut и reassignment имеются | Формат собственного config отличается, полный keyboard сценарий не пройден | Assign2 shortcuts, reassign conflict, remove1, defaults, restart |
| R171-UI02 | Layout/preferences/time controls — [MENU], [PREF] | T: панели, dock, time formats, visibility и persistence | Pixel/layout parity и весь перечень preference defaults не доказаны | UI preferencesApplyToActualControls/displayNumberingUsesOwnerOffset; DPI100/150/200 с одинаковым window size |

## Экспорт и интеграция

| ID | Публичное требование / первоисточник | Есть в OpenFrame | Пробел или граница | Проверка |
|---|---|---|---|---|
| R171-EX01 | Video/audio/images, source/viewer — [EXPORT] | T: asynchronous export/cancel и source/viewer input | Native API допускает расширения; exact dialog defaults требуют эталона | CORE exportAndCancel; UI exportCancellationAndBlankPathLeaveStateUntouched |
| R171-EX02 | Stepped / bookmarks images — [EXPORT] | T: sparse selections и numbering | Books-only ограничен images; timing reference не сравнивался | CORE steppedAndBookmarkExport/sequenceNamingAndBackground; сверить список номеров и final frame |
| R171-EX03 | Overwrite / export preferences — [EXPORT] | T/V: atomic outputs, sequence backup/rollback, UI settings | Native API намеренно не включает force-overwrite flag; набор codec/preset другой | CORE forceOverwriteSequenceRollback/cancellationPreservesExisting; failure preserves unrelated files |
| R171-EX04 | Rational FPS / color capture — [RN], 1.6.2 | T (0.4.0): outputFps pair, VFR по PTS, display-converted/preserve-source и explicit bit depth | Неподдерживаемые codec/depth сочетания отклоняются; не все кодеки охвачены, исходный dialog/defaults не сравнены | API actual12/24/30000:1001; [API04] preflight без mutation; [COLOR04]/[PACKCOLOR04] tagged ramps, native precision и display export |
| R171-EX05 | Внешний FFmpeg path — [PREF] | D: OpenFrame использует bundled SDK API | Намеренная архитектура; другой executable не подменяет codecs | Package DLL audit; unavailable encoder сообщает ошибку; MPEG4/AAC и h264_mf/AAC probes |
| R171-API01 | Python operations — [CLIENT] | T: независимый facade и55 own live checks | Это не wire compatibility; дополнительные params и error semantics самостоятельные | `run_live_contract.py`; comparative method scenarios по опубликованным signatures |
| R171-API02 | Maya Tools commandPort — [TOOLS], [PREF] | D: authenticated outbound adapter выполняет typed ops | Намеренно нет arbitrary MEL/Python commandPort и старого wire | Собственные bidirectional callbacks/ownership/reconnect tests; прямой старый клиент не объявляется работающим |
| R171-API03 | Maya Tools timeout — [PREF] | T (0.4.0): configurable client/adapter/server timeout, UI binding и monotonic session TTL | Timeout/defaults эталона не измерены; fault injection выполнялся в собственных targets | [PY04] delayed/drip, whole-RPC deadline включая lock, interruptible stop120/reconnect; [API04] precedence/rollback/TTL; [GUI04] нормальный обмен настоящей Maya с пакетом |
| R171-MY01 | Sync offset/multiplier/relative range — [MAYA] | T: оба направления, main-thread callbacks, mapping/coalescing | Сложные rounding/negative-range границы с1.7.1 не сопоставлены | MAYA mapping/multiple sessions/reconnect; actual Maya synthetic GUI smoke |
| R171-MY02 | После playblast первый кадр / activate — [RN], 1.4.4 | T (0.3.0): первый captured frame и window.activate; movie и обе sequence в B | Сравнительного GUI прогона эталона ещё нет | [GUI03]: Maya 17→1, movie24 и PNG24/257; MAYA failure сохраняет 17 — **наша failure policy**; activation проверена fake/native сценариями |
| R171-MY03 | Highlighted range без лишнего кадра — [RN], 1.4.4 | T: public Maya timeControl; `[5,10)`→5..9, timecode 5 | Реальный GUI selection ещё нужно пройти; malformed/fractional rejection — собственная policy | MAYA highlighted success/failure/no-selection; Send Range остаётся playback 1..24 |
| R171-MY04 | Текущие playblast settings — [MAYA] | M: saved Maya UI prefs не читаются; T (0.3.0): explicit CaptureSettings и typed timeline sound | Официальный getter полного UI snapshot/key mapping не найден; не выдавать defaults за current settings | [GUI03]/[FILES03]: sound on/off, 24 video frames, ненулевой PCM и отсутствие audio при выключении; public UI preference mapping остаётся pending |
| R171-MY05 | Image-sequence playblast — [RN], 1.3.4 | T (0.3.0): отдельный capture/sequence API, детерминированный набор, bounded descriptor → один source | Проверены конкретные Maya2024 PNG и native BMP; не все форматы/размеры/кадры имеют pixel oracle | API1448: 300 BMP, compact responses, missing/reparse no-partial; [GUI03]: PNG24/257, 128×72, один source, B/timecode; [FILES03]: first/last RGB exact на чёрном фоне |
| R171-MY06 | WebM Maya2026 — [RN], 1.4.1 | V: format предусмотрен, ошибка не маскируется fallback | Доступной GUI Maya2026 проверки нет | Реальный2026 capture/import/count; блокер проверки, не доказанный missing-code |
| R171-MY07 | Temp Folder / Clear — [MAYA] | D: локальный listing внутри Maya, scoped owned-file clear; T (0.3.0): sequence-group protection и empty-owned-dir cleanup | Намеренно нет Explorer и удаления чужих/изменённых/загруженных файлов | MAYA confirmation+identity+loaded-group/partial/unknown-file tests и fail-closed reference errors; [CTEST03]; реального GUI Clear comparison нет |
| R171-MY08 | Advanced Playblast activate — [RN], 1.4.4 | V: window.activate есть | Внешний Advanced Playblast сценарий не проверен; raw-wire совместимость отсутствует | Разрешённый synthetic integration scenario, без чтения стороннего codebody |
| R171-PLAT01 | macOS / Apple silicon — [PRODUCT], [NEW] | D: текущая поставкаWindows | Осознанная граница Windows-задания, не cross-platform parity | Отдельные платформенные сборки отсутствуют |
| R171-LOCAL01 | Online help / license UI — [MENU] | D: локальная справка, свободная лицензия, без активации | Намеренные требования OpenFrame; не воспроизводятся network/license механизмы | Проверка отсутствия updater/telemetry/browser launch; это не утверждение системной изоляции |

## Состояние проверок и блокеры

1. `test-results/packaged-long-media/long-media-report.json` проверяет собственные H.264/AAC 160×90: 2700@30000/1001 и 5400@60, около 90 секунд, GOP 900/1800. Последние 48/60 кадров экспортированы в FFV1/PCM; first/last RGB совпали с независимым system FFmpeg, PCM содержит сигнал и sample count верен. Это не многочасовой soak, не screenshot эталона и не измерение завершения GUI seek.
2. `test-results/packaged-integration/summary.json` относится к предыдущему SHA `F69DD70B…5824`: 55 facade checks, MPEG4/AAC/h264_mf/AAC и 60/120 Hz FakeMaya input. Успех coalescing не означает 60/120 отрисованных кадров в секунду.
3. Checkpoint 0.3.0 имеет [GUI03] 38/38 и [FILES03] 12/12: movie24, PNG24/257, один source на sequence, B/timecode, 17→1, movie sound on/off. Звук: 48001 PCM samples при 48 kHz, 439.9979 Hz, RMS 0.1414; silent AVI без audio stream. Это не проверка всех сценариев timeline sound и не результат эталона.
4. Независимый FFmpeg отказался декодировать EXIF в данных Maya PNG. Windows System.Drawing прочитал неизменённые оригиналы; first/last PNG и экспортированные BMP обеих sequences дали одинаковые RGB SHA256 после композиции на одинаковом чёрном фоне. Сравнение raw RGB без alpha было неверным oracle. Первоначальные отчёты `maya-files`/`maya-files-final` сохранены; окончательное доказательство — [FILES03], без удаления EXIF или изменения originals.
5. Цвет/битность проверены в 0.4.0 отдельно от физического планшета и длительного A/V soak. HDR CPU имеет измеренное ограничение скорости; оно не подменяется отсутствием эталонной проверки. Нельзя представлять missing implementation как всего лишь отсутствие comparative acceptance.
6. Для comparative acceptance нужен короткий журнал на каждую строку: версия/хеш эталона, hash synthetic fixture, точные действия, observed result обеих программ, допускаемая ошибка и приложенный отчёт. Пока такого журнала нет — comparative status **UNVERIFIED**, даже если собственные тесты T.

[CTEST03]: ../test-results/ctest-0.3.0-final.log
[GUI03]: ../test-results/packaged-0.3.0-wave2/maya-final/maya-report.json
[FILES03]: ../test-results/packaged-0.3.0-wave2/maya-files-opaque/capture-file-report.json
[FEATURES03]: ../test-results/packaged-0.3.0-wave2/features-final/feature-report.json
[PY04]: ../test-results/python-04-timeouts.txt
[API04]: ../test-results/api-04-tests-final.txt
[CTEST04]: ../test-results/package-0.4.0-final.log
[CORE04]: ../test-results/core-wave4-complete.txt
[COLOR04]: ../test-results/core-wave4-color.txt
[UI04]: ../test-results/ui-wave4-validation-summary.json
[PACKCOLOR04]: ../test-results/packaged-0.4.0-final/color/color-runtime-report.json
[DRAW04]: ../test-results/packaged-0.4.0-final/drawing/drawing-runtime-report.json
[GUI04]: ../test-results/packaged-0.4.0-final/maya/maya-report.json
[FILES04]: ../test-results/packaged-0.4.0-final/maya-files/capture-file-report.json

Proprietary project/wire formats остаются отдельным compatibility blocker без разрешённых fixture/публичного контракта. Их нельзя выдумывать, заявлять совместимыми или заполнять данными из binary analysis.

## Первичные источники

Ссылки проверены 2026-11-20. Ни один download исходного клиента/бинарника здесь не используется. Release-note feature labels выше кратко перефразированы; локальные выводы основаны на OpenFrame source/test evidence.

[PRODUCT]: https://zurbrigg.com/keyframe-pro-2
[RN]: https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-release-notes
[NEW]: https://zurbrigg.com/keyframe-pro-2-documentation/whats-new-in-keyframe-pro-2
[MENU]: https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-main-menu
[PREF]: https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-preferences
[PAN]: https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-pan-and-zoom
[SCRUB]: https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-scrubbing-and-frame-stepping
[STEP]: https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-stepped-playback
[ANNOT]: https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-annotations
[NOTES]: https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-notes
[KEYS]: https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-hotkey-manager
[EXPORT]: https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-export-to-file
[CLIENT]: https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-python-client-api-reference
[MAYA]: https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-maya-to-keyframe-pro-2-overview
[TOOLS]: https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-maya-tools-overview
