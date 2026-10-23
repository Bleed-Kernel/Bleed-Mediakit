#include <stdlib.h>
#include <string.h>
#include "mediakit_priv.h"

#define WAV_PCM         1
#define WAV_FLOAT       3
#define WAV_EXTENSIBLE  0xFFFE

typedef struct {
    const uint8_t *samples;
    uint32_t       tag;         // WAV_PCM or WAV_FLOAT
    uint32_t       bytes;       // a sample
    uint32_t       align;       // a frame, every channel
} wav_t;

static void wav_tags(mk_media_t *m, const uint8_t *p, uint32_t size) {
    if (size < 4 || memcmp(p, "INFO", 4) != 0)
        return;

    for (uint32_t at = 4; at + 8 <= size;) {
        uint32_t len = rd32le(p + at + 4);
        if (len > size - at - 8)
            break;

        const uint8_t *text = p + at + 8;
        if (!memcmp(p + at, "INAM", 4))
            mk_text_utf8(m->info.title, text, len);
        else if (!memcmp(p + at, "IART", 4))
            mk_text_utf8(m->info.artist, text, len);
        else if (!memcmp(p + at, "IPRD", 4))
            mk_text_utf8(m->info.album, text, len);
        at += 8 + len + (len & 1);
    }
}

static int wav_open(mk_media_t *m) {
    const uint8_t *p = m->data;
    if (m->len < 12 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0)
        return -1;

    wav_t   w = {0};
    uint32_t channels = 0, rate = 0, bits = 0, data_len = 0;

    for (size_t at = 12; at + 8 <= m->len;) {
        uint32_t size = rd32le(p + at + 4);
        const uint8_t *body = p + at + 8;
        size_t room = m->len - at - 8;

        if (!memcmp(p + at, "data", 4)) {
            w.samples = body;
            data_len  = (size && size <= room) ? size : (uint32_t)room;
        } else if (size > room) {
            break;
        } else if (!memcmp(p + at, "fmt ", 4) && size >= 16) {
            w.tag    = rd16le(body);
            channels = rd16le(body + 2);
            rate     = rd32le(body + 4);
            bits     = rd16le(body + 14);
            if (w.tag == WAV_EXTENSIBLE && size >= 26)
                w.tag = rd16le(body + 24);
        } else if (!memcmp(p + at, "LIST", 4)) {
            wav_tags(m, body, size);
        }

        if (w.samples && w.tag)
            break;
        at += 8 + (size_t)size + (size & 1);
    }

    if (!w.samples || !channels || !rate)
        return -1;
    if (w.tag == WAV_PCM && bits != 8 && bits != 16 && bits != 24 && bits != 32)
        return -1;
    if (w.tag == WAV_FLOAT && bits != 32)
        return -1;
    if (w.tag != WAV_PCM && w.tag != WAV_FLOAT)
        return -1;

    w.bytes = bits / 8;
    w.align = w.bytes * channels;

    wav_t *priv = malloc(sizeof(*priv));
    if (!priv)
        return -1;
    *priv   = w;
    m->priv = priv;

    m->info.rate     = rate;
    m->info.channels = channels;
    m->info.bits     = bits;
    m->info.frames   = data_len / w.align;
    m->info.bitrate  = (uint32_t)((uint64_t)rate * w.align * 8 / 1000);
    return 0;
}

static int16_t wav_sample(const wav_t *w, const uint8_t *p) {
    if (w->tag == WAV_FLOAT) {
        float f;
        memcpy(&f, p, 4);
        if (f >= 1.0f)  return 32767;
        if (f <= -1.0f) return -32768;
        return (int16_t)(f * 32767.0f);
    }

    switch (w->bytes) {
    case 1:  return (int16_t)((p[0] - 128) << 8);
    case 2:  return (int16_t)rd16le(p);
    case 3:  return (int16_t)(p[1] | p[2] << 8);
    }
    return (int16_t)rd16le(p + 2);
}

static uint32_t wav_read(mk_media_t *m, int16_t *out, uint32_t frames) {
    const wav_t   *w  = m->priv;
    const uint8_t *at = w->samples + m->pos * w->align;

    for (uint32_t i = 0; i < frames; i++, at += w->align) {
        int16_t l = wav_sample(w, at);
        out[i * 2]     = l;
        out[i * 2 + 1] = m->info.channels > 1 ? wav_sample(w, at + w->bytes) : l;
    }
    return frames;
}

static int wav_seek(mk_media_t *m, uint64_t frame) {
    (void)m;
    (void)frame;
    return 0;
}

static void wav_close(mk_media_t *m) {
    free(m->priv);
}

const mk_decoder_t mk_wav_decoder = {
    .format = MK_FORMAT_WAV,
    .exts   = { "wav", "wave" },
    .open   = wav_open,
    .read   = wav_read,
    .seek   = wav_seek,
    .close  = wav_close,
};
