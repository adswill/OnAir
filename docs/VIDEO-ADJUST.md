# Brightness and contrast for analog TV and FPV (issue #239)

The Decoder section of the analog TV and FPV modes has Brightness (-1 to 1) and Contrast (0 to 3) sliders, a Reset button that appears when either is changed, and the values are saved per mode (`atvBright`, `atvContrast`, `fpvBright`, `fpvContrast`). They work on the copy of the picture sent to the screen, including the snow of an empty channel and full screen: the decoding, the saved pictures and the FPV recording are unchanged. Saturation was already there (a decoder setting).

Code: `core/include/dect2/video/display_adjust.h` (the 256-entry table, pure), `app/ui/video_adjust.h` (the sliders). Test `display_adjust` (identity, brightness, contrast, monotonic over a sweep, clamping, alpha kept): not yet run on CI.
