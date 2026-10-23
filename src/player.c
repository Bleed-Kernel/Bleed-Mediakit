#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <bleed_audio.h>
#include "mediakit_priv.h"

#define RING_FRAMES     32768
#define LEAD_MS         200
#define MARGIN_MS       20
#define CARD_MS         120
#define CHUNK           1024
#define METER_FRAMES    1024

#define FFT_N           512
#define FFT_FULL_DB     42.14f
#define VOLUME_DB       0.48f

#define NOT_YET         (~0ULL)
#define TWO_PI          6.283185307179586

struct mk_player {
    mk_media_t *media;
    au_stream_t stream;
    int         state;
    uint32_t    out_rate;
    float       gain;

    int16_t     src[CHUNK * 2];
    uint32_t    src_len, src_at;
    int         src_done;
    uint32_t    tail;
    float       win[4][2];
    uint64_t    frac;           // 32.32 between win[1] and win[2]
    uint64_t    step;

    uint64_t    written;
    uint64_t    seg_out;
    uint64_t    seg_src;
    uint64_t    end_out;
    uint64_t    held;

    int16_t     copy[RING_FRAMES * 2];
};

static float  fft_cos[FFT_N / 2];
static float  fft_sin[FFT_N / 2];
static float  fft_hann[FFT_N];
static int    fft_ready;

mk_player_t *mk_player_new(void) {
    if (!au_is_open() && au_open() < 0)
        return NULL;

    mk_player_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;

    if (au_stream_open(RING_FRAMES, &p->stream) < 0) {
        free(p);
        return NULL;
    }
    au_latency(CARD_MS);
    p->out_rate = au_rate();
    p->gain     = 1.0f;
    return p;
}

void mk_player_free(mk_player_t *p) {
    if (!p)
        return;
    mk_player_unload(p);
    au_stream_close();
    free(p);
}

static int src_pull(mk_player_t *p, float *out) {
    if (p->src_at >= p->src_len && !p->src_done) {
        p->src_len = mk_read(p->media, p->src, CHUNK);
        p->src_at  = 0;
        if (!p->src_len)
            p->src_done = 1;
    }
    if (p->src_at >= p->src_len) {
        out[0] = out[1] = 0.0f;
        p->tail++;
        return 0;
    }

    out[0] = p->src[p->src_at * 2];
    out[1] = p->src[p->src_at * 2 + 1];
    p->src_at++;
    return 1;
}

static void window_push(mk_player_t *p) {
    memmove(p->win[0], p->win[1], sizeof(p->win[0]) * 3);
    src_pull(p, p->win[3]);
}

static void restart(mk_player_t *p, uint64_t frame) {
    memset(p->stream.ring, 0, (size_t)p->stream.frames * 2 * sizeof(int16_t));
    memset(p->copy, 0, sizeof(p->copy));

    p->written = au_stream_pos() + (uint64_t)p->out_rate * MARGIN_MS / 1000;
    p->seg_out = p->written;
    p->seg_src = frame;
    p->end_out = NOT_YET;
    if (!p->media)
        return;

    mk_seek(p->media, frame);
    p->src_len  = p->src_at = 0;
    p->src_done = 0;
    p->tail     = 0;
    p->frac     = 0;
    p->step     = ((uint64_t)mk_info(p->media)->rate << 32) / p->out_rate;

    src_pull(p, p->win[1]);
    memcpy(p->win[0], p->win[1], sizeof(p->win[0]));
    src_pull(p, p->win[2]);
    src_pull(p, p->win[3]);
}

// catmull rom through the middle two
static float cubic(float a, float b, float c, float d, float t) {
    return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
}

static int16_t clip(float v) {
    if (v > 32767.0f)  return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

// n frames into the ring at written. 0 once the source is used up
static uint32_t render(mk_player_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        // two zeros in means win[1] has gone past the last real frame
        if (p->src_done && p->tail >= 2)
            return i;

        float    t   = (float)(p->frac & 0xFFFFFFFF) / 4294967296.0f;
        uint32_t at  = (uint32_t)(p->written % RING_FRAMES) * 2;
        for (int ch = 0; ch < 2; ch++) {
            float v = cubic(p->win[0][ch], p->win[1][ch], p->win[2][ch], p->win[3][ch], t) * p->gain;
            p->stream.ring[at + ch] = p->copy[at + ch] = clip(v);
        }
        p->written++;

        p->frac += p->step;
        while (p->frac >> 32) {
            p->frac -= 1ULL << 32;
            window_push(p);
        }
    }
    return n;
}

int mk_player_pump(mk_player_t *p) {
    if (p->state != MK_PLAYING)
        return -1;

    uint64_t pos = au_stream_pos();
    if (p->end_out != NOT_YET) {
        if (pos >= p->end_out) {
            p->state = MK_ENDED;
            p->held  = mk_info(p->media)->frames;
            return -1;
        }
        return (int)((p->end_out - pos) * 1000 / p->out_rate) + 1;
    }

    uint64_t margin = (uint64_t)p->out_rate * MARGIN_MS / 1000;
    if (p->written <= pos) {
        uint64_t gap = pos + margin - p->written;
        p->written += gap;
        p->seg_out += gap;
    }

    uint64_t target = pos + (uint64_t)p->out_rate * LEAD_MS / 1000;
    while (p->written < target) {
        uint32_t want = target - p->written < CHUNK ? (uint32_t)(target - p->written) : CHUNK;
        if (render(p, want) < want) {
            p->end_out = p->written;
            break;
        }
    }
    return LEAD_MS / 4;
}

int mk_player_load(mk_player_t *p, const char *path) {
    mk_media_t *m = mk_open(path);
    if (!m)
        return -1;

    mk_player_unload(p);
    p->media = m;
    p->state = MK_STOPPED;
    p->held  = 0;
    return 0;
}

void mk_player_unload(mk_player_t *p) {
    restart(p, 0);
    mk_close(p->media);
    p->media = NULL;
    p->state = MK_STOPPED;
    p->held  = 0;
}

const mk_info_t *mk_player_info(const mk_player_t *p) {
    return p->media ? mk_info(p->media) : NULL;
}

void mk_player_play(mk_player_t *p) {
    if (!p->media || p->state == MK_PLAYING)
        return;
    if (p->state == MK_ENDED)
        p->held = 0;
    restart(p, p->held);
    p->state = MK_PLAYING;
    mk_player_pump(p);
}

static uint64_t source_at(const mk_player_t *p, uint64_t pos) {
    const mk_info_t *info  = mk_info(p->media);
    uint64_t         frame = p->seg_src;
    if (pos > p->seg_out)
        frame += (pos - p->seg_out) * info->rate / p->out_rate;
    return frame < info->frames ? frame : info->frames;
}

void mk_player_pause(mk_player_t *p) {
    if (p->state != MK_PLAYING)
        return;
    // whats on the card still plays out, so carry on from where the card got to 
    p->held = source_at(p, au_stream_pos());
    restart(p, p->held);
    p->state = MK_PAUSED;
}

void mk_player_stop(mk_player_t *p) {
    restart(p, 0);
    p->held  = 0;
    p->state = MK_STOPPED;
}

void mk_player_seek_ms(mk_player_t *p, uint64_t ms) {
    if (!p->media)
        return;

    const mk_info_t *info = mk_info(p->media);
    uint64_t frame = ms * info->rate / 1000;
    if (frame > info->frames)
        frame = info->frames;

    p->held = frame;
    if (p->state == MK_ENDED)
        p->state = MK_PAUSED;
    if (p->state == MK_PLAYING) {
        restart(p, frame);
        mk_player_pump(p);
    }
}

int mk_player_state(const mk_player_t *p) {
    return p->state;
}

uint64_t mk_player_pos_ms(const mk_player_t *p) {
    if (!p->media)
        return 0;

    const mk_info_t *info = mk_info(p->media);
    uint64_t frame = p->state == MK_PLAYING ? source_at(p, au_stream_heard()) : p->held;
    return info->rate ? frame * 1000 / info->rate : 0;
}

uint64_t mk_player_length_ms(const mk_player_t *p) {
    if (!p->media)
        return 0;
    const mk_info_t *info = mk_info(p->media);
    return info->rate ? info->frames * 1000 / info->rate : 0;
}

void mk_player_volume(mk_player_t *p, int vol) {
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    p->gain = vol ? (float)pow(10.0, -(double)(100 - vol) * VOLUME_DB / 20.0) : 0.0f;
}

// meters

// the n frames heard most recently, oldest first
static void recent(const mk_player_t *p, int16_t *out, uint32_t n) {
    uint64_t pos = au_stream_heard();
    if (p->state != MK_PLAYING || pos < p->seg_out) {
        memset(out, 0, (size_t)n * 2 * sizeof(int16_t));
        return;
    }

    uint64_t from = pos >= n ? pos - n : 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t f = from + i;
        uint32_t at = (uint32_t)(f % RING_FRAMES) * 2;
        out[i * 2]     = f < pos ? p->copy[at] : 0;
        out[i * 2 + 1] = f < pos ? p->copy[at + 1] : 0;
    }
}

void mk_player_levels(const mk_player_t *p, int *left, int *right) {
    int16_t buf[METER_FRAMES * 2];
    int     peak[2] = {0, 0};

    recent(p, buf, METER_FRAMES);
    for (uint32_t i = 0; i < METER_FRAMES * 2; i++) {
        int v = buf[i] < 0 ? -buf[i] : buf[i];
        if (v > peak[i & 1])
            peak[i & 1] = v;
    }
    if (left)  *left  = peak[0] >> 7;
    if (right) *right = peak[1] >> 7;
}

static void fft_init(void) {
    for (int i = 0; i < FFT_N / 2; i++) {
        fft_cos[i] = (float)cos(TWO_PI * i / FFT_N);
        fft_sin[i] = (float)sin(TWO_PI * i / FFT_N);
    }
    for (int i = 0; i < FFT_N; i++)
        fft_hann[i] = 0.5f - 0.5f * (float)cos(TWO_PI * i / (FFT_N - 1));
    fft_ready = 1;
}

static void fft(float *re, float *im) {
    for (int i = 1, j = 0; i < FFT_N; i++) {
        int bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for (int len = 2; len <= FFT_N; len <<= 1) {
        int stride = FFT_N / len;
        for (int i = 0; i < FFT_N; i += len) {
            for (int k = 0; k < len / 2; k++) {
                float wr = fft_cos[k * stride], wi = -fft_sin[k * stride];
                float *ar = &re[i + k], *ai = &im[i + k];
                float *br = &re[i + k + len / 2], *bi = &im[i + k + len / 2];
                float tr = *br * wr - *bi * wi;
                float ti = *br * wi + *bi * wr;
                *br = *ar - tr;
                *bi = *ai - ti;
                *ar += tr;
                *ai += ti;
            }
        }
    }
}

void mk_player_spectrum(const mk_player_t *p, uint8_t *bands) {
    static float re[FFT_N], im[FFT_N];
    int16_t buf[FFT_N * 2];

    if (!fft_ready)
        fft_init();

    recent(p, buf, FFT_N);
    for (int i = 0; i < FFT_N; i++) {
        re[i] = (buf[i * 2] + buf[i * 2 + 1]) * 0.5f / 32768.0f * fft_hann[i];
        im[i] = 0.0f;
    }
    fft(re, im);

    // bands spread out logarithmically from about 60hz to 16k, the way ears hear them
    float hz_per_bin = (float)p->out_rate / FFT_N;
    for (int b = 0; b < MK_SPECTRUM; b++) {
        float lo_hz = 60.0f * (float)pow(16000.0 / 60.0, (double)b / MK_SPECTRUM);
        float hi_hz = 60.0f * (float)pow(16000.0 / 60.0, (double)(b + 1) / MK_SPECTRUM);
        int   lo    = (int)(lo_hz / hz_per_bin);
        int   hi    = (int)(hi_hz / hz_per_bin);
        if (hi <= lo) hi = lo + 1;
        if (hi > FFT_N / 2) hi = FFT_N / 2;

        float peak = 0.0f;
        for (int k = lo; k < hi; k++) {
            float mag = re[k] * re[k] + im[k] * im[k];
            if (mag > peak)
                peak = mag;
        }

        float db = 10.0f * (float)log10(peak + 1e-12) - FFT_FULL_DB;
        int   v  = (int)((db + 66.0f) * 255.0f / 60.0f);
        bands[b] = v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
    }
}
