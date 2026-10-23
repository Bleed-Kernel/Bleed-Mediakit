#pragma once

#include <stdint.h>
#include <stddef.h>


#define MK_TEXT_MAX     128
#define MK_SPECTRUM     16

enum {
    MK_FORMAT_NONE,
    MK_FORMAT_WAV,
    MK_FORMAT_MP3,
    MK_FORMAT_FLAC,
    MK_FORMAT_OGG,
};

typedef struct {
    int      format;            // MK_FORMAT_*
    uint32_t rate;
    uint32_t channels;
    uint64_t frames;
    uint32_t bitrate;
    uint32_t bits;              // 0 when its compressed
    char     title[MK_TEXT_MAX];
    char     artist[MK_TEXT_MAX];
    char     album[MK_TEXT_MAX];
} mk_info_t;

// decoding

typedef struct mk_media mk_media_t;

// a file by what it starts with, not its name. NULL if its nothing we can read
mk_media_t *mk_open(const char *path);

// the same from memory thats already been read. the kit keeps its own copy
mk_media_t *mk_open_memory(const void *data, size_t len);
void        mk_close(mk_media_t *m);

const mk_info_t *mk_info(const mk_media_t *m);

// up to frames of s16 stereo at the files own rate. 0 at the end
uint32_t mk_read(mk_media_t *m, int16_t *out, uint32_t frames);

// to a frame at the files rate, -1 if it cant
int      mk_seek(mk_media_t *m, uint64_t frame);
uint64_t mk_tell(const mk_media_t *m);

// whether a name looks like something mk_open would take, for listing a folder
int mk_supported(const char *name);

// "MP3", "WAV", "FLAC", "OGG"
const char *mk_format_name(int format);

enum {
    MK_STOPPED,
    MK_PLAYING,
    MK_PAUSED,
    MK_ENDED,                   // ran off the end, stays here until something else is asked for
};

typedef struct mk_player mk_player_t;

// opens the card through libaudio if nobody has yet. NULL without a card
mk_player_t *mk_player_new(void);
void         mk_player_free(mk_player_t *p);

int  mk_player_load(mk_player_t *p, const char *path);
void mk_player_unload(mk_player_t *p);

const mk_info_t *mk_player_info(const mk_player_t *p);

void mk_player_play(mk_player_t *p);
void mk_player_pause(mk_player_t *p);
void mk_player_stop(mk_player_t *p);
void mk_player_seek_ms(mk_player_t *p, uint64_t ms);
int  mk_player_state(const mk_player_t *p);

// where its at as near as the card can tell us, and how long the whole thing is
uint64_t mk_player_pos_ms(const mk_player_t *p);
uint64_t mk_player_length_ms(const mk_player_t *p);

// 0-100, on top of the cards own volume
void mk_player_volume(mk_player_t *p, int vol);
int  mk_player_pump(mk_player_t *p); // keep the caller fed

// how loud whats coming out right now is, 0-255 a side
void mk_player_levels(const mk_player_t *p, int *left, int *right);

// MK_SPECTRUM bands of whats coming out, low to high, 0-255 each
void mk_player_spectrum(const mk_player_t *p, uint8_t *bands);
