#include <stdlib.h>
#include <string.h>
#include "mediakit_priv.h"

#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_IMPLEMENTATION
#include "vendor/dr_flac.h"

static void flac_meta(void *user, drflac_metadata *meta) {
    if (meta->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT)
        return;

    drflac_vorbis_comment_iterator it;
    drflac_uint32 len;
    const char   *comment;

    drflac_init_vorbis_comment_iterator(&it, meta->data.vorbis_comment.commentCount, meta->data.vorbis_comment.pComments);
    while ((comment = drflac_next_vorbis_comment(&it, &len)))
        mk_text_comment(user, comment, len);
}

static int flac_open(mk_media_t *m) {
    if (m->len < 4 || memcmp(m->data, "fLaC", 4) != 0)
        return -1;

    drflac *f = drflac_open_memory_with_metadata(m->data, m->len, flac_meta, &m->info, NULL);
    if (!f)
        return -1;

    m->priv          = f;
    m->info.rate     = f->sampleRate;
    m->info.channels = f->channels;
    m->info.bits     = f->bitsPerSample;
    m->info.frames   = f->totalPCMFrameCount;
    if (f->totalPCMFrameCount && f->sampleRate)
        m->info.bitrate = (uint32_t)((uint64_t)m->len * 8 * f->sampleRate / f->totalPCMFrameCount / 1000);
    return 0;
}

static uint32_t flac_read(mk_media_t *m, int16_t *out, uint32_t frames) {
    drflac  *f    = m->priv;
    uint32_t done = 0;

    if (f->channels == 2)
        return (uint32_t)drflac_read_pcm_frames_s16(f, frames, out);

    // anything else comes out as its own channels, mono doubles up and past two are left off
    int16_t buf[1024 * 8];
    uint32_t per = sizeof(buf) / sizeof(buf[0]) / f->channels;
    while (done < frames) {
        uint32_t want = frames - done < per ? frames - done : per;
        uint32_t got  = (uint32_t)drflac_read_pcm_frames_s16(f, want, buf);
        if (!got)
            break;

        for (uint32_t i = 0; i < got; i++) {
            const int16_t *in = buf + i * f->channels;
            out[(done + i) * 2]     = in[0];
            out[(done + i) * 2 + 1] = f->channels > 1 ? in[1] : in[0];
        }
        done += got;
    }
    return done;
}

static int flac_seek(mk_media_t *m, uint64_t frame) {
    return drflac_seek_to_pcm_frame(m->priv, frame) ? 0 : -1;
}

static void flac_close(mk_media_t *m) {
    drflac_close(m->priv);
}

const mk_decoder_t mk_flac_decoder = {
    .format = MK_FORMAT_FLAC,
    .exts   = { "flac" },
    .open   = flac_open,
    .read   = flac_read,
    .seek   = flac_seek,
    .close  = flac_close,
};
