#include <stdlib.h>
#include <string.h>
#include "mediakit_priv.h"

#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "vendor/minimp3.h"

#define DECODER_DELAY   529
#define SEEK_PREROLL    3

typedef struct {
    mp3dec_t  dec;
    uint32_t *frames;
    uint32_t  count;
    uint32_t  spf;
    uint32_t  delay;
    uint32_t  end;

    uint32_t  next;             // the frame to decode next
    int16_t   pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    uint32_t  pcm_len;          // frames in pcm
    uint32_t  pcm_at;
    uint32_t  pcm_channels;
    uint64_t  skip;             // samples still to throw away before handing any out
} mp3_t;

static uint32_t syncsafe(const uint8_t *p) {
    return (uint32_t)(p[0] & 0x7F) << 21 | (uint32_t)(p[1] & 0x7F) << 14 | (uint32_t)(p[2] & 0x7F) << 7 | (p[3] & 0x7F);
}

static void id3_text(char *out, const uint8_t *p, uint32_t len) {
    if (!len)
        return;

    switch (p[0]) {
    case 0:  mk_text_latin1(out, p + 1, len - 1); break;
    case 1:  mk_text_utf16(out, p + 1, len - 1, 0); break;
    case 2:  mk_text_utf16(out, p + 1, len - 1, 1); break;
    default: mk_text_utf8(out, p + 1, len - 1); break;
    }
}

static size_t id3v2(mk_media_t *m) {
    const uint8_t *p = m->data;
    if (m->len < 10 || memcmp(p, "ID3", 3) != 0)
        return 0;

    int      ver   = p[3];
    uint32_t size  = syncsafe(p + 6);
    size_t   total = 10 + (size_t)size + ((p[5] & 0x10) ? 10 : 0);
    if (total > m->len)
        return m->len;

    // 2.2 has three letter ids and three byte sizes
    size_t   head  = ver == 2 ? 6 : 10;
    size_t   at    = 10;
    size_t   end   = 10 + size;
    if (ver >= 3 && (p[5] & 0x40) && at + 4 <= end)
        at += ver == 4 ? syncsafe(p + at) : rd32be(p + at) + 4;

    while (at + head <= end && p[at]) {
        const uint8_t *f = p + at;
        uint32_t len;
        if (ver == 2)
            len = (uint32_t)f[3] << 16 | f[4] << 8 | f[5];
        else
            len = ver == 4 ? syncsafe(f + 4) : rd32be(f + 4);
        if (len > end - at - head)
            break;

        const uint8_t *body = f + head;
        if (!memcmp(f, ver == 2 ? "TT2" : "TIT2", ver == 2 ? 3 : 4))
            id3_text(m->info.title, body, len);
        else if (!memcmp(f, ver == 2 ? "TP1" : "TPE1", ver == 2 ? 3 : 4))
            id3_text(m->info.artist, body, len);
        else if (!memcmp(f, ver == 2 ? "TAL" : "TALB", ver == 2 ? 3 : 4))
            id3_text(m->info.album, body, len);
        at += head + len;
    }
    return total;
}

// id3v1 is the last 128 bytes, only used for what v2 didnt say
static size_t id3v1(mk_media_t *m, size_t start) {
    if (m->len < start + 128)
        return m->len;

    const uint8_t *t = m->data + m->len - 128;
    if (memcmp(t, "TAG", 3) != 0)
        return m->len;

    if (!m->info.title[0])  mk_text_latin1(m->info.title, t + 3, 30);
    if (!m->info.artist[0]) mk_text_latin1(m->info.artist, t + 33, 30);
    if (!m->info.album[0])  mk_text_latin1(m->info.album, t + 63, 30);
    return m->len - 128;
}

static int xing(const uint8_t *f, uint32_t bytes, uint32_t *delay, uint32_t *padding) {
    int mpeg1 = HDR_TEST_MPEG1(f) != 0;
    int mono  = HDR_IS_MONO(f);
    uint32_t at = 4 + (HDR_IS_CRC(f) ? 2 : 0) + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
    if (at + 8 > bytes)
        return 0;

    const uint8_t *tag = f + at;
    if (memcmp(tag, "Xing", 4) != 0 && memcmp(tag, "Info", 4) != 0)
        return 0;

    uint32_t flags = rd32be(tag + 4);
    at += 8;
    if (flags & 1) at += 4;
    if (flags & 2) at += 4;
    if (flags & 4) at += 100;
    if (flags & 8) at += 4;

    if (at + 24 <= bytes && !memcmp(f + at, "LAME", 4)) {
        const uint8_t *d = f + at + 21;
        *delay   = (uint32_t)(d[0] << 4 | d[1] >> 4);
        *padding = (uint32_t)((d[1] & 0xF) << 8 | d[2]);
    }
    return 1;
}

static int mp3_open(mk_media_t *m) {
    size_t start = id3v2(m);
    size_t end   = id3v1(m, start);
    if (start >= end)
        return -1;

    mp3_t *s = calloc(1, sizeof(*s));
    if (!s)
        return -1;

    uint32_t cap = 0, enc_delay = 0, padding = 0, hz = 0, channels = 0;
    uint64_t bytes_total = 0;
    mp3dec_frame_info_t fi;

    mp3dec_init(&s->dec);
    for (size_t at = start; at < end;) {
        int spf = mp3dec_decode_frame(&s->dec, m->data + at, (int)(end - at), NULL, &fi);
        if (!spf) {
            if (!fi.frame_bytes)
                break;
            at += fi.frame_bytes;
            continue;
        }
        at += fi.frame_offset;

        if (!s->count && !hz) {
            hz       = (uint32_t)fi.hz;
            channels = (uint32_t)fi.channels;
            s->spf   = (uint32_t)spf;
            if (xing(m->data + at, (uint32_t)fi.frame_bytes, &enc_delay, &padding)) {
                at += fi.frame_bytes;
                continue;
            }
        }

        if (s->count == cap) {
            cap = cap ? cap * 2 : 1024;
            uint32_t *grown = realloc(s->frames, cap * sizeof(uint32_t));
            if (!grown)
                break;
            s->frames = grown;
        }
        s->frames[s->count++] = (uint32_t)at;
        bytes_total += (uint64_t)fi.frame_bytes;
        at += fi.frame_bytes;
    }

    if (s->count < 4) {
        free(s->frames);
        free(s);
        return -1;
    }

    uint64_t raw  = (uint64_t)s->count * s->spf;
    uint64_t trim = 0;
    if (enc_delay) {
        s->delay = enc_delay + DECODER_DELAY;
        trim     = (uint64_t)s->delay + padding;
    }
    s->end = (uint32_t)end;

    m->priv          = s;
    m->info.rate     = hz;
    m->info.channels = channels;
    m->info.frames   = raw > trim ? raw - trim : raw;
    m->info.bitrate  = (uint32_t)(bytes_total * 8 * hz / (raw ? raw : 1) / 1000);
    s->skip          = s->delay;
    mp3dec_init(&s->dec);
    return 0;
}

static int mp3_decode(mk_media_t *m, mp3_t *s) {
    while (s->next < s->count) {
        uint32_t at = s->frames[s->next++];
        mp3dec_frame_info_t fi;

        int n = mp3dec_decode_frame(&s->dec, m->data + at, (int)(s->end - at), s->pcm, &fi);
        if (n <= 0) {
            n = (int)s->spf;
            fi.channels = 2;
            memset(s->pcm, 0, (size_t)n * 2 * sizeof(int16_t));
        }
        s->pcm_len      = (uint32_t)n;
        s->pcm_at       = 0;
        s->pcm_channels = (uint32_t)fi.channels;
        return 1;
    }
    return 0;
}

static uint32_t mp3_read(mk_media_t *m, int16_t *out, uint32_t frames) {
    mp3_t   *s    = m->priv;
    uint32_t done = 0;

    while (done < frames) {
        if (s->pcm_at >= s->pcm_len && !mp3_decode(m, s))
            break;

        uint32_t avail = s->pcm_len - s->pcm_at;
        if (s->skip) {
            uint32_t drop = s->skip < avail ? (uint32_t)s->skip : avail;
            s->pcm_at += drop;
            s->skip   -= drop;
            continue;
        }

        uint32_t n = frames - done < avail ? frames - done : avail;
        const int16_t *in = s->pcm + s->pcm_at * s->pcm_channels;
        if (s->pcm_channels == 2) {
            memcpy(out + done * 2, in, n * 2 * sizeof(int16_t));
        } else {
            for (uint32_t i = 0; i < n; i++)
                out[(done + i) * 2] = out[(done + i) * 2 + 1] = in[i];
        }
        s->pcm_at += n;
        done      += n;
    }
    return done;
}

static int mp3_seek(mk_media_t *m, uint64_t frame) {
    mp3_t   *s     = m->priv;
    uint64_t raw   = frame + s->delay;
    uint32_t want  = (uint32_t)(raw / s->spf);
    uint32_t first = want > SEEK_PREROLL ? want - SEEK_PREROLL : 0;

    mp3dec_init(&s->dec);
    s->next    = first;
    s->pcm_len = 0;
    s->pcm_at  = 0;
    s->skip    = raw - (uint64_t)first * s->spf;
    return 0;
}

static void mp3_close(mk_media_t *m) {
    mp3_t *s = m->priv;
    if (!s)
        return;
    free(s->frames);
    free(s);
}

const mk_decoder_t mk_mp3_decoder = {
    .format = MK_FORMAT_MP3,
    .exts   = { "mp3", "mp2", "mpga" },
    .open   = mp3_open,
    .read   = mp3_read,
    .seek   = mp3_seek,
    .close  = mp3_close,
};
