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

static int extract_es(const char *src, const char *dst)
{
    FILE *f = fopen(src, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return -1; }
    fclose(f);

    g_data = d; g_size = (size_t)n;

    const unsigned char *stsz = find_box(d, d + n, "stsz", 0);
    const unsigned char *stco = find_box(d, d + n, "stco", 0);
    const unsigned char *co64 = find_box(d, d + n, "co64", 0);
    const unsigned char *stsc = find_box(d, d + n, "stsc", 0);
    if (!stsz || (!stco && !co64) || !stsc) { free(d); return -1; }

    unsigned fixed = be32(stsz + 12);
    unsigned count = be32(stsz + 16);
    const unsigned char *size_tab = stsz + 20;
    const unsigned char *chunk_box = stco ? stco : co64;
    unsigned nchunks = be32(chunk_box + 12);
    const unsigned char *chunk_tab = chunk_box + 16;
    int chunk64 = (co64 != NULL);
    unsigned nsc = be32(stsc + 12);
    const unsigned char *sc_tab = stsc + 16;

    FILE *o = fopen(dst, "wb");
    if (!o) { free(d); return -1; }

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
            if (off + ssz > g_size) break;
            fwrite(d + off, 1, ssz, o);
            off += ssz;
            sample++;
        }
    }
    fclose(o);
    free(d);
    g_data = NULL;
    return (sample == count) ? 0 : -1;
}


/* simple output gain + clamp for the matrix downmix */
typedef struct { float gain; float env[2]; float rel; } PostState;

static void post_init(PostState* p, int rate)
{
    (void)rate;
    p->gain = 1.25f;  /* +1.9 dB, tamed by the peak limiter */
    p->env[0] = p->env[1] = 0.0f;
    p->rel = expf(-1.0f / (0.15f * rate));
}

static void post_process(PostState* p, short* stereo, int frames)
{
    for (int i = 0; i < frames; ++i) {
        float l = stereo[i * 2] / 32768.0f;
        float r = stereo[i * 2 + 1] / 32768.0f;
        float m = (l + r) * 0.5f;
        float s = (l - r) * 0.5f * 0.68f;
        float lr[2] = { m + s, m - s };
        for (int c = 0; c < 2; ++c) {
            int idx = i * 2 + c;
            float x = lr[c] * p->gain;
            float a = x < 0 ? -x : x;
            if (a > p->env[c]) p->env[c] = a; else p->env[c] *= p->rel;
            float g = p->env[c] > 0.95f ? 0.95f / p->env[c] : 1.0f;
            x *= g;
            if (x > 1.0f) x = 1.0f;
            if (x < -1.0f) x = -1.0f;
            stereo[idx] = (short)(x * 32767.0f);
        }
    }
}

static void downmix(const short *in, int channels, int frames, short *out)
{
    const float g = 0.55f;
    for (int i = 0; i < frames; i++) {
        const short *s = in + (size_t)i * channels;
        float l = 0, r = 0;
        if (channels >= 12) {
            l = s[0] + 0.707f * s[2] + 0.707f * s[4] + 0.707f * s[6] + 0.30f * s[3] + 0.5f * s[8] + 0.5f * s[10];
            r = s[1] + 0.707f * s[2] + 0.707f * s[5] + 0.707f * s[7] + 0.30f * s[3] + 0.5f * s[9] + 0.5f * s[11];
        } else if (channels == 2) { l = s[0]; r = s[1]; }
        else if (channels == 1) { l = r = s[0]; }
        else { for (int c = 0; c < channels; c++) { if (c & 1) r += s[c] * 0.5f; else l += s[c] * 0.5f; } }
        l *= g; r *= g;
        if (l > 32767) l = 32767; if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; if (r < -32768) r = -32768;
        out[i * 2] = (short)l;
        out[i * 2 + 1] = (short)r;
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


    char dllpath[MAX_PATH];
    char modelpath[MAX_PATH];
    dll_dir(dllpath, MAX_PATH);
    snprintf(modelpath, sizeof(modelpath), "%s\\model.bin", dllpath);

    char src[MAX_PATH], tmppath[MAX_PATH], espath[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, path, -1, src, MAX_PATH, NULL, NULL);
    char tmpdir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpdir);
    snprintf(espath, sizeof(espath), "%sav3a_dll_%lu.av3a", tmpdir, (unsigned long)GetCurrentProcessId());
    snprintf(tmppath, sizeof(tmppath), "%sav3a_dll_%lu.nul", tmpdir, (unsigned long)GetCurrentProcessId());

    if (extract_es(src, espath) != 0) {
        DeleteFileA(espath);
        return -2;
    }

    AVS3DecoderHandle dec = (AVS3DecoderHandle)malloc(sizeof(AVS3Decoder));
    if (!dec) { DeleteFileA(espath); return -3; }

    FILE *fIn = NULL, *fOut = NULL, *fModel = NULL;
    char *dargv[5];
    dargv[0] = (char *)"av3a_dec";
    dargv[1] = (char *)"-if";
    dargv[2] = espath;
    dargv[3] = (char *)"-of";
    dargv[4] = tmppath;

    GetAvs3DecoderCommandLine(dec, 5, dargv, &fIn, &fOut);
    Avs3InitDecoder(dec, &fModel);

    int channels = dec->numChansOutput;
    int frame_len = dec->frameLength;
    short *pcm = (short *)malloc(sizeof(short) * MAX_CHANNELS * FRAME_LEN);
    short *stereo = (short *)malloc(sizeof(short) * frame_len * 2);
    g_pcm = NULL; g_pcm_size = 0; g_pcm_cap = 0;

    if (!pcm || !stereo) {
        if (pcm) free(pcm);
        if (stereo) free(stereo);
        if (fIn) fclose(fIn);
        if (fOut) fclose(fOut);
        if (fModel) fclose(fModel);
        free(dec);
        DeleteFileA(espath);
        DeleteFileA(tmppath);
        return -4;
    }

    while (ReadBitstream(dec, fIn) != 0) {
        Avs3Decode(dec, pcm);
        ResetBitstream(dec->hBitstream);
        downmix(pcm, channels, frame_len, stereo);
        if (pcm_append(stereo, (size_t)frame_len * 2) != 0) break;
    }

    if (fIn) fclose(fIn);
    if (fOut) fclose(fOut);
    if (fModel) fclose(fModel);
    free(pcm);
    free(stereo);
    free(dec);
    DeleteFileA(espath);
    DeleteFileA(tmppath);

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
    put_le32(h + 24, OUT_RATE);
    put_le32(h + 28, OUT_RATE * OUT_CH * 2);
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
/* progressive WAV file + background decode                            */
/* ------------------------------------------------------------------ */
typedef struct Av3aJob {
    HANDLE thread;
    volatile LONG stop;
    volatile LONG eof;
    int channels, frame_len;
    unsigned total_frames, frames_done;
    unsigned long long capacity, produced;
    char src_path[MAX_PATH];
    char es_path[MAX_PATH];
    unsigned char* buf;
} Av3aJob;

static Av3aJob g_job;
static CRITICAL_SECTION g_job_cs;
static LONG g_job_cs_ready = 0;
static WCHAR g_return_path[MAX_PATH];

static unsigned mp4_sample_count(const char* src)
{
    FILE* f = fopen(src, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* d = (unsigned char*)malloc((size_t)n);
    if (!d) { fclose(f); return 0; }
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return 0; }
    fclose(f);
    unsigned count = 0;
    const unsigned char* stsz = find_box(d, d + n, "stsz", 0);
    if (stsz) count = be32(stsz + 16);
    free(d);
    return count;
}

static DWORD WINAPI av3a_job_thread(LPVOID param)
{
    Av3aJob* st = (Av3aJob*)param;
    FILE* fIn = NULL, *fOut = NULL, *fModel = NULL;
    AVS3DecoderHandle dec = NULL;
    short* pcm = NULL;
    short* stereo = NULL;
    char tmppath[MAX_PATH];
    PostState post;

    if (extract_es(st->src_path, st->es_path) != 0) { }
    snprintf(tmppath, sizeof(tmppath), "%s.nul", st->es_path);

    dec = (AVS3DecoderHandle)malloc(sizeof(AVS3Decoder));
    if (dec) {
        char* dargv[5];
        dargv[0] = (char*)"av3a_stream";
        dargv[1] = (char*)"-if";
        dargv[2] = st->es_path;
        dargv[3] = (char*)"-of";
        dargv[4] = tmppath;
        GetAvs3DecoderCommandLine(dec, 5, dargv, &fIn, &fOut);
        Avs3InitDecoder(dec, &fModel);
        pcm = (short*)malloc(sizeof(short) * MAX_CHANNELS * FRAME_LEN);
        stereo = (short*)malloc(sizeof(short) * dec->frameLength * 2);
        st->channels = dec->numChansOutput;
        st->frame_len = dec->frameLength;
    }

    post_init(&post, OUT_RATE);
    while (!st->stop && pcm && stereo && ReadBitstream(dec, fIn) != 0) {
        unsigned bytes = (unsigned)st->frame_len * 2 * sizeof(short);
        Avs3Decode(dec, pcm);
        ResetBitstream(dec->hBitstream);
        downmix(pcm, st->channels, st->frame_len, stereo);
        post_process(&post, stereo, st->frame_len);
        if (st->produced + bytes > st->capacity) break;
        EnterCriticalSection(&g_job_cs);
        memcpy(st->buf + st->produced, stereo, bytes);
        st->produced += bytes;
        st->frames_done++;
        LeaveCriticalSection(&g_job_cs);
    }

    if (fIn) fclose(fIn);
    if (fOut) fclose(fOut);
    if (fModel) fclose(fModel);
    free(pcm);
    free(stereo);
    free(dec);
    DeleteFileA(st->es_path);
    DeleteFileA(tmppath);

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
    free(g_job.buf);
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

    char src[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, path, -1, src, MAX_PATH, NULL, NULL);
    unsigned frames = mp4_sample_count(src);
    if (frames == 0) return NULL;

    unsigned long long pcm_bytes = (unsigned long long)frames * 1024 * OUT_CH * sizeof(short);
    unsigned long long total = 44 + pcm_bytes;

    strncpy(g_job.src_path, src, MAX_PATH - 1);
    g_job.capacity = total;
    g_job.produced = 44;
    g_job.frame_len = 1024;
    g_job.channels = 12;

    char tmpdir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpdir);
    snprintf(g_job.es_path, sizeof(g_job.es_path), "%sav3a_job_%lu.av3a", tmpdir, (unsigned long)GetCurrentProcessId());

    unsigned char* buf = (unsigned char*)calloc(1, (size_t)total);
    if (!buf) return NULL;
    g_job.buf = buf;

    unsigned char* h = buf;
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, (unsigned)(36 + pcm_bytes));
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);
    put_le16(h + 22, OUT_CH);
    put_le32(h + 24, OUT_RATE);
    put_le32(h + 28, OUT_RATE * OUT_CH * 2);
    put_le16(h + 32, OUT_CH * 2);
    put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, (unsigned)pcm_bytes);

    g_job.thread = CreateThread(NULL, 0, av3a_job_thread, &g_job, 0, NULL);
    if (!g_job.thread) {
        free(g_job.buf);
        memset(&g_job, 0, sizeof(g_job));
        return NULL;
    }

    while (!g_job.eof && g_job.produced < 44 + (unsigned long long)OUT_RATE * OUT_CH * 2 * 2) {
        Sleep(10);
    }
    return &g_job;
}

EXPORT unsigned long long av3a_stream_length(void* handle)
{
    Av3aJob* st = (Av3aJob*)handle;
    return st ? st->capacity : 0;
}

EXPORT int av3a_stream_read(void* handle, void* buf, unsigned len, unsigned long long offset)
{
    Av3aJob* st = (Av3aJob*)handle;
    if (!st || !buf || !st->buf) return 0;
    if (offset >= st->capacity) return 0;
    unsigned long long end = offset + len;
    if (end > st->capacity) end = st->capacity;
    memcpy(buf, st->buf + offset, (size_t)(end - offset));
    return (int)(end - offset);
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
