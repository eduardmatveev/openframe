# UI asset provenance

OpenFrame's icons are drawn programmatically in `src/ui/Theme.cpp` using basic Qt lines, polygons and text. The palette and application styling are independently authored in the same file. No original-product image, stylesheet, binary or icon has been copied or embedded.

The UI implementation uses the task specification and publicly available feature descriptions at:

- https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-main-menu
- https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-annotations
- https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-notes
- https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-preferences
- https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-hotkey-manager
- https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-export-to-file
- https://zurbrigg.com/keyframe-pro-2-documentation/kpro2-pan-and-zoom

`--render-preview` generates original synthetic animation frames with Qt primitives, imports them through the media engine, captures the application's own UI, and records command checks in JSON. These captures are not reference screenshots.

Pixel-level similarity and complete command/behavior parity require a controlled licensed-reference comparison and are not claimed by these assets or tests.

The four small chevron SVGs in "resources/arrows" are independently authored geometric paths for high-DPI combo/spin controls. They are compiled into the application through "src/ui/UiResources.qrc". Qt 6.8 subcontrol syntax was checked against https://doc.qt.io/qt-6.8/stylesheet-examples.html; no example artwork was reused.
