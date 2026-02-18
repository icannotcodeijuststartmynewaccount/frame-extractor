# Frame Extractor
Extract frames from videos using FFmpeg

## Features
- Extract single frame: `-frame 100`
- Extract multiple frames: `-frames 10,20,30`
- Extract range: `-range 100 200 -step 5`
- Time-based: `-time 00:01:30`
- Progress bar with ETA
- Precise time -time 00:01:30.500
- Time-range -time-range 00:01:00 00:01:30
- audio extraction only -audio-only
- audio bit rate (32-320 kbps) -audio-bitrate 128
- save raw YUV data -fast
- extract audio and picture -extract-audio
- audio format (mp3 as defualt) -audio-format


# Compile
gcc -o frame_extractor frame_extractor.c -lavformat -lavcodec -lavutil -lswscale -lpng
