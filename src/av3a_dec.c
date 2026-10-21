/*
 * av3a_dec.dll - AVS3-P3 (Audio Vivid) av3a m4a decoder for MusicPlayer2.
 *
 * Exports (cdecl):
 *   int  av3a_probe(const wchar_t *path);                      1 = av3a m4a
 *   int  av3a_decode_wav(const wchar_t *path, void **data, unsigned *size);
 *   void av3a_free(void *data);
 *
 * Output is stereo 16-bit 44100 Hz PCM in a RIFF/WAVE buffer.
 * model.bin must sit next to this DLL.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "avs3_prot_dec.h"
#include "avs3_stat_dec.h"
#include "avs3_cnst_com.h"

#define OUT_RATE 44100
#define OUT_CH 2

#define EXPORT __declspec(dllexport)

extern const char *g_av3a_model_path;
static char g_model_path[MAX_PATH];

static void dll_dir(char *out, DWORD n)
{
    HMODULE mod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&dll_dir, &mod);
    GetModuleFileNameA(mod, out, n);
    char *slash = strrchr(out, '\\');
    if (slash) *slash = 0;
}


static void resolve_model(void)
{
    if (g_av3a_model_path != NULL) return;
    char dir[MAX_PATH], p1[MAX_PATH], p2[MAX_PATH];
    dll_dir(dir, MAX_PATH);
    snprintf(p1, sizeof(p1), "%s\\model.bin", dir);
    snprintf(p2, sizeof(p2), "%s\\vivid_model.bin", dir);
    FILE *f = fopen(p1, "rb");
    if (f) { fclose(f); snprintf(g_model_path, sizeof(g_model_path), "%s", p1); }
    else {
        f = fopen(p2, "rb");
        if (f) { fclose(f); snprintf(g_model_path, sizeof(g_model_path), "%s", p2); }
        else return;
    }
    g_av3a_model_path = g_model_path;
}

static unsigned be32(const unsigned char *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static unsigned long long be64(const unsigned char *p)
{
    return ((unsigned long long)be32(p) << 32) | be32(p + 4);
}

static const unsigned char *g_data;
static size_t g_size;

static const unsigned char *find_box(const unsigned char *start, const unsigned char *end, const char *type, int depth)
{
    static const char *containers[] = { "moov", "trak", "mdia", "minf", "stbl", "edts", "dinf", "udta", NULL };
    while (start + 8 <= end) {
        unsigned long long sz = be32(start);
        int hdr = 8;
        if (sz == 1) { if (start + 16 > end) return NULL; sz = be64(start + 8); hdr = 16; }
        else if (sz == 0) sz = (unsigned long long)(end - start);
        if (sz < (unsigned)hdr || start + sz > end) return NULL;
        if (memcmp(start + 4, type, 4) == 0) return start;
        if (depth < 8) {
            for (int i = 0; containers[i]; i++) {
                if (memcmp(start + 4, containers[i], 4) == 0) {
                    const unsigned char *r = find_box(start + hdr, start + sz, type, depth + 1);
                    if (r) return r;
                    break;
                }
            }
        }
        start += sz;
    }
    return NULL;
}

static int has_av3a_entry(const unsigned char *d, size_t n)
{
    const unsigned char *stsd = find_box(d, d + n, "stsd", 0);
    if (!stsd) return 0;
    unsigned count = be32(stsd + 12);
    if (count < 1) return 0;
    const unsigned char *entry = stsd + 16;
    return memcmp(entry + 4, "av3a", 4) == 0;
}

static unsigned char* extract_es_mem(const char* src, size_t* out_size)
{
    FILE* f = fopen(src, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* d = (unsigned char*)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);

    const unsigned char* stsz = find_box(d, d + n, "stsz", 0);
    const unsigned char* stco = find_box(d, d + n, "stco", 0);
    const unsigned char* co64 = find_box(d, d + n, "co64", 0);
    const unsigned char* stsc = find_box(d, d + n, "stsc", 0);
    if (!stsz || (!stco && !co64) || !stsc) { free(d); return NULL; }

    unsigned fixed = be32(stsz + 12);
    unsigned count = be32(stsz + 16);
    const unsigned char* size_tab = stsz + 20;
    const unsigned char* chunk_box = stco ? stco : co64;
    unsigned nchunks = be32(chunk_box + 12);
    const unsigned char* chunk_tab = chunk_box + 16;
    int chunk64 = (co64 != NULL);
    unsigned nsc = be32(stsc + 12);
    const unsigned char* sc_tab = stsc + 16;

    unsigned char* out = (unsigned char*)malloc((size_t)n);
    if (!out) { free(d); return NULL; }
    size_t pos = 0;
    unsigned sample = 0;

    for (unsigned ci = 1; ci <= nchunks && sample < count; ci++) {
        unsigned per_chunk = 0;
        for (unsigned j = 0; j < nsc; j++) {
            unsigned first = be32(sc_tab + j * 12);
            unsigned next_first = (j + 1 < nsc) ? be32(sc_tab + (j + 1) * 12) : 0xffffffffu;
            if (ci >= first && ci < next_first) { per_chunk = be32(sc_tab + j * 12 + 4); break; }
        }
        unsigned long long off = chunk64 ? be64(chunk_tab + (ci - 1) * 8) : be32(chunk_tab + (ci - 1) * 4);
        for (unsigned k = 0; k < per_chunk && sample < count; k++) {
            unsigned ssz = fixed ? fixed : be32(size_tab + sample * 4);
            if (off + ssz > (unsigned long long)n) break;
            memcpy(out + pos, d + off, ssz);
            pos += ssz;
            off += ssz;
            sample++;
        }
    }
    free(d);
    if (sample != count) { free(out); return NULL; }
    *out_size = pos;
    return out;
}

/*
 * Stereo rendering copied from the NetEase CloudMusic ear::VIVIDRender
 * panner (1000 Hz.. panner gains measured from the running client):
 *
 *   L = g*(FL + C + LFE) + s*(BL - BR + SL + SR + TopL-front + TopL-back)
 *   R = g*(FR + C + LFE) + s*(BR - BL + SL + SR + TopR-front + TopR-back)
 *
 * with g = 0.85 and s = g/sqrt(2) = 0.601.  It is a memoryless matrix; the
 * only dynamics are a look-ahead peak limiter (same latency as the client's
 * renderer) followed by a hard clamp.
 */
#define LIM_LOOKAHEAD 2147
#define LIM_RELEASE_SEC 0.227f
#define LIM_SMOOTH 0.6f
#define LIM_THRESHOLD (32760.0f / 32768.0f)

typedef struct {
    float ring[2][LIM_LOOKAHEAD];
    int idx;
    float gain[2];
    float env[2];
    float decay;
} PostState;

static void post_init(PostState* p, int rate)
{
    memset(p, 0, sizeof(*p));
    p->gain[0] = p->gain[1] = 1.0f;
    p->decay = expf(-1.0f / (LIM_RELEASE_SEC * (float)rate));
}

static float lim_one(PostState* p, int c, float x)
{
    float a = x < 0 ? -x : x;
    float e = p->env[c] * p->decay;
    if (a > e) e = a;
    p->env[c] = e;
    float target = e > LIM_THRESHOLD ? LIM_THRESHOLD / e : 1.0f;
    p->gain[c] += (1.0f - LIM_SMOOTH) * (target - p->gain[c]);
    float y = p->gain[c] * p->ring[c][p->idx];
    p->ring[c][p->idx] = x;
    return y;
}

static short to_pcm(float y)
{
    if (y > 1.0f) y = 1.0f;
    if (y < -1.0f) y = -1.0f;
    return (short)(y * 32767.0f);
}

static void post_process(PostState* p, const float* in, short* out, int frames)
{
    for (int i = 0; i < frames; ++i) {
        float y0 = lim_one(p, 0, in[i * 2]);
        float y1 = lim_one(p, 1, in[i * 2 + 1]);
        p->idx = (p->idx + 1) % LIM_LOOKAHEAD;
        out[i * 2] = to_pcm(y0);
        out[i * 2 + 1] = to_pcm(y1);
    }
}

/* drain the look-ahead delay line with silence at the end of the stream */
static void post_flush(PostState* p, short* out, int frames)
{
    for (int i = 0; i < frames; ++i) {
        float y0 = lim_one(p, 0, 0.0f);
        float y1 = lim_one(p, 1, 0.0f);
        p->idx = (p->idx + 1) % LIM_LOOKAHEAD;
        out[i * 2] = to_pcm(y0);
        out[i * 2 + 1] = to_pcm(y1);
    }
}

static void downmix(const short *in, int channels, int frames, float *out)
{
    const float g = 0.85f;
    const float s = g / 1.41421356f;   /* 0.601 */
    for (int i = 0; i < frames; i++) {
        const short *v = in + (size_t)i * channels;
        float l = 0.0f, r = 0.0f;
        if (channels >= 12) {
            l = g * (v[0] + v[2] + v[3]) + s * (v[4] - v[5] + v[6] + v[7] + v[8] + v[10]);
            r = g * (v[1] + v[2] + v[3]) + s * (v[5] - v[4] + v[6] + v[7] + v[9] + v[11]);
        } else if (channels >= 10) {
            l = g * (v[0] + v[2] + v[3]) + s * (v[4] - v[5] + v[6] + v[7] + v[8]);
            r = g * (v[1] + v[2] + v[3]) + s * (v[5] - v[4] + v[6] + v[7] + v[9]);
        } else if (channels >= 8) {
            l = g * (v[0] + v[2] + v[3]) + s * (v[4] - v[5] + v[6] + v[7]);
            r = g * (v[1] + v[2] + v[3]) + s * (v[5] - v[4] + v[6] + v[7]);
        } else if (channels >= 6) {
            l = g * (v[0] + v[2] + v[3]) + s * (v[4] - v[5]);
            r = g * (v[1] + v[2] + v[3]) + s * (v[5] - v[4]);
        } else if (channels == 2) { l = v[0]; r = v[1]; }
        else if (channels == 1) { l = r = v[0]; }
        else { for (int c = 0; c < channels; c++) { if (c & 1) r += v[c] * 0.5f; else l += v[c] * 0.5f; } }
        out[i * 2] = l / 32768.0f;
        out[i * 2 + 1] = r / 32768.0f;
    }
}

static unsigned char *g_pcm;
static size_t g_pcm_size, g_pcm_cap;

static int pcm_append(const short *data, size_t samples)
{
    size_t bytes = samples * sizeof(short);
    if (g_pcm_size + bytes > g_pcm_cap) {
        size_t cap = g_pcm_cap ? g_pcm_cap : (1 << 20);
        while (cap < g_pcm_size + bytes) cap *= 2;
        unsigned char *p = (unsigned char *)realloc(g_pcm, cap);
        if (!p) return -1;
        g_pcm = p;
        g_pcm_cap = cap;
    }
    memcpy(g_pcm + g_pcm_size, data, bytes);
    g_pcm_size += bytes;
    return 0;
}

static void put_le32(unsigned char *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff; }
static void put_le16(unsigned char *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }

EXPORT int av3a_probe(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    unsigned char head[64];
    size_t n = fread(head, 1, sizeof(head), f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    if (n < 16 || memcmp(head + 4, "ftyp", 4) != 0) return 0;
    if (size <= 0 || size > (512L << 20)) return 0;

    unsigned char *d = (unsigned char *)malloc((size_t)size);
    if (!d) return 0;
    f = _wfopen(path, L"rb");
    if (!f) { free(d); return 0; }
    size_t got = fread(d, 1, (size_t)size, f);
    fclose(f);
    int ok = (got == (size_t)size) ? has_av3a_entry(d, (size_t)size) : 0;
    free(d);
    return ok;
}


EXPORT int av3a_decode_wav(const wchar_t *path, void **data, unsigned *size)
{
    resolve_model();
    if (!data || !size) return -1;
    *data = NULL;
    *size = 0;

    char src[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, path, -1, src, MAX_PATH, NULL, NULL);

    size_t es_size = 0;
    unsigned char* es = extract_es_mem(src, &es_size);
    if (!es) return -2;

    AVS3DecoderHandle dec = (AVS3DecoderHandle)calloc(1, sizeof(AVS3Decoder));
    if (!dec) { free(es); return -3; }
    dec->memData = es;
    dec->memSize = (unsigned long)es_size;
    dec->memPos = 0;

    FILE* fModel = NULL;
    Avs3ParseBsFrameHeader(dec, NULL, 1, NULL);
    Avs3InitDecoder(dec, &fModel);

    int channels = dec->numChansOutput;
    int frame_len = dec->frameLength;
    int out_rate = (int)dec->outputFs;
    if (out_rate <= 0) out_rate = OUT_RATE;
    short *pcm = (short *)malloc(sizeof(short) * MAX_CHANNELS * FRAME_LEN);
    short *stereo = (short *)malloc(sizeof(short) * frame_len * 2);
    float *mix = (float *)malloc(sizeof(float) * frame_len * 2);
    g_pcm = NULL; g_pcm_size = 0; g_pcm_cap = 0;

    if (!pcm || !stereo || !mix) {
        if (pcm) free(pcm);
        if (stereo) free(stereo);
        if (mix) free(mix);
        if (fModel) fclose(fModel);
        free(dec);
        free(es);
        return -4;
    }

    PostState post;
    post_init(&post, out_rate);
    while (ReadBitstream(dec, NULL) != 0) {
        Avs3Decode(dec, pcm);
        ResetBitstream(dec->hBitstream);
        downmix(pcm, channels, frame_len, mix);
        post_process(&post, mix, stereo, frame_len);
        if (pcm_append(stereo, (size_t)frame_len * 2) != 0) break;
    }
    /* drain the limiter's look-ahead delay line */
    for (int left = LIM_LOOKAHEAD; left > 0; ) {
        int n = left < frame_len ? left : frame_len;
        post_flush(&post, stereo, n);
        if (pcm_append(stereo, (size_t)n * 2) != 0) break;
        left -= n;
    }

    if (fModel) fclose(fModel);
    free(pcm);
    free(stereo);
    free(mix);
    free(dec);
    free(es);

    if (g_pcm_size == 0) {
        free(g_pcm);
        g_pcm = NULL;
        return -5;
    }

    unsigned char *wav = (unsigned char *)malloc(44 + g_pcm_size);
    if (!wav) { free(g_pcm); g_pcm = NULL; return -6; }
    unsigned char *h = wav;
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, (unsigned)(36 + g_pcm_size));
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);
    put_le16(h + 22, OUT_CH);
    put_le32(h + 24, (unsigned)out_rate);
    put_le32(h + 28, (unsigned)out_rate * OUT_CH * 2);
    put_le16(h + 32, OUT_CH * 2);
    put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, (unsigned)g_pcm_size);
    memcpy(wav + 44, g_pcm, g_pcm_size);
    free(g_pcm);
    g_pcm = NULL;

    *data = wav;
    *size = (unsigned)(44 + g_pcm_size);
    return 0;
}

EXPORT void av3a_free(void *data)
{
    free(data);
}


/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* windowed streaming: decode on demand, seek like the client          */
/* ------------------------------------------------------------------ */
#define WIN_SECONDS 6
#define AHEAD_SECONDS 4
#define PREROLL_FRAMES 8

typedef struct Av3aJob {
    HANDLE thread;
    volatile LONG stop;
    volatile LONG eof;

    int channels, frame_len, out_rate;
    unsigned total_frames;
    unsigned long long total_samples;
    unsigned long long capacity;

    /* frame -> byte offset into the elementary stream */
    const unsigned char* es;
    unsigned long long es_size;
    unsigned* frame_off;      /* prefix sums, total_frames + 1 entries */
    unsigned frame_bytes;     /* constant sample size, 0 if variable */

    /* decoded window: interleaved stereo int16 (virtual absolute samples) */
    short* win;
    unsigned long long win_samples;
    unsigned long long win_base;
    unsigned long long win_valid;

    /* reader state */
    unsigned long long want;
    volatile LONG dirty;

    unsigned char header[44];
    char src_path[MAX_PATH];
    volatile LONG frames_decoded;
} Av3aJob;

static Av3aJob g_job;
static CRITICAL_SECTION g_job_cs;
static LONG g_job_cs_ready = 0;

/* stsz sample sizes -> prefix sum table (NULL when the size is constant) */
static unsigned* mp4_frame_table(const char* src, unsigned* count, unsigned* fixed, unsigned long long* es_size)
{
    FILE* f = fopen(src, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* d = (unsigned char*)malloc((size_t)n);
    if (!d) { fclose(f); return NULL; }
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return NULL; }
    fclose(f);

    unsigned* table = NULL;
    const unsigned char* stsz = find_box(d, d + n, "stsz", 0);
    if (stsz) {
        unsigned fx = be32(stsz + 12);
        unsigned cnt = be32(stsz + 16);
        *count = cnt;
        *fixed = fx;
        const unsigned char* sizes = stsz + 20;
        table = (unsigned*)malloc(sizeof(unsigned) * ((size_t)cnt + 1));
        if (table) {
            unsigned long long sum = 0;
            for (unsigned i = 0; i < cnt; ++i) {
                table[i] = (unsigned)sum;
                sum += fx ? fx : be32(sizes + (size_t)i * 4);
            }
            table[cnt] = (unsigned)sum;
            *es_size = sum;
        }
    }
    free(d);
    return table;
}

static unsigned long long frame_offset(Av3aJob* st, unsigned frame)
{
    if (frame > st->total_frames) frame = st->total_frames;
    if (st->frame_off) return st->frame_off[frame];
    return (unsigned long long)frame * st->frame_bytes;
}

static void post_reset(PostState* p, int rate)
{
    post_init(p, rate);
}

static void job_decoder_close(AVS3DecoderHandle* dec, FILE** fModel)
{
    if (*fModel) { fclose(*fModel); *fModel = NULL; }
    if (*dec) { free(*dec); *dec = NULL; }
}

/* (re)start the decoder so that output sample `base` is the first one written */
static int job_seek_decoder(Av3aJob* st, AVS3DecoderHandle* dec, FILE** fModel, PostState* post,
                            unsigned long long base, unsigned long long* out_abs)
{
    job_decoder_close(dec, fModel);
    post_reset(post, st->out_rate);

    unsigned long long first_frame = base / (unsigned)st->frame_len;
    unsigned start_frame = first_frame > PREROLL_FRAMES ? (unsigned)(first_frame - PREROLL_FRAMES) : 0;

    AVS3DecoderHandle d = (AVS3DecoderHandle)calloc(1, sizeof(AVS3Decoder));
    if (!d) return 0;
    d->memData = (unsigned char*)st->es;
    d->memSize = (unsigned long)st->es_size;
    d->memPos = 0;

    FILE* fm = NULL;
    Avs3ParseBsFrameHeader(d, NULL, 1, NULL);   /* config from frame 0, rewinds */
    Avs3InitDecoder(d, &fm);
    /* jump must happen after the init, which rewinds to the stream start */
    d->memPos = (unsigned long)frame_offset(st, start_frame);
    ResetBitstream(d->hBitstream);
    if (d->numChansOutput != st->channels || d->frameLength != st->frame_len) {
        /* streams with a different config are not supported mid-stream */
        if (fm) fclose(fm);
        free(d);
        return 0;
    }
    *dec = d;
    *fModel = fm;
    *out_abs = (unsigned long long)start_frame * (unsigned)st->frame_len;
    return 1;
}

/* drop decoded samples the reader has already passed */
static int win_compact(Av3aJob* st)
{
    unsigned long long keep = (st->want / (unsigned)st->frame_len) * (unsigned)st->frame_len;
    if (st->win_valid == 0) { st->win_base = keep > st->win_base ? keep : st->win_base; return 1; }
    if (keep <= st->win_base) return 0;
    unsigned long long drop = keep - st->win_base;
    if (drop >= st->win_valid) {
        st->win_base = keep;
        st->win_valid = 0;
        return 1;
    }
    memmove(st->win, st->win + drop * 2, (size_t)(st->win_valid - drop) * sizeof(short) * 2);
    st->win_base = keep;
    st->win_valid -= drop;
    return 1;
}

static DWORD WINAPI av3a_job_thread(LPVOID param)
{
    Av3aJob* st = (Av3aJob*)param;
    AVS3DecoderHandle dec = NULL;
    FILE* fModel = NULL;
    PostState post;
    unsigned long long out_abs = 0;
    short* pcm = (short*)malloc(sizeof(short) * MAX_CHANNELS * FRAME_LEN);
    short* out = (short*)malloc(sizeof(short) * st->frame_len * 2);
    float* mix = (float*)malloc(sizeof(float) * st->frame_len * 2);
    int seeded = 0;

    if (!pcm || !out || !mix) {
        InterlockedExchange(&st->eof, 1);
        free(pcm); free(out); free(mix);
        return 0;
    }

    while (!st->stop) {
        unsigned long long want, valid_end;
        int jump, space;
        EnterCriticalSection(&g_job_cs);
        want = st->want;
        jump = (int)st->dirty;
        st->dirty = 0;
        valid_end = st->win_base + st->win_valid;
        LeaveCriticalSection(&g_job_cs);

        if (jump || !dec || !seeded) {
            unsigned long long base = (want / (unsigned)st->frame_len) * (unsigned)st->frame_len;
            if (!job_seek_decoder(st, &dec, &fModel, &post, base, &out_abs)) {
                InterlockedExchange(&st->eof, 1);
                break;
            }
            EnterCriticalSection(&g_job_cs);
            st->win_base = base;
            st->win_valid = 0;
            LeaveCriticalSection(&g_job_cs);
            seeded = 1;
        } else if (want + (unsigned long long)(AHEAD_SECONDS * st->out_rate) <= valid_end) {
            Sleep(4);
            continue;
        }

        EnterCriticalSection(&g_job_cs);
        if (st->win_valid + (unsigned long long)st->frame_len > st->win_samples) win_compact(st);
        space = (st->win_valid + (unsigned long long)st->frame_len <= st->win_samples);
        LeaveCriticalSection(&g_job_cs);
        if (!space) { Sleep(4); continue; }

        if (ReadBitstream(dec, NULL) == 0) {
            InterlockedExchange(&st->eof, 1);
            /* idle until the reader seeks somewhere else */
            while (!st->stop && !st->dirty) Sleep(5);
            if (st->stop) break;
            InterlockedExchange(&st->eof, 0);
            continue;
        }
        Avs3Decode(dec, pcm);
        InterlockedIncrement(&st->frames_decoded);
        ResetBitstream(dec->hBitstream);
        downmix(pcm, st->channels, st->frame_len, mix);
        post_process(&post, mix, out, st->frame_len);

        EnterCriticalSection(&g_job_cs);
        if (out_abs < st->total_samples && out_abs + (unsigned long long)st->frame_len > st->win_base) {
            unsigned long long start = out_abs > st->win_base ? out_abs : st->win_base;
            unsigned long long stop = out_abs + (unsigned long long)st->frame_len;
            if (stop > st->win_base + st->win_samples) stop = st->win_base + st->win_samples;
            if (stop > st->total_samples) stop = st->total_samples;
            if (stop > start) {
                unsigned skip = (unsigned)(start - out_abs);
                unsigned n = (unsigned)(stop - start);
                memcpy(st->win + (size_t)(start - st->win_base) * 2, out + skip * 2, (size_t)n * sizeof(short) * 2);
                if (start + n > st->win_base + st->win_valid)
                    st->win_valid = start + n - st->win_base;
            }
        }
        LeaveCriticalSection(&g_job_cs);
        out_abs += (unsigned long long)st->frame_len;
    }

    job_decoder_close(&dec, &fModel);
    free(pcm);
    free(out);
    free(mix);
    InterlockedExchange(&st->eof, 1);
    return 0;
}

static void av3a_job_stop(void)
{
    if (!g_job_cs_ready) return;
    InterlockedExchange(&g_job.stop, 1);
    if (g_job.thread) {
        WaitForSingleObject(g_job.thread, 8000);
        CloseHandle(g_job.thread);
        g_job.thread = NULL;
    }
    EnterCriticalSection(&g_job_cs);
    free(g_job.win);
    free(g_job.frame_off);
    free((void*)g_job.es);
    memset(&g_job, 0, sizeof(g_job));
    LeaveCriticalSection(&g_job_cs);
}

EXPORT void* av3a_stream_open(const wchar_t* path)
{
    if (!path) return NULL;
    if (!g_job_cs_ready) {
        if (InterlockedCompareExchange(&g_job_cs_ready, 1, 0) == 0)
            InitializeCriticalSection(&g_job_cs);
    }
    av3a_job_stop();
    resolve_model();

    char src[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, path, -1, src, MAX_PATH, NULL, NULL);

    unsigned frames = 0, fixed = 0;
    unsigned long long es_size = 0;
    unsigned* table = mp4_frame_table(src, &frames, &fixed, &es_size);
    if (!table || frames == 0) { free(table); return NULL; }

    size_t es_read = 0;
    unsigned char* es = extract_es_mem(src, &es_read);
    if (!es) { free(table); return NULL; }

    int frame_len = 1024, out_rate = OUT_RATE;
    {
        AVS3DecoderHandle probe = (AVS3DecoderHandle)calloc(1, sizeof(AVS3Decoder));
        if (!probe) { free(es); free(table); return NULL; }
        probe->memData = es;
        probe->memSize = (unsigned long)es_read;
        probe->memPos = 0;
        Avs3ParseBsFrameHeader(probe, NULL, 1, NULL);
        if (probe->frameLength > 0) frame_len = probe->frameLength;
        if (probe->outputFs > 0) out_rate = (int)probe->outputFs;
        free(probe);
    }

    unsigned long long total_samples = (unsigned long long)frames * (unsigned)frame_len;
    unsigned long long capacity = 44 + total_samples * OUT_CH * sizeof(short);

    unsigned long long win_samples = (unsigned long long)out_rate * WIN_SECONDS;
    if (win_samples < (unsigned long long)frame_len * 64) win_samples = (unsigned long long)frame_len * 64;

    short* win = (short*)malloc((size_t)win_samples * OUT_CH * sizeof(short));
    if (!win) { free(es); free(table); return NULL; }

    strncpy(g_job.src_path, src, MAX_PATH - 1);
    g_job.es = es;
    g_job.es_size = es_read;
    g_job.frame_off = table;
    g_job.frame_bytes = fixed;
    g_job.total_frames = frames;
    g_job.total_samples = total_samples;
    g_job.capacity = capacity;
    g_job.frame_len = frame_len;
    g_job.out_rate = out_rate;
    g_job.channels = 12;
    g_job.win = win;
    g_job.win_samples = win_samples;
    g_job.win_base = 0;
    g_job.win_valid = 0;
    g_job.want = 0;
    g_job.dirty = 0;

    unsigned char* h = g_job.header;
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, (unsigned)(36 + total_samples * OUT_CH * sizeof(short)));
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);
    put_le16(h + 22, OUT_CH);
    put_le32(h + 24, (unsigned)out_rate);
    put_le32(h + 28, (unsigned)out_rate * OUT_CH * 2);
    put_le16(h + 32, OUT_CH * 2);
    put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, (unsigned)(total_samples * OUT_CH * sizeof(short)));

    g_job.thread = CreateThread(NULL, 0, av3a_job_thread, &g_job, 0, NULL);
    if (!g_job.thread) {
        free(g_job.win); g_job.win = NULL;
        free(g_job.frame_off); g_job.frame_off = NULL;
        free(es); g_job.es = NULL;
        memset(&g_job, 0, sizeof(g_job));
        return NULL;
    }

    /* wait for the initial buffer */
    unsigned long long pre = (unsigned long long)out_rate * OUT_CH * sizeof(short) * 2;
    for (;;) {
        unsigned long long have;
        EnterCriticalSection(&g_job_cs);
        have = g_job.win_valid * OUT_CH * sizeof(short);
        LeaveCriticalSection(&g_job_cs);
        if (g_job.eof || have >= pre) break;
        Sleep(10);
    }
    return &g_job;
}

EXPORT unsigned long long av3a_stream_length(void* handle)
{
    Av3aJob* st = (Av3aJob*)handle;
    return st ? st->capacity : 0;
}

/* block until [s, s+n) is decoded */
static int job_wait_range(Av3aJob* st, unsigned long long s, unsigned long long n)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        EnterCriticalSection(&g_job_cs);
        unsigned long long end = st->win_base + st->win_valid;
        int ok = (s >= st->win_base && s + n <= end);
        if (!ok) {
            st->want = s;
            if (s < st->win_base || s >= st->win_base + st->win_samples)
                st->dirty = 1;
        }
        LeaveCriticalSection(&g_job_cs);
        if (ok) return 1;
        if (st->stop) return 0;
        if (GetTickCount() - t0 > 4000) return 0;
        Sleep(1);
    }
}

EXPORT int av3a_stream_read(void* handle, void* buf, unsigned len, unsigned long long offset)
{
    Av3aJob* st = (Av3aJob*)handle;
    if (!st || !buf || len == 0) return 0;
    if (offset >= st->capacity) return 0;
    unsigned long long end = offset + len;
    if (end > st->capacity) end = st->capacity;

    unsigned char* dst = (unsigned char*)buf;
    unsigned got = 0;

    while (offset < end) {
        if (offset < 44) {
            unsigned long long take = 44 - offset;
            if (take > end - offset) take = end - offset;
            memcpy(dst + got, st->header + offset, (size_t)take);
            got += (unsigned)take;
            offset += take;
            continue;
        }
        unsigned long long pcm_off = offset - 44;
        unsigned long long sample = pcm_off / (OUT_CH * sizeof(short));
        unsigned long long byte_in_sample = pcm_off % (OUT_CH * sizeof(short));
        unsigned long long remaining = end - offset;
        unsigned long long max_bytes = (st->win_samples / 2) * OUT_CH * sizeof(short);
        if (remaining > max_bytes) remaining = max_bytes;
        unsigned long long samples_needed = (byte_in_sample + remaining + OUT_CH * sizeof(short) - 1) / (OUT_CH * sizeof(short));

        if (!job_wait_range(st, sample, samples_needed)) return (int)got;

        EnterCriticalSection(&g_job_cs);
        unsigned long long avail_end = st->win_base + st->win_valid;
        unsigned long long take_samples = samples_needed;
        if (sample + take_samples > avail_end) take_samples = avail_end > sample ? avail_end - sample : 0;
        if (take_samples > 0) {
            const unsigned char* src = (const unsigned char*)(st->win + (size_t)(sample - st->win_base) * 2);
            unsigned long long bytes = (unsigned long long)take_samples * OUT_CH * sizeof(short) - byte_in_sample;
            if (bytes > remaining) bytes = remaining;
            memcpy(dst + got, src + byte_in_sample, (size_t)bytes);
            got += (unsigned)bytes;
            offset += bytes;
        }
        LeaveCriticalSection(&g_job_cs);
        if (take_samples == 0) return (int)got;
    }
    return (int)got;
}

EXPORT void av3a_stream_debug(void* handle, unsigned long long* out)
{
    Av3aJob* st = (Av3aJob*)handle;
    if (!st || !out) return;
    EnterCriticalSection(&g_job_cs);
    out[0] = st->win_base;
    out[1] = st->win_valid;
    out[2] = st->want;
    out[3] = (unsigned long long)st->dirty;
    out[4] = (unsigned long long)st->eof;
    out[5] = (unsigned long long)st->stop;
    out[6] = st->total_samples;
    out[7] = (unsigned long long)st->frames_decoded;
    LeaveCriticalSection(&g_job_cs);
}

EXPORT void av3a_stream_close(void* handle)
{
    if (!g_job_cs_ready) return;
    (void)handle;
    av3a_job_stop();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        resolve_model();
    }
    return TRUE;
}
