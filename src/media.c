#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <syscalls.h>
#include "mediakit_priv.h"

#define READ_CHUNK  (256 * 1024)

static const mk_decoder_t *decoders[] = {
    &mk_wav_decoder,
    &mk_flac_decoder,
    &mk_ogg_decoder,
    &mk_mp3_decoder,
};

#define DECODERS (sizeof(decoders) / sizeof(decoders[0]))

static mk_media_t *media_try(uint8_t *data, size_t len) {
    mk_media_t *m = calloc(1, sizeof(*m));
    if (!m) {
        free(data);
        return NULL;
    }
    m->data = data;
    m->len  = len;

    for (size_t i = 0; i < DECODERS; i++) {
        m->dec = decoders[i];
        memset(&m->info, 0, sizeof(m->info));
        if (m->dec->open(m) == 0) {
            m->info.format = m->dec->format;
            return m;
        }
    }

    free(data);
    free(m);
    return NULL;
}

mk_media_t *mk_open(const char *path) {
    int fd = _open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    long size = _seek(fd, 0, SEEK_END);
    if (size <= 0 || _seek(fd, 0, 0) != 0) {
        _close(fd);
        return NULL;
    }

    uint8_t *data = malloc((size_t)size);
    if (!data) {
        _close(fd);
        return NULL;
    }

    long got = 0;
    while (got < size) {
        long want = size - got < READ_CHUNK ? size - got : READ_CHUNK;
        long n    = _read(fd, data + got, (unsigned long)want);
        if (n <= 0)
            break;
        got += n;
    }
    _close(fd);

    if (got != size) {
        free(data);
        return NULL;
    }
    return media_try(data, (size_t)size);
}

mk_media_t *mk_open_memory(const void *data, size_t len) {
    if (!data || !len)
        return NULL;

    uint8_t *copy = malloc(len);
    if (!copy)
        return NULL;
    memcpy(copy, data, len);
    return media_try(copy, len);
}

void mk_close(mk_media_t *m) {
    if (!m)
        return;
    if (m->dec->close)
        m->dec->close(m);
    free(m->data);
    free(m);
}

const mk_info_t *mk_info(const mk_media_t *m) {
    return &m->info;
}

uint32_t mk_read(mk_media_t *m, int16_t *out, uint32_t frames) {
    if (m->pos >= m->info.frames)
        return 0;
    if (frames > m->info.frames - m->pos)
        frames = (uint32_t)(m->info.frames - m->pos);

    uint32_t n = m->dec->read(m, out, frames);
    m->pos += n;
    return n;
}

int mk_seek(mk_media_t *m, uint64_t frame) {
    if (frame > m->info.frames)
        frame = m->info.frames;
    if (m->dec->seek(m, frame) < 0)
        return -1;
    m->pos = frame;
    return 0;
}

uint64_t mk_tell(const mk_media_t *m) {
    return m->pos;
}

static int ext_is(const char *name, const char *ext) {
    size_t nl = strlen(name), el = strlen(ext);
    if (nl <= el || name[nl - el - 1] != '.')
        return 0;

    for (size_t i = 0; i < el; i++) {
        char c = name[nl - el + i];
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != ext[i])
            return 0;
    }
    return 1;
}

int mk_supported(const char *name) {
    for (size_t i = 0; i < DECODERS; i++)
        for (int e = 0; e < 4 && decoders[i]->exts[e]; e++)
            if (ext_is(name, decoders[i]->exts[e]))
                return 1;
    return 0;
}

const char *mk_format_name(int format) {
    switch (format) {
    case MK_FORMAT_WAV: return "WAV";
    case MK_FORMAT_MP3: return "MP3";
    case MK_FORMAT_FLAC: return "FLAC";
    case MK_FORMAT_OGG: return "OGG";
    }
    return "";
}

// tag text

static size_t put_utf8(char *out, size_t at, uint32_t cp) {
    char   tmp[4];
    size_t n;

    if (cp < 0x80) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | cp >> 6);
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | cp >> 12);
        tmp[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        tmp[0] = (char)(0xF0 | cp >> 18);
        tmp[1] = (char)(0x80 | (cp >> 12 & 0x3F));
        tmp[2] = (char)(0x80 | (cp >> 6 & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }

    // a character that doesnt fit whole is left off rather than cut in half
    if (at + n >= MK_TEXT_MAX)
        return at;
    memcpy(out + at, tmp, n);
    return at + n;
}

static void text_end(char *out, size_t at) {
    while (at && (out[at - 1] == ' ' || out[at - 1] == '\0'))
        at--;
    out[at] = '\0';
}

void mk_text_latin1(char *out, const uint8_t *in, size_t len) {
    size_t at = 0;
    for (size_t i = 0; i < len && in[i]; i++)
        at = put_utf8(out, at, in[i]);
    text_end(out, at);
}

void mk_text_utf8(char *out, const uint8_t *in, size_t len) {
    size_t at = 0;
    for (size_t i = 0; i < len && in[i] && at + 1 < MK_TEXT_MAX; i++)
        out[at++] = (char)in[i];

    // dont leave half a character on the end
    size_t keep = at;
    while (keep && ((uint8_t)out[keep - 1] & 0xC0) == 0x80)
        keep--;
    if (keep && (uint8_t)out[keep - 1] >= 0xC0) {
        uint8_t lead = (uint8_t)out[keep - 1];
        size_t  need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (at - (keep - 1) < need)
            at = keep - 1;
    }
    text_end(out, at);
}

void mk_text_utf16(char *out, const uint8_t *in, size_t len, int big_endian) {
    size_t at = 0;

    // a bom says which way round it is, and wins over what we were told
    if (len >= 2 && ((in[0] == 0xFF && in[1] == 0xFE) || (in[0] == 0xFE && in[1] == 0xFF))) {
        big_endian = in[0] == 0xFE;
        in  += 2;
        len -= 2;
    }

    for (size_t i = 0; i + 1 < len; i += 2) {
        uint32_t cp = big_endian ? (uint32_t)(in[i] << 8 | in[i + 1]) : (uint32_t)(in[i] | in[i + 1] << 8);
        if (!cp)
            break;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < len) {
            uint32_t lo = big_endian ? (uint32_t)(in[i + 2] << 8 | in[i + 3]) : (uint32_t)(in[i + 2] | in[i + 3] << 8);
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        at = put_utf8(out, at, cp);
    }
    text_end(out, at);
}

static int key_is(const char *comment, size_t len, const char *key) {
    size_t kl = strlen(key);
    if (len <= kl || comment[kl] != '=')
        return 0;
    for (size_t i = 0; i < kl; i++)
        if ((comment[i] & ~0x20) != key[i])
            return 0;
    return 1;
}

void mk_text_comment(mk_info_t *info, const char *comment, size_t len) {
    const uint8_t *value = (const uint8_t *)comment;

    if (key_is(comment, len, "TITLE"))
        mk_text_utf8(info->title, value + 6, len - 6);
    else if (key_is(comment, len, "ARTIST"))
        mk_text_utf8(info->artist, value + 7, len - 7);
    else if (key_is(comment, len, "ALBUM"))
        mk_text_utf8(info->album, value + 6, len - 6);
}
