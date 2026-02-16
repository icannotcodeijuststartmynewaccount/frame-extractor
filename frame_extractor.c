#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/parseutils.h>
#include <png.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

// ==================== TIMING ====================

#ifdef _WIN32
typedef LARGE_INTEGER Timer;
static double timer_frequency = 0.0;

static void timer_start(Timer* t) {
    if (timer_frequency == 0.0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        timer_frequency = (double)freq.QuadPart;
    }
    QueryPerformanceCounter(t);
}

static double timer_elapsed(Timer start) {
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    return (double)(end.QuadPart - start.QuadPart) / timer_frequency;
}
#else
typedef struct timeval Timer;

static void timer_start(Timer* t) {
    gettimeofday(t, NULL);
}

static double timer_elapsed(Timer start) {
    struct timeval end;
    gettimeofday(&end, NULL);
    return (end.tv_sec - start.tv_sec) + 
           (end.tv_usec - start.tv_usec) / 1000000.0;
}
#endif

// ==================== PROGRESS BAR ====================

typedef struct {
    Timer start_time;
    int total_frames;
    int frames_processed;
    int width;
    double last_display_time;
} ProgressTracker;

static void progress_init(ProgressTracker* pt, int total_frames) {
    timer_start(&pt->start_time);
    pt->total_frames = total_frames;
    pt->frames_processed = 0;
    pt->width = 50;
    pt->last_display_time = 0.0;
    
    printf("Extracting frames: %d total\n", total_frames);
}

static void progress_update(ProgressTracker* pt, int frames_increment) {
    pt->frames_processed += frames_increment;
    if (pt->frames_processed > pt->total_frames) {
        pt->frames_processed = pt->total_frames;
    }
    
    double elapsed = timer_elapsed(pt->start_time);
    
    if ((elapsed - pt->last_display_time) < 0.1 && 
        pt->frames_processed < pt->total_frames) {
        return;
    }
    pt->last_display_time = elapsed;
    
    float percentage = (float)pt->frames_processed / pt->total_frames;
    if (percentage > 1.0f) percentage = 1.0f;
    
    double frames_per_sec = (elapsed > 0.001) ? pt->frames_processed / elapsed : 0;
    double remaining_time = 0;
    if (percentage > 0.01 && frames_per_sec > 0) {
        remaining_time = (pt->total_frames - pt->frames_processed) / frames_per_sec;
    }
    
    printf("\r[");
    int pos = pt->width * percentage;
    for (int i = 0; i < pt->width; i++) {
        if (i < pos) printf("=");
        else if (i == pos) printf(">");
        else printf(" ");
    }
    printf("] %5.1f%%", percentage * 100);
    
    if (frames_per_sec > 1000) {
        printf(" | %.1f Kfps", frames_per_sec / 1000);
    } else {
        printf(" | %.1f fps", frames_per_sec);
    }
    
    if (remaining_time < 60) {
        printf(" | ETA: %.0fs", remaining_time);
    } else if (remaining_time < 3600) {
        printf(" | ETA: %.1fm", remaining_time / 60);
    } else {
        printf(" | ETA: %.1fh", remaining_time / 3600);
    }
    
    fflush(stdout);
}

static void progress_finish(ProgressTracker* pt) {
    progress_update(pt, pt->total_frames - pt->frames_processed);
    
    double elapsed = timer_elapsed(pt->start_time);
    double frames_per_sec = (elapsed > 0.001) ? pt->total_frames / elapsed : 0;
    
    printf("\n\nCompleted in %.2f seconds", elapsed);
    if (frames_per_sec > 1000) {
        printf(" (%.1f Kfps)\n", frames_per_sec / 1000);
    } else {
        printf(" (%.1f fps)\n", frames_per_sec);
    }
}

// ==================== PNG SAVING ====================

int save_png(const char* filename, uint8_t* image, int width, int height) {
    FILE *fp = fopen(filename, "wb"); 
    if (!fp) return 0;
    
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,0,0,0);
    if (!png) { fclose(fp); return 0; }
    
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png,0); fclose(fp); return 0; }
    
    if (setjmp(png_jmpbuf(png))) { 
        png_destroy_write_struct(&png,&info); 
        fclose(fp); 
        return 0; 
    }
    
    png_init_io(png, fp);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB, 
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, 
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    
    png_bytep rows[height];
    for (int y=0; y<height; y++) {
        rows[y] = image + y * width * 3;
    }
    
    png_write_image(png, rows);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 1;
}

// ==================== FRAME EXTRACTOR ====================

typedef struct {
    char input[512];
    char output_pattern[512];
    int frames[1024];
    int frame_count;
    int start_frame;
    int end_frame;
    int step;
    int format;
    int use_time;
    char time_str[64];
} Config;

void print_usage() {
    printf("\n=== Frame Extractor v2.0 ===\n");
    printf("Usage: frame_extractor -input <video> [options]\n\n");
    printf("Options:\n");
    printf("  -input <file>         Input video file\n");
    printf("  -output <pattern>     Output filename pattern (e.g., frame_%%03d.png)\n");
    printf("  -frame <n>            Extract single frame\n");
    printf("  -frames <n1,n2,n3>    Extract specific frames (comma-separated)\n");
    printf("  -range <start> <end>  Extract range of frames\n");
    printf("  -step <n>             Step for range extraction\n");
    printf("  -time <time>          Extract frame at time (e.g., 00:01:30)\n");
    printf("  -format <png/jpg/bmp> Output format (default: png)\n\n");
    printf("Examples:\n");
    printf("  frame_extractor -input video.mp4 -frame 100 -output frame_100.png\n");
    printf("  frame_extractor -input video.mp4 -frames 10,20,30 -output frame_%%d.png\n");
    printf("  frame_extractor -input video.mp4 -range 100 200 -step 5 -output frame_%%03d.png\n");
    printf("  frame_extractor -input video.mp4 -time 00:01:30 -output scene.png\n");
}

int parse_frames_string(const char* str, int* frames, int* count) {
    char temp[1024];
    strcpy(temp, str);
    *count = 0;
    
    char* token = strtok(temp, ",");
    while (token != NULL && *count < 1024) {
        frames[*count] = atoi(token);
        (*count)++;
        token = strtok(NULL, ",");
    }
    return *count > 0;
}

int parse_time_to_frame(const char* time_str, double fps) {
    int64_t seconds = 0;
    int h, m, s;
    
    if (sscanf(time_str, "%d:%d:%d", &h, &m, &s) == 3) {
        seconds = h * 3600 + m * 60 + s;
    } else if (sscanf(time_str, "%d:%d", &m, &s) == 2) {
        seconds = m * 60 + s;
    } else {
        seconds = atoi(time_str);
    }
    
    return (int)(seconds * fps);
}

void save_frame(AVFrame* frame, const char* filename, int width, int height, int format) {
    // Convert to RGB
    struct SwsContext* sws_ctx = sws_getContext(
        width, height, frame->format,
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL
    );
    
    if (!sws_ctx) {
        printf("Error: Cannot create sws context\n");
        return;
    }
    
    uint8_t* rgb_data = (uint8_t*)malloc(width * height * 3);
    uint8_t* rgb_ptrs[1] = {rgb_data};
    int rgb_linesize[1] = {width * 3};
    
    // FIX: Cast frame->data to const uint8_t* const*
    sws_scale(sws_ctx, (const uint8_t* const*)frame->data, frame->linesize, 0, height, rgb_ptrs, rgb_linesize);
    
    // Save based on format
    if (format == 0) { // PNG
        save_png(filename, rgb_data, width, height);
    } else if (format == 1) { // JPEG (simplified - just save as PNG for now)
        save_png(filename, rgb_data, width, height);
    }
    
    free(rgb_data);
    sws_freeContext(sws_ctx);
}

int main(int argc, char** argv) {
    // FIX: Properly initialize the config structure
    Config config;
    memset(&config, 0, sizeof(Config));
    strcpy(config.output_pattern, "frame_%d.png");
    config.step = 1;
    config.format = 0;
    
    // Parse command line
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-input") == 0 && i+1 < argc) {
            strcpy(config.input, argv[++i]);
        } else if (strcmp(argv[i], "-output") == 0 && i+1 < argc) {
            strcpy(config.output_pattern, argv[++i]);
        } else if (strcmp(argv[i], "-frame") == 0 && i+1 < argc) {
            config.frames[0] = atoi(argv[++i]);
            config.frame_count = 1;
        } else if (strcmp(argv[i], "-frames") == 0 && i+1 < argc) {
            parse_frames_string(argv[++i], config.frames, &config.frame_count);
        } else if (strcmp(argv[i], "-range") == 0 && i+2 < argc) {
            config.start_frame = atoi(argv[++i]);
            config.end_frame = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-step") == 0 && i+1 < argc) {
            config.step = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-time") == 0 && i+1 < argc) {
            strcpy(config.time_str, argv[++i]);
            config.use_time = 1;
        } else if (strcmp(argv[i], "-format") == 0 && i+1 < argc) {
            if (strcmp(argv[i+1], "jpg") == 0 || strcmp(argv[i+1], "jpeg") == 0) {
                config.format = 1;
            } else if (strcmp(argv[i+1], "bmp") == 0) {
                config.format = 2;
            } else {
                config.format = 0;
            }
            i++;
        } else if (strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        }
    }
    
    if (config.input[0] == '\0') {
        print_usage();
        return 1;
    }
    
    // Initialize FFmpeg
    avformat_network_init();
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    
    printf("Opening: %s\n", config.input);
    if (avformat_open_input(&fmt_ctx, config.input, NULL, NULL) != 0) {
        printf("Error: Cannot open file!\n");
        return 1;
    }
    
    avformat_find_stream_info(fmt_ctx, NULL);
    
    // Find video stream
    int video_stream = -1;
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = i;
            break;
        }
    }
    
    if (video_stream == -1) {
        printf("Error: No video stream found!\n");
        return 1;
    }
    
    // Get video info
    AVStream* stream = fmt_ctx->streams[video_stream];
    double fps = av_q2d(stream->avg_frame_rate);
    int total_frames = stream->nb_frames;
    
    printf("Video: %dx%d, %.2f fps, %d frames\n", 
           stream->codecpar->width, stream->codecpar->height, 
           fps, total_frames);
    
    // Handle time-based extraction
    if (config.use_time) {
        config.frames[0] = parse_time_to_frame(config.time_str, fps);
        config.frame_count = 1;
        printf("Time %s = frame %d\n", config.time_str, config.frames[0]);
    }
    
    // Initialize codec
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    avcodec_open2(codec_ctx, codec, NULL);
    
    // Prepare frame list
    int frames_to_extract[1024];
    int extract_count = 0;
    
    if (config.start_frame > 0 && config.end_frame > 0) {
        // Range extraction
        for (int f = config.start_frame; f <= config.end_frame && f < total_frames; f += config.step) {
            frames_to_extract[extract_count++] = f;
        }
        printf("Extracting frames from %d to %d, step %d\n", config.start_frame, config.end_frame, config.step);
    } else {
        // Single or list extraction
        for (int i = 0; i < config.frame_count; i++) {
            if (config.frames[i] < total_frames) {
                frames_to_extract[extract_count++] = config.frames[i];
            } else {
                printf("Warning: Frame %d exceeds video length (%d)\n", 
                       config.frames[i], total_frames);
            }
        }
    }
    
    if (extract_count == 0) {
        printf("Error: No valid frames to extract!\n");
        return 1;
    }
    
    printf("Extracting %d frames\n", extract_count);
    
    // Progress bar
    ProgressTracker progress;
    progress_init(&progress, extract_count);
    
    // Extract frames
    AVPacket packet;
    AVFrame* frame = av_frame_alloc();
    int current_frame = 0;
    int extracted = 0;
    int frame_index = 0;
    
    while (av_read_frame(fmt_ctx, &packet) >= 0 && extracted < extract_count) {
        if (packet.stream_index == video_stream) {
            avcodec_send_packet(codec_ctx, &packet);
            
            while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                // Check if current frame is in our list
                for (int i = 0; i < extract_count; i++) {
                    if (frames_to_extract[i] == current_frame) {
                        // Generate filename
                        char filename[512];
                        snprintf(filename, sizeof(filename), config.output_pattern, current_frame);
                        
                        // Add extension if not present
                        if (strstr(filename, ".png") == NULL && 
                            strstr(filename, ".jpg") == NULL && 
                            strstr(filename, ".bmp") == NULL) {
                            char with_ext[512];
                            if (config.format == 0) {
                                snprintf(with_ext, sizeof(with_ext), "%s.png", filename);
                            } else if (config.format == 1) {
                                snprintf(with_ext, sizeof(with_ext), "%s.jpg", filename);
                            } else {
                                snprintf(with_ext, sizeof(with_ext), "%s.bmp", filename);
                            }
                            strcpy(filename, with_ext);
                        }
                        
                        printf("\nSaving: %s\n", filename);
                        save_frame(frame, filename, 
                                 codec_ctx->width, codec_ctx->height, 
                                 config.format);
                        extracted++;
                        progress_update(&progress, 1);
                        break;
                    }
                }
                current_frame++;
            }
        }
        av_packet_unref(&packet);
    }
    
    progress_finish(&progress);
    
    // Cleanup
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    
    return 0;
}
