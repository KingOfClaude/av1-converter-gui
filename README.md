<p align="center">
  <img src="icon.png" alt="AV1 Batch Converter icon" width="128" height="128">
</p>

<h1 align="center">AV1 Batch Converter</h1>

<p align="center">
  A tiny native Windows GUI for batch-converting videos to <strong>AV1</strong> with <strong>ffmpeg</strong> and <strong>SVT-AV1</strong>.<br>
  One source file, no Qt, no frameworks, no runtime dependencies.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-CC0%201.0-lightgrey.svg" alt="License: CC0 1.0"></a>
  <img src="https://img.shields.io/badge/platform-Windows-blue.svg" alt="Platform: Windows">
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C.svg" alt="C++17">
</p>

---

## What it does

Point it at a folder, tick the videos you want, and it converts them to AV1 using ffmpeg's `libsvtav1` encoder. Converted files are written to an `output` sub-folder inside the folder you picked, so your originals are never touched.

## Features

- **Folder browser with file list** – shows every video file in the folder with its size; tick the ones you want, or use *Select all* / *Select none*.
- **Quality control** – CRF value (0–63, default `30`; lower = better quality and bigger files).
- **Speed preset** – SVT-AV1 presets 0–13 (default `6`, balanced).
- **Audio** – copy the original audio untouched, or re-encode to Opus at 128 kbps.
- **Resolution** – keep the original or scale down to 360p / 480p / 720p / 1080p / 1440p / 2160p. Smaller sources are never upscaled and the aspect ratio is preserved.
- **Delete if larger** – optionally discard a converted file when it ends up bigger than the original.
- **Safe by default** – existing output files are skipped, and cancelling mid-encode removes the partial file.
- **Live log and progress bar** – see exactly what ffmpeg is doing, with the size change for each finished file.
- **DPI aware** – crisp on high-DPI displays, with modern Windows visual styles.

Supported input extensions: `mp4`, `mkv`, `avi`, `mov`, `wmv`, `flv`, `webm`, `m4v`, `mpg`, `mpeg`, `ts`, `m2ts`, `mts`, `3gp`, `ogv`.

## Requirements

- Windows 10 or later
- **ffmpeg built with `libsvtav1`**. The "full" builds from [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) include it. Either add `ffmpeg.exe` to your `PATH` or enter its full path in the app.

The app checks for ffmpeg and the SVT-AV1 encoder before it starts converting and tells you if either is missing.

## Usage

1. Run `main.exe`.
2. Click **Browse…** next to *Input folder* and choose a folder with videos.
3. (Optional) Set the path to `ffmpeg.exe` if it isn't on your `PATH`.
4. Adjust quality, preset, audio and resolution.
5. Tick the files you want to convert and click **Start**.
6. Find the results in `<your folder>\output\`.

## Encoding details

Each file is converted with roughly this ffmpeg command:

```
ffmpeg -i input -map 0:V -map 0:a? -map 0:s? ^
       [-vf "scale=-2:'min(HEIGHT,ih)':flags=lanczos"] ^
       -c:v libsvtav1 -crf <CRF> -preset <PRESET> -pix_fmt yuv420p10le ^
       -c:s copy -c:a copy|"libopus -b:a 128k" ^
       output\name.mkv
```

- Output is always **Matroska (`.mkv`)**, which holds AV1, any audio codec and most subtitle formats.
- Video is encoded in **10-bit** (`yuv420p10le`) for better compression efficiency and less banding.
- All video, audio and subtitle streams are carried over; cover art (attached pictures) is not.

## Building

You need Visual Studio (or the Build Tools) with the C++ workload. From an **x64 Native Tools Command Prompt for VS**:

```
rc app.rc
cl /EHsc /std:c++17 /O2 main.cpp app.res
```

`rc app.rc` compiles the application icon into `app.res`, and the second command builds and links it into `main.exe`. Libraries are linked via `#pragma comment`, so no extra flags are needed.

## Project layout

| File | Purpose |
| --- | --- |
| `main.cpp` | The whole application (Win32 GUI + ffmpeg worker thread) |
| `app.rc` | Resource script that embeds the icon |
| `resource.h` | Resource IDs |
| `icon.ico` | Multi-size Windows icon (16–256 px) |
| `icon.png` | Icon for this README |

## License

This project is released into the public domain under the
[CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/) dedication. You can copy, modify, distribute and use it, even for commercial purposes, without asking permission. See [LICENSE](LICENSE) for the full text.

ffmpeg and SVT-AV1 are separate projects with their own licenses and are not included here.
