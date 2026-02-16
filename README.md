# Frame Extractor
Extract frames from videos using FFmpeg

## Features
- Extract single frame: `-frame 100`
- Extract multiple frames: `-frames 10,20,30`
- Extract range: `-range 100 200 -step 5`
- Time-based: `-time 00:01:30`
- Progress bar with ETA

## Compile
gcc -o frame_extractor frame_extractor.c -lavformat -lavcodec -lavutil -lswscale -lpng
