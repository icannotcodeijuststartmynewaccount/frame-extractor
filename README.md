# Frame Extractor
Extract frames from videos using FFmpeg

## Features
- Extract single frame: `-frame 100`
- Extract multiple frames: `-frames 10,20,30`
- Extract range: `-range 100 200 -step 5`
- Time-based: `-time 00:01:30`
- Progress bar with ETA
- Precise time `-time 00:01:30.500`
- Time-range `-time-range 00:01:00 00:01:30`
- audio extraction only `-audio-only`
- audio bit rate (32-320 kbps) `-audio-bitrate 128`
- save raw YUV data `-fast`
- extract audio and picture `-extract-audio`
- audio format (mp3 as defualt) `-audio-format`


## Compilation

### On Termux (Android)
`
pkg install clang ffmpeg libpng
clang -O3 -o frame_extractor frame_extractor_v10.c \
    -I/data/data/com.termux/files/usr/include \
    -L/data/data/com.termux/files/usr/lib \
    -lavcodec -lavformat -lavutil -lswscale -lpng -lm -lpthread
`

### On Windows (MinGW)
`
gcc -O3 -o frame_extractor.exe frame_extractor_v10.c ^
    -I"C:\msys64\mingw64\include" ^
    -L"C:\msys64\mingw64\lib" ^
    -lavcodec -lavformat -lavutil -lswscale -lpng -lm -lpthread
`

### On Linux
`
sudo apt install ffmpeg libavcodec-dev libavformat-dev \
                 libavutil-dev libswscale-dev libpng-dev
gcc -O3 -o frame_extractor frame_extractor_v10.c \
    -lavcodec -lavformat -lavutil -lswscale -lpng -lm -lpthread
`
