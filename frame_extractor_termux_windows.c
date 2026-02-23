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
#include <math.h>
#include <dirent.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#define MKDIR(p) _mkdir(p)
#else
#include <sys/time.h>
#include <sys/wait.h>
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#define MKDIR(p) mkdir(p, 0777)
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
    char output_pattern[512];
    
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
    
    for (int y = 0; y < height; y++) {
        fwrite(frame->data[0] + y * frame->linesize[0], 1, width, fp);
    }
    
    for (int y = 0; y < height/2; y++) {
        fwrite(frame->data[1] + y * frame->linesize[1], 1, width/2, fp);
    }
    
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
    int audio_bitrate;
    int use_time;
    int use_time_range;
    char time_str[64];
    char start_time[64];
    char end_time[64];
    char ytdl_url[1024];      // New: YouTube URL
    char ytdl_format[64];      // New: yt-dlp format
    int ytdl_download;         // New: flag for yt-dlp
} Config;

void print_usage() {
    printf("\n🎬 === Frame Extractor v10.0 (YOUTUBE EDITION) ===\n");
    printf("Extract frames/audio from local videos OR YouTube URLs!\n\n");
    printf("Usage: frame_extractor -input <video> [options]\n");
    printf("   OR: frame_extractor -ytdl <url> [options]\n\n");
    
    printf("📹 FRAME OPTIONS:\n");
    printf("  -output <pattern>     Output filename pattern (e.g., frame_%%03d.png)\n");
    printf("  -frame <n>            Extract single frame\n");
    printf("  -frames <n1,n2,n3>    Extract specific frames\n");
    printf("  -range <start> <end>  Extract range of frames\n");
    printf("  -step <n>             Step for range extraction\n");
    printf("  -time <time>          Extract frame at time\n");
    printf("  -time-range <start> <end>  Extract frames between times\n");
    printf("  -fast                  FAST MODE: save raw YUV\n\n");
    
    printf("🎵 AUDIO OPTIONS:\n");
    printf("  -extract-audio        Extract audio along with frames\n");
    printf("  -audio-only           Extract audio only\n");
    printf("  -output-audio <file>  Audio output filename\n");
    printf("  -audio-format <fmt>   mp3, aac, wav, ogg (default: mp3)\n");
    printf("  -audio-bitrate <kbps> Bitrate for audio (32-320, default: 128)\n\n");
    
    printf("🌐 YOUTUBE OPTIONS (NEW!):\n");
    printf("  -ytdl <url>           Download from YouTube first\n");
    printf("  -ytdl-format <fmt>    yt-dlp format (default: auto)\n");
    printf("                         auto = best for your needs\n\n");
    
    printf("🚀 YOUTUBE EXAMPLES:\n");
    printf("  # Download and extract audio\n");
    printf("  frame_extractor -ytdl \"https://youtu.be/...\" -audio-only -output-audio song.mp3\n\n");
    printf("  # Download and extract frame at 1:30\n");
    printf("  frame_extractor -ytdl \"https://youtu.be/...\" -time 00:01:30 -output thumbnail.png\n\n");
    printf("  # Download best video, extract frames 100-200\n");
    printf("  frame_extractor -ytdl \"https://youtu.be/...\" -range 100 200 -output frames/%%03d.png\n");
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

// ==================== PATH FIXING FOR WINDOWS ====================

void fix_windows_path(char* path) {
    for (int i = 0; path[i]; i++) {
        if (path[i] == '\\') {
            path[i] = '/';
        }
    }
}

// ==================== YT-DLP DOWNLOAD FUNCTION ====================

int download_from_youtube(Config* config) {
    printf("\n🌐 === YOUTUBE DOWNLOAD ===\n");
    
    // Check if yt-dlp is installed
    if (system("yt-dlp --version > /dev/null 2>&1") != 0) {
        printf("❌ yt-dlp not found! Install with: pip install yt-dlp\n");
        return 0;
    }
    
    // Smart format selection
    const char* format;
    if (config->audio_only) {
        format = "bestaudio";
        printf("🎵 Audio-only mode: downloading best audio\n");
    }
    else if (config->frame_count > 0 || config->use_time_range || config->start_frame > 0) {
        format = "bestvideo[ext=mp4]";
        printf("📹 Frame extraction: downloading best video\n");
    }
    else if (config->extract_audio) {
        format = "bestvideo+bestaudio";
        printf("🎬 Video+Audio: downloading both\n");
    }
    else {
        format = "best[ext=mp4]";
        printf("📥 Default: downloading best MP4\n");
    }
    
    // Use custom format if specified
    if (config->ytdl_format[0] != '\0') {
        format = config->ytdl_format;
        printf("📥 Using custom format: %s\n", format);
    }
    
    // Build download command
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "yt-dlp -f '%s' --newline --progress -o 'ytdl_%%(title)s.%%(ext)s' '%s'",
             format, config->ytdl_url);
    
    printf("\n🔄 Downloading...\n");
    fflush(stdout);
    
    // Run download and show progress
    FILE* fp = popen(cmd, "r");
    if (!fp) {
        printf("❌ Failed to run yt-dlp\n");
        return 0;
    }
    
    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, "[download]") && strstr(line, "%")) {
            // Extract percentage
            char* percent_str = strstr(line, " ");
            if (percent_str) {
                percent_str++;
                char* end = strchr(percent_str, '%');
                if (end) *end = '\0';
                printf("\r   ⬇️  Downloaded: %s%%", percent_str);
                fflush(stdout);
            }
        }
    }
    
    int status = pclose(fp);
    if (status != 0) {
        printf("\n❌ Download failed!\n");
        return 0;
    }
    
    printf("\r   ⬇️  Download complete!          \n");
    
    // Find the downloaded file
    FILE* ls_fp = popen("ls ytdl_* 2>/dev/null | head -1", "r");
    if (!ls_fp) {
        printf("❌ Cannot find downloaded file\n");
        return 0;
    }
    
    if (fgets(config->input, sizeof(config->input), ls_fp) == NULL) {
        printf("❌ No downloaded file found\n");
        pclose(ls_fp);
        return 0;
    }
    
    // Remove newline
    config->input[strcspn(config->input, "\n")] = '\0';
    pclose(ls_fp);
    
    printf("📂 Downloaded: %s\n", config->input);
    return 1;
}

// ==================== FFMPEG AUDIO EXTRACTION ====================

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
    if (bitrate <= 0) bitrate = 128;
    snprintf(option, sizeof(option), "-b:a %dk", bitrate);
    return option;
}

void* extract_audio_thread(void* arg) {
    Config* config = (Config*)arg;
    
    printf("\n🎵 === AUDIO EXTRACTION (FFmpeg) ===\n");
    
    // Check if ffmpeg is installed
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
        char temp[512];
        snprintf(temp, sizeof(temp), "%s.%s", config->audio_output, ext);
        strcpy(final_output, temp);
    }
    
    char fixed_input[512];
    strcpy(fixed_input, config->input);
    fix_windows_path(fixed_input);
    fix_windows_path(final_output);
    
    printf("📂 Input: %s\n", fixed_input);
    printf("📂 Output: %s\n", final_output);
    printf("🎵 Format: %s (%s)\n", ext, codec);
    if (config->audio_bitrate > 0) printf("🎚️ Bitrate: %d kbps\n", config->audio_bitrate);
    
    char time_option[256] = "";
    if (config->use_time_range) {
        double start_seconds = 0, end_seconds = 0;
        int h, m, s;
        
        if (sscanf(config->start_time, "%d:%d:%d", &h, &m, &s) == 3) start_seconds = h * 3600 + m * 60 + s;
        else if (sscanf(config->start_time, "%d:%d", &m, &s) == 2) start_seconds = m * 60 + s;
        else start_seconds = atof(config->start_time);
        
        if (sscanf(config->end_time, "%d:%d:%d", &h, &m, &s) == 3) end_seconds = h * 3600 + m * 60 + s;
        else if (sscanf(config->end_time, "%d:%d", &m, &s) == 2) end_seconds = m * 60 + s;
        else end_seconds = atof(config->end_time);
        
        double duration = end_seconds - start_seconds;
        if (duration > 0) {
            snprintf(time_option, sizeof(time_option), "-ss %.3f -t %.3f", start_seconds, duration);
            printf("⏱️ Time range: %.2fs to %.2fs\n", start_seconds, end_seconds);
        }
    }
    
    char command[8192];
    snprintf(command, sizeof(command),
             "ffmpeg -i \"%s\" %s -vn %s -acodec %s -y \"%s\" 2>&1",
             fixed_input, time_option, bitrate_opt, codec, final_output);
    
    printf("\n🔄 Running FFmpeg...\n");
    fflush(stdout);
    
    FILE* fp = popen(command, "r");
    if (!fp) { printf("❌ Failed to run FFmpeg\n"); return NULL; }
    
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, "size=") || strstr(line, "time=")) {
            char* time_str = strstr(line, "time=");
            if (time_str) {
                time_str += 5;
                char* end = strchr(time_str, ' ');
                if (end) *end = '\0';
                printf("\r   ⏱️ %s", time_str);
                fflush(stdout);
            }
        }
    }
    
    int status = pclose(fp);
    if (status == 0) printf("\n\n✅ Audio extraction complete!\n");
    else printf("\n\n❌ FFmpeg failed with code %d\n", status);
    
    return NULL;
}

// ==================== MAIN ====================

int main(int argc, char** argv) {
    // Silence FFmpeg warnings
    av_log_set_level(AV_LOG_QUIET);
    
    Config config;
    memset(&config, 0, sizeof(Config));
    strcpy(config.output_pattern, "frame_%d.png");
    strcpy(config.audio_output, "audio");
    config.step = 1;
    config.format = 0;
    config.fast_mode = 0;
    config.extract_audio = 0;
    config.audio_only = 0;
    config.audio_format = 0;
    config.audio_bitrate = 128;
    config.ytdl_download = 0;
    
    printf("\n🎬 Frame Extractor v10.0 (YOUTUBE EDITION)\n");
    printf("==========================================\n");
    
    // Parse command line
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-input") == 0 && i + 1 < argc) {
            strcpy(config.input, argv[++i]);
        } else if (strcmp(argv[i], "-output") == 0 && i + 1 < argc) {
            strcpy(config.output_pattern, argv[++i]);
        } else if (strcmp(argv[i], "-output-audio") == 0 && i + 1 < argc) {
            strcpy(config.audio_output, argv[++i]);
        } else if (strcmp(argv[i], "-audio-format") == 0 && i + 1 < argc) {
            if (strcmp(argv[i+1], "mp3") == 0) config.audio_format = 0;
            else if (strcmp(argv[i+1], "aac") == 0) config.audio_format = 1;
            else if (strcmp(argv[i+1], "wav") == 0) config.audio_format = 2;
            else if (strcmp(argv[i+1], "ogg") == 0) config.audio_format = 3;
            i++;
        } else if (strcmp(argv[i], "-audio-bitrate") == 0 && i + 1 < argc) {
            config.audio_bitrate = atoi(argv[++i]);
            if (config.audio_bitrate < 32) config.audio_bitrate = 32;
            if (config.audio_bitrate > 320) config.audio_bitrate = 320;
        } else if (strcmp(argv[i], "-frame") == 0 && i + 1 < argc) {
            config.frames[0] = atoi(argv[++i]);
            config.frame_count = 1;
        } else if (strcmp(argv[i], "-frames") == 0 && i + 1 < argc) {
            parse_frames_string(argv[++i], config.frames, &config.frame_count);
        } else if (strcmp(argv[i], "-range") == 0 && i + 2 < argc) {
            config.start_frame = atoi(argv[++i]);
            config.end_frame = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-step") == 0 && i + 1 < argc) {
            config.step = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-time") == 0 && i + 1 < argc) {
            strcpy(config.time_str, argv[++i]);
            config.use_time = 1;
        } else if (strcmp(argv[i], "-time-range") == 0 && i + 2 < argc) {
            strcpy(config.start_time, argv[++i]);
            strcpy(config.end_time, argv[++i]);
            config.use_time_range = 1;
        } else if (strcmp(argv[i], "-fast") == 0) {
            config.fast_mode = 1;
        } else if (strcmp(argv[i], "-extract-audio") == 0) {
            config.extract_audio = 1;
        } else if (strcmp(argv[i], "-audio-only") == 0) {
            config.audio_only = 1;
        } else if (strcmp(argv[i], "-ytdl") == 0 && i + 1 < argc) {
            strcpy(config.ytdl_url, argv[++i]);
            config.ytdl_download = 1;
        } else if (strcmp(argv[i], "-ytdl-format") == 0 && i + 1 < argc) {
            strcpy(config.ytdl_format, argv[++i]);
        } else if (strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        }
    }
    
    // ===== YOUTUBE DOWNLOAD =====
    if (config.ytdl_download) {
        if (!download_from_youtube(&config)) {
            return 1;
        }
    }
    
    // Check if we have an input file
    if (config.input[0] == '\0') {
        print_usage();
        return 1;
    }
    
    fix_windows_path(config.input);
    
    // ===== AUDIO-ONLY MODE =====
    if (config.audio_only) {
        extract_audio_thread(&config);
        return 0;
    }
    
    // ===== FRAME EXTRACTION =====
    
    avformat_network_init();
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    
    printf("📂 Opening: %s\n", config.input);
    if (avformat_open_input(&fmt_ctx, config.input, NULL, NULL) != 0) {
        printf("❌ Error: Cannot open file!\n");
        return 1;
    }
    
    avformat_find_stream_info(fmt_ctx, NULL);
    
    int video_stream_idx = -1;
    
    printf("\n🔍 Scanning streams:\n");
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* stream = fmt_ctx->streams[i];
        const char* type = "unknown";
        
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            type = "VIDEO";
            video_stream_idx = i;
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            type = "AUDIO";
        }
        
        printf("   Stream %d: %s\n", i, type);
    }
    
    if (video_stream_idx == -1) {
        printf("❌ No video stream found!\n");
        return 1;
    }
    
    AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
    double fps = av_q2d(video_stream->avg_frame_rate);
    int total_frames = video_stream->nb_frames;
    int width = video_stream->codecpar->width;
    int height = video_stream->codecpar->height;
    
    printf("\n📹 Video: stream %d, %dx%d, %.2f fps, %d frames\n", 
           video_stream_idx, width, height, fps, total_frames);
    
    // ===== FIX FOR VIDEOS WITH NO FRAME COUNT =====
    if (total_frames <= 0) {
        printf("\n⚠️  Warning: Video has no frame count in header\n");
        
        double duration = 0;
        if (fmt_ctx->duration > 0) {
            duration = fmt_ctx->duration / (double)AV_TIME_BASE;
        }
        if (duration <= 0 && video_stream->duration > 0) {
            duration = video_stream->duration * av_q2d(video_stream->time_base);
        }
        
        if (duration > 0) {
            double exact_frames = duration * fps;
            total_frames = (int)ceil(exact_frames);
            printf("   Duration: %.3f seconds\n", duration);
            printf("   Calculated frames: %.3f → %d frames\n", exact_frames, total_frames);
        } else {
            printf("❌ Cannot determine video duration!\n");
            return 1;
        }
    }
    
    int start_frame = 0, end_frame = total_frames - 1;
    
    if (config.use_time) {
        start_frame = parse_time_to_frame(config.time_str, fps);
        end_frame = start_frame;
        printf("\n⏱️ Time %s = frame %d\n", config.time_str, start_frame);
    } else if (config.use_time_range) {
        start_frame = parse_time_to_frame(config.start_time, fps);
        end_frame = parse_time_to_frame(config.end_time, fps);
        printf("\n⏱️ Time range %s to %s = frames %d to %d\n", 
               config.start_time, config.end_time, start_frame, end_frame);
    } else if (config.start_frame > 0 || config.end_frame > 0) {
        if (config.start_frame > 0) start_frame = config.start_frame;
        if (config.end_frame > 0) end_frame = config.end_frame;
        printf("\n📹 Frame range: %d to %d\n", start_frame, end_frame);
    }
    
    if (start_frame < 0) start_frame = 0;
    if (end_frame >= total_frames) end_frame = total_frames - 1;
    
    int frames_to_extract[4096];
    int extract_count = 0;
    
    if (config.frame_count > 0) {
        for (int i = 0; i < config.frame_count; i++) {
            if (config.frames[i] >= start_frame && config.frames[i] <= end_frame) {
                frames_to_extract[extract_count++] = config.frames[i];
            }
        }
        printf("📋 Extracting %d specific frames\n", extract_count);
    } else {
        for (int f = start_frame; f <= end_frame; f += config.step) {
            frames_to_extract[extract_count++] = f;
        }
        printf("📋 Extracting %d frames (range %d-%d, step %d)\n", 
               extract_count, start_frame, end_frame, config.step);
    }
    
    if (extract_count == 0) {
        printf("❌ No frames to extract!\n");
        return 1;
    }
    
    const AVCodec* codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, video_stream->codecpar);
    
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        printf("❌ Failed to open video codec\n");
        return 1;
    }
    
    AVFrame* frame = av_frame_alloc();
    
    if (start_frame > 0) {
        int64_t seek_pts = av_rescale_q(start_frame, 
                                        (AVRational){1, (int)fps},
                                        video_stream->time_base);
        av_seek_frame(fmt_ctx, video_stream_idx, seek_pts, AVSEEK_FLAG_BACKWARD);
    }
    
    FrameQueue frame_queue;
    queue_init(&frame_queue, width, height, config.format, config.fast_mode, 
               config.output_pattern, extract_count);
    
    ProgressTracker progress;
    progress_init(&progress, extract_count);
    
    pthread_t saver_threads[NUM_SAVER_THREADS];
    SaverThreadArgs thread_args[NUM_SAVER_THREADS];
    
    for (int i = 0; i < NUM_SAVER_THREADS; i++) {
        thread_args[i].queue = &frame_queue;
        thread_args[i].progress = &progress;
        pthread_create(&saver_threads[i], NULL, frame_saver_thread, &thread_args[i]);
    }
    
    pthread_t audio_thread;
    if (config.extract_audio) {
        pthread_create(&audio_thread, NULL, extract_audio_thread, &config);
    }
    
    printf("\n🔄 Decoding frames with %d saver threads...\n", NUM_SAVER_THREADS);
    
    AVPacket packet;
    int current_frame = 0;
    int frames_queued = 0;
    int frames_decoded = 0;
    
    while (av_read_frame(fmt_ctx, &packet) >= 0 && frames_queued < extract_count) {
        if (packet.stream_index == video_stream_idx) {
            avcodec_send_packet(codec_ctx, &packet);
            
            while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                if (current_frame >= start_frame && 
                    current_frame <= end_frame &&
                    (config.frame_count == 0 || 
                     frame_in_list(current_frame, frames_to_extract, extract_count))) {
                    
                    queue_push(&frame_queue, frame, current_frame);
                    frames_queued++;
                    frames_decoded++;
                    
                    if (frames_decoded % 10 == 0) {
                        printf("\r📽️ Decoded: %d/%d frames", frames_decoded, extract_count);
                        fflush(stdout);
                    }
                }
                current_frame++;
                
                if (frames_queued >= extract_count) break;
            }
        }
        av_packet_unref(&packet);
        
        if (frames_queued >= extract_count) break;
    }
    
    printf("\r📽️ Decoded: %d/%d frames - done!\n", frames_decoded, extract_count);
    
    queue_set_done(&frame_queue);
    
    for (int i = 0; i < NUM_SAVER_THREADS; i++) {
        pthread_join(saver_threads[i], NULL);
    }
    
    if (config.extract_audio) {
        pthread_join(audio_thread, NULL);
    }
    
    progress_finish(&progress);
    
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    queue_destroy(&frame_queue);
    
    printf("\n✅ Done! Extracted %d frames using %d threads!\n", 
           extract_count, NUM_SAVER_THREADS);
    
    // Clean up downloaded file if from YouTube
    if (config.ytdl_download) {
        printf("\n🧹 Cleaning up downloaded file...\n");
        char rm_cmd[512];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -f \"%s\"", config.input);
        system(rm_cmd);
    }
    
    return 0;
}
