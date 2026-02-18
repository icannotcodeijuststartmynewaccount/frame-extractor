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
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#include <sys/wait.h>
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
    int audio_packets;
    int width;
    double last_display_time;
    pthread_mutex_t progress_mutex;
} ProgressTracker;

static void progress_init(ProgressTracker* pt, int total_frames) {
    timer_start(&pt->start_time);
    pt->total_frames = total_frames;
    pt->frames_processed = 0;
    pt->audio_packets = 0;
    pt->width = 50;
    pt->last_display_time = 0.0;
    pthread_mutex_init(&pt->progress_mutex, NULL);
}

static void progress_update(ProgressTracker* pt, int frames_inc, int audio_inc) {
    pthread_mutex_lock(&pt->progress_mutex);
    
    pt->frames_processed += frames_inc;
    pt->audio_packets += audio_inc;
    
    if (pt->frames_processed > pt->total_frames) {
        pt->frames_processed = pt->total_frames;
    }
    
    double elapsed = timer_elapsed(pt->start_time);
    
    if ((elapsed - pt->last_display_time) < 0.1 && 
        pt->frames_processed < pt->total_frames) {
        pthread_mutex_unlock(&pt->progress_mutex);
        return;
    }
    pt->last_display_time = elapsed;
    
    float percentage = (pt->total_frames > 0) ? 
                       (float)pt->frames_processed / pt->total_frames : 0;
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
    
    if (pt->total_frames > 0) {
        printf(" | Frames: %d/%d", pt->frames_processed, pt->total_frames);
    }
    if (pt->audio_packets > 0) {
        printf(" | Audio: %d packets", pt->audio_packets);
    }
    
    if (frames_per_sec > 1000) {
        printf(" | %.1f Kfps", frames_per_sec / 1000);
    } else if (frames_per_sec > 0) {
        printf(" | %.1f fps", frames_per_sec);
    }
    
    if (remaining_time > 0) {
        if (remaining_time < 60) {
            printf(" | ETA: %.0fs", remaining_time);
        } else if (remaining_time < 3600) {
            printf(" | ETA: %.1fm", remaining_time / 60);
        } else {
            printf(" | ETA: %.1fh", remaining_time / 3600);
        }
    }
    
    fflush(stdout);
    pthread_mutex_unlock(&pt->progress_mutex);
}

static void progress_finish(ProgressTracker* pt) {
    progress_update(pt, pt->total_frames - pt->frames_processed, 0);
    
    double elapsed = timer_elapsed(pt->start_time);
    
    printf("\n\n✅ Completed in %.2f seconds", elapsed);
    if (pt->total_frames > 0) {
        printf(" (%d frames)", pt->total_frames);
    }
    if (pt->audio_packets > 0) {
        printf(", %d audio packets", pt->audio_packets);
    }
    printf("\n");
    pthread_mutex_destroy(&pt->progress_mutex);
}

// ==================== MULTI-THREADING STRUCTURES ====================

#define MAX_QUEUE_SIZE 32
#define NUM_SAVER_THREADS 4

typedef struct {
    AVFrame* frames[MAX_QUEUE_SIZE];
    int frame_numbers[MAX_QUEUE_SIZE];
    int width;
    int height;
    int format;
    int fast_mode;
    char output_pattern[256];
    
    int head;
    int tail;
    int count;
    
    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
    
    int done;
    int frames_saved;
    int total_frames;
} FrameQueue;

typedef struct {
    FrameQueue* queue;
    ProgressTracker* progress;
    int thread_id;
} SaverThreadArgs;

// ==================== PNG SAVING ====================

int save_png(const char* filename, uint8_t* image, int width, int height) {
    FILE *fp = fopen(filename, "wb"); 
    if (!fp) return 0;
    
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
    if (!png) { fclose(fp); return 0; }
    
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, 0); fclose(fp); return 0; }
    
    if (setjmp(png_jmpbuf(png))) { 
        png_destroy_write_struct(&png, &info); 
        fclose(fp); 
        return 0; 
    }
    
    png_init_io(png, fp);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB, 
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, 
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    
    png_bytep rows[height];
    for (int y = 0; y < height; y++) {
        rows[y] = image + y * width * 3;
    }
    
    png_write_image(png, rows);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 1;
}

// ==================== RAW YUV SAVING ====================

void save_yuv_frame(AVFrame* frame, const char* filename, int width, int height) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) return;
    
    // Write Y plane
    for (int y = 0; y < height; y++) {
        fwrite(frame->data[0] + y * frame->linesize[0], 1, width, fp);
    }
    
    // Write U plane (half size for 4:2:0)
    for (int y = 0; y < height/2; y++) {
        fwrite(frame->data[1] + y * frame->linesize[1], 1, width/2, fp);
    }
    
    // Write V plane (half size for 4:2:0)
    for (int y = 0; y < height/2; y++) {
        fwrite(frame->data[2] + y * frame->linesize[2], 1, width/2, fp);
    }
    
    fclose(fp);
}

// ==================== FRAME QUEUE MANAGEMENT ====================

void queue_init(FrameQueue* q, int width, int height, int format, int fast_mode, 
                const char* pattern, int total_frames) {
    memset(q, 0, sizeof(FrameQueue));
    q->width = width;
    q->height = height;
    q->format = format;
    q->fast_mode = fast_mode;
    strcpy(q->output_pattern, pattern);
    q->total_frames = total_frames;
    
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

void queue_destroy(FrameQueue* q) {
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
}

void queue_push(FrameQueue* q, AVFrame* frame, int frame_number) {
    pthread_mutex_lock(&q->mutex);
    
    while (q->count >= MAX_QUEUE_SIZE) {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }
    
    q->frames[q->head] = av_frame_clone(frame);
    q->frame_numbers[q->head] = frame_number;
    q->head = (q->head + 1) % MAX_QUEUE_SIZE;
    q->count++;
    
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

int queue_pop(FrameQueue* q, AVFrame** frame, int* frame_number) {
    pthread_mutex_lock(&q->mutex);
    
    while (q->count == 0 && !q->done) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }
    
    if (q->count == 0 && q->done) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }
    
    *frame = q->frames[q->tail];
    *frame_number = q->frame_numbers[q->tail];
    q->tail = (q->tail + 1) % MAX_QUEUE_SIZE;
    q->count--;
    
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

void queue_set_done(FrameQueue* q) {
    pthread_mutex_lock(&q->mutex);
    q->done = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

// ==================== FRAME SAVER THREAD ====================

void* frame_saver_thread(void* arg) {
    SaverThreadArgs* args = (SaverThreadArgs*)arg;
    FrameQueue* q = args->queue;
    ProgressTracker* progress = args->progress;
    
    AVFrame* frame;
    int frame_number;
    
    while (queue_pop(q, &frame, &frame_number)) {
        char filename[512];
        snprintf(filename, sizeof(filename), q->output_pattern, frame_number);
        
        if (q->fast_mode) {
            if (strstr(filename, ".yuv") == NULL) {
                char with_ext[512];
                snprintf(with_ext, sizeof(with_ext), "%s.yuv", filename);
                strcpy(filename, with_ext);
            }
            save_yuv_frame(frame, filename, q->width, q->height);
        } else {
            if (strstr(filename, ".png") == NULL) {
                char with_ext[512];
                snprintf(with_ext, sizeof(with_ext), "%s.png", filename);
                strcpy(filename, with_ext);
            }
            
            struct SwsContext* sws_ctx = sws_getContext(
                q->width, q->height, frame->format,
                q->width, q->height, AV_PIX_FMT_RGB24,
                SWS_BILINEAR, NULL, NULL, NULL
            );
            
            if (sws_ctx) {
                uint8_t* rgb_data = (uint8_t*)malloc(q->width * q->height * 3);
                uint8_t* rgb_ptrs[1] = {rgb_data};
                int rgb_linesize[1] = {q->width * 3};
                
                sws_scale(sws_ctx, (const uint8_t* const*)frame->data, frame->linesize, 
                         0, q->height, rgb_ptrs, rgb_linesize);
                
                save_png(filename, rgb_data, q->width, q->height);
                
                free(rgb_data);
                sws_freeContext(sws_ctx);
            }
        }
        
        av_frame_free(&frame);
        q->frames_saved++;
        progress_update(progress, 1, 0);
    }
    
    return NULL;
}

// ==================== MEDIA EXTRACTOR CONFIG ====================

typedef struct {
    char input[512];
    char output_pattern[512];
    char audio_output[512];
    int frames[1024];
    int frame_count;
    int start_frame;
    int end_frame;
    int step;
    int format;
    int fast_mode;
    int extract_audio;
    int audio_only;
    int audio_format;
    int audio_bitrate;        // NEW: bitrate for audio (kbps)
    int use_time;
    int use_time_range;
    char time_str[64];
    char start_time[64];
    char end_time[64];
} Config;

void print_usage() {
    printf("\n🎬 === Frame Extractor v8.0 (MULTI-THREADED + BITRATE) ===\n");
    printf("Extract frames with %d threads + FFmpeg audio with custom bitrate!\n\n", NUM_SAVER_THREADS);
    printf("Usage: frame_extractor -input <video> [options]\n\n");
    printf("📹 FRAME OPTIONS:\n");
    printf("  -output <pattern>     Output filename pattern (e.g., frame_%%03d.png)\n");
    printf("  -frame <n>            Extract single frame\n");
    printf("  -frames <n1,n2,n3>    Extract specific frames\n");
    printf("  -range <start> <end>  Extract range of frames\n");
    printf("  -step <n>             Step for range extraction\n");
    printf("  -time <time>          Extract frame at time\n");
    printf("  -time-range <start> <end>  Extract frames between times\n");
    printf("  -fast                  FAST MODE: save raw YUV\n\n");
    
    printf("🎵 AUDIO OPTIONS (USES FFMPEG!):\n");
    printf("  -extract-audio        Extract audio along with frames\n");
    printf("  -audio-only           Extract audio only (no frames)\n");
    printf("  -output-audio <file>  Audio output filename\n");
    printf("  -audio-format <fmt>   mp3, aac, wav, ogg (default: mp3)\n");
    printf("  -audio-bitrate <kbps> Bitrate for audio (e.g., 128, 192, 320) (default: 128)\n\n");
    
    printf("🎚️  BITRATE EXAMPLES:\n");
    printf("  # Low quality (64 kbps) - smaller file\n");
    printf("  frame_extractor -input video.mp4 -audio-only -audio-bitrate 64 -output-audio small.mp3\n\n");
    printf("  # High quality (320 kbps) - best sound\n");
    printf("  frame_extractor -input video.mp4 -audio-only -audio-bitrate 320 -output-audio best.mp3\n\n");
    
    printf("🚀 FULL EXAMPLES:\n");
    printf("  # Extract frames with %d threads + 320kbps audio:\n", NUM_SAVER_THREADS);
    printf("  frame_extractor -input video.mp4 -time-range 00:02 00:03 \\\n");
    printf("                  -output frame_%%03d.png -extract-audio \\\n");
    printf("                  -output-audio clip.mp3 -audio-bitrate 320\n");
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
    int h, m, s;
    double ms = 0.0;
    double seconds = 0;
    
    if (sscanf(time_str, "%d:%d:%d.%lf", &h, &m, &s, &ms) == 4) {
        seconds = h * 3600 + m * 60 + s + ms;
    } else if (sscanf(time_str, "%d:%d:%d", &h, &m, &s) == 3) {
        seconds = h * 3600 + m * 60 + s;
    } else if (sscanf(time_str, "%d:%d", &m, &s) == 2) {
        seconds = m * 60 + s;
    } else {
        seconds = atof(time_str);
    }
    
    return (int)(seconds * fps);
}

int64_t time_to_pts(const char* time_str, AVRational time_base) {
    int h, m, s;
    double ms = 0.0;
    double seconds = 0;
    
    if (sscanf(time_str, "%d:%d:%d.%lf", &h, &m, &s, &ms) == 4) {
        seconds = h * 3600 + m * 60 + s + ms;
    } else if (sscanf(time_str, "%d:%d:%d", &h, &m, &s) == 3) {
        seconds = h * 3600 + m * 60 + s;
    } else if (sscanf(time_str, "%d:%d", &m, &s) == 2) {
        seconds = m * 60 + s;
    } else {
        seconds = atof(time_str);
    }
    
    return (int64_t)(seconds * time_base.den / time_base.num);
}

int frame_in_list(int frame, int* list, int count) {
    for (int i = 0; i < count; i++) {
        if (list[i] == frame) return 1;
    }
    return 0;
}

// ==================== FFMPEG AUDIO EXTRACTION WITH BITRATE ====================

const char* get_audio_codec(int format) {
    switch(format) {
        case 0: return "libmp3lame";
        case 1: return "aac";
        case 2: return "pcm_s16le";
        case 3: return "libvorbis";
        default: return "libmp3lame";
    }
}

const char* get_audio_extension(int format) {
    switch(format) {
        case 0: return "mp3";
        case 1: return "m4a";
        case 2: return "wav";
        case 3: return "ogg";
        default: return "mp3";
    }
}

const char* get_bitrate_option(int format, int bitrate) {
    static char option[64];
    
    if (bitrate <= 0) bitrate = 128;  // Default
    
    switch(format) {
        case 0:  // MP3
            snprintf(option, sizeof(option), "-b:a %dk", bitrate);
            break;
        case 1:  // AAC
            snprintf(option, sizeof(option), "-b:a %dk", bitrate);
            break;
        case 2:  // WAV (uncompressed, bitrate ignored)
            return "";
        case 3:  // OGG Vorbis
            snprintf(option, sizeof(option), "-b:a %dk", bitrate);
            break;
        default:
            return "";
    }
    return option;
}

void* extract_audio_thread(void* arg) {
    Config* config = (Config*)arg;
    
    printf("\n🎵 === AUDIO EXTRACTION (FFmpeg) ===\n");
    
    if (system("ffmpeg -version > /dev/null 2>&1") != 0) {
        printf("❌ FFmpeg not found! Install with: pkg install ffmpeg\n");
        return NULL;
    }
    
    const char* codec = get_audio_codec(config->audio_format);
    const char* ext = get_audio_extension(config->audio_format);
    const char* bitrate_opt = get_bitrate_option(config->audio_format, config->audio_bitrate);
    
    char final_output[512];
    strcpy(final_output, config->audio_output);
    if (strstr(final_output, ".") == NULL) {
        snprintf(final_output, sizeof(final_output), "%s.%s", config->audio_output, ext);
    }
    
    printf("📂 Input: %s\n", config->input);
    printf("📂 Output: %s\n", final_output);
    printf("🎵 Format: %s (%s)\n", ext, codec);
    if (config->audio_bitrate > 0) {
        printf("🎚️  Bitrate: %d kbps\n", config->audio_bitrate);
    }
    
    char time_option[256] = "";
    if (config->use_time_range) {
        double start_seconds = 0, end_seconds = 0;
        int h, m, s;
        
        if (sscanf(config->start_time, "%d:%d:%d", &h, &m, &s) == 3) {
            start_seconds = h * 3600 + m * 60 + s;
        } else if (sscanf(config->start_time, "%d:%d", &m, &s) == 2) {
            start_seconds = m * 60 + s;
        } else {
            start_seconds = atof(config->start_time);
        }
        
        if (sscanf(config->end_time, "%d:%d:%d", &h, &m, &s) == 3) {
            end_seconds = h * 3600 + m * 60 + s;
        } else if (sscanf(config->end_time, "%d:%d", &m, &s) == 2) {
            end_seconds = m * 60 + s;
        } else {
            end_seconds = atof(config->end_time);
        }
        
        double duration = end_seconds - start_seconds;
        if (duration > 0) {
            snprintf(time_option, sizeof(time_option), 
                     "-ss %.3f -t %.3f", start_seconds, duration);
            printf("⏱️ Time range: %.2fs to %.2fs (duration: %.2fs)\n", 
                   start_seconds, end_seconds, duration);
        }
    } else if (config->use_time) {
        double seconds = atof(config->time_str);
        snprintf(time_option, sizeof(time_option), "-ss %.3f -t 0.04", seconds);
        printf("⏱️ Time: %.2fs\n", seconds);
    }
    
    char command[4096];
    snprintf(command, sizeof(command),
             "ffmpeg -i \"%s\" %s -vn %s -acodec %s -y \"%s\" 2>&1",
             config->input, time_option, bitrate_opt, codec, final_output);
    
    printf("\n🔄 Running FFmpeg...\n");
    fflush(stdout);
    
    FILE* fp = popen(command, "r");
    if (!fp) {
        printf("❌ Failed to run FFmpeg\n");
        return NULL;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, "size=") || strstr(line, "time=")) {
            char* time_str = strstr(line, "time=");
            if (time_str) {
                time_str += 5;
                char* end = strchr(time_str, ' ');
                if (end) *end = '\0';
                printf("\r   ⏱️  %s", time_str);
                fflush(stdout);
            }
            
            char* size_str = strstr(line, "size=");
            if (size_str && strstr(line, "kB")) {
                size_str += 5;
                char* end = strchr(size_str, 'k');
                if (end) *end = '\0';
                printf(" | 📦 %s KB", size_str);
                fflush(stdout);
            }
        }
    }
    
    int status = pclose(fp);
    
    if (status == 0) {
        printf("\n\n✅ Audio extraction complete!\n");
        
        char info[512];
        snprintf(info, sizeof(info), "ls -lh \"%s\"", final_output);
        system(info);
        
        if (config->audio_bitrate > 0) {
            printf("🎚️  Bitrate used: %d kbps\n", 