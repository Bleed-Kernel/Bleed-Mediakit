#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mediakit_priv.h"
// blibc should have this
static double mk_ldexp(double x, int exp) {
    while (exp > 60)  { x *= 1152921504606846976.0; exp -= 60; }
    while (exp < -60) { x /= 1152921504606846976.0; exp += 60; }
    return exp >= 0 ? x * (double)(1ULL << exp) : x / (double)(1ULL << -exp);
}

static void mk_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    uint8_t *b = base;
    uint8_t  tmp[16];
    if (size > sizeof(tmp))
        return;

    for (size_t gap = n / 2; gap; gap /= 2) {
        for (size_t i = gap; i < n; i++) {
            memcpy(tmp, b + i * size, size);
            size_t j = i;
            for (; j >= gap && cmp(b + (j - gap) * size, tmp) > 0; j -= gap)
                memcpy(b + j * size, b + (j - gap) * size, size);
            memcpy(b + j * size, tmp, size);
        }
    }
}

#define ldexp   mk_ldexp
#define qsort   mk_qsort
#define alloca  __builtin_alloca
#define M_PI    3.14159265358979323846264f

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "vendor/stb_vorbis.c"

static int ogg_open(mk_media_t *m) {
    if (m->len < 4 || memcmp(m->data, "OggS", 4) != 0)
        return -1;

    int         err;
    stb_vorbis *v = stb_vorbis_open_memory(m->data, (int)m->len, &err, NULL);
    if (!v)
        return -1;

    stb_vorbis_info    vi = stb_vorbis_get_info(v);
    stb_vorbis_comment vc = stb_vorbis_get_comment(v);
    for (int i = 0; i < vc.comment_list_length; i++)
        mk_text_comment(&m->info, vc.comment_list[i], strlen(vc.comment_list[i]));

    m->priv          = v;
    m->info.rate     = vi.sample_rate;
    m->info.channels = (uint32_t)vi.channels;
    m->info.frames   = stb_vorbis_stream_length_in_samples(v);
    if (m->info.frames && vi.sample_rate)
        m->info.bitrate = (uint32_t)((uint64_t)m->len * 8 * vi.sample_rate / m->info.frames / 1000);
    return 0;
}

static uint32_t ogg_read(mk_media_t *m, int16_t *out, uint32_t frames) {
    return (uint32_t)stb_vorbis_get_samples_short_interleaved(m->priv, 2, out, (int)frames * 2);
}

static int ogg_seek(mk_media_t *m, uint64_t frame) {
    return stb_vorbis_seek(m->priv, (unsigned int)frame) ? 0 : -1;
}

static void ogg_close(mk_media_t *m) {
    stb_vorbis_close(m->priv);
}

const mk_decoder_t mk_ogg_decoder = {
    .format = MK_FORMAT_OGG,
    .exts   = { "ogg", "oga" },
    .open   = ogg_open,
    .read   = ogg_read,
    .seek   = ogg_seek,
    .close  = ogg_close,
};
