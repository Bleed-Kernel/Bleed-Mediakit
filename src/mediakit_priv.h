#pragma once

#include <bleed_mediakit.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int         format;
    const char *exts[4];

    int      (*open)(mk_media_t *m);
    uint32_t (*read)(mk_media_t *m, int16_t *out, uint32_t frames);
    int      (*seek)(mk_media_t *m, uint64_t frame);
    void     (*close)(mk_media_t *m);
} mk_decoder_t;

struct mk_media {
    const mk_decoder_t *dec;
    uint8_t            *data;
    size_t              len;
    mk_info_t           info;
    uint64_t            pos;
    void               *priv;
};

extern const mk_decoder_t mk_wav_decoder;
extern const mk_decoder_t mk_mp3_decoder;
extern const mk_decoder_t mk_flac_decoder;
extern const mk_decoder_t mk_ogg_decoder;

// little and big endian out of a file
static inline uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3]; }

void mk_text_latin1(char *out, const uint8_t *in, size_t len);
void mk_text_utf8(char *out, const uint8_t *in, size_t len);
void mk_text_utf16(char *out, const uint8_t *in, size_t len, int big_endian);

void mk_text_comment(mk_info_t *info, const char *comment, size_t len);
