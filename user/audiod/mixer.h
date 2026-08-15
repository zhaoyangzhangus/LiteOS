#ifndef LITEOS_AUDIOD_MIXER_H
#define LITEOS_AUDIOD_MIXER_H

#include <uapi/audio.h>

#include <stdbool.h>

#define AUDIOD_MAX_CLIENTS 8U
#define AUDIOD_RING_FRAMES 1024U
#define AUDIOD_INVALID_CLIENT UINT32_MAX

#define AUDIOD_OK        0
#define AUDIOD_EINVAL   (-22)
#define AUDIOD_ENOMEM   (-12)
#define AUDIOD_EBUSY    (-16)
#define AUDIOD_EDEVREMOVED (-19)

typedef struct audiod_client {
    bool used;
    os_audio_stream_config_t config;
    uint32_t read_frame;
    uint32_t write_frame;
    uint32_t queued_frames;
    uint64_t submitted_frames;
    uint64_t underruns;
    uint64_t overruns;
    int32_t samples[AUDIOD_RING_FRAMES * OS_AUDIO_MAX_CHANNELS];
} audiod_client_t;

typedef struct audiod_server {
    bool initialized;
    os_audio_stream_config_t config;
    uint32_t state;
    uint64_t device_generation;
    uint64_t mixed_frames;
    audiod_client_t clients[AUDIOD_MAX_CLIENTS];
} audiod_server_t;

int audiod_server_init(audiod_server_t *server);
int audiod_client_open(audiod_server_t *server,
                       const os_audio_stream_config_t *config,
                       uint32_t *client_id);
int audiod_client_close(audiod_server_t *server, uint32_t client_id);
int audiod_client_submit(audiod_server_t *server, uint32_t client_id,
                         const void *samples, uint32_t frames);
int audiod_mix(audiod_server_t *server, void *output, size_t output_bytes,
               uint32_t frames);
int audiod_device_disconnect(audiod_server_t *server);
int audiod_device_recover(audiod_server_t *server);
int audiod_client_get_stats(const audiod_server_t *server, uint32_t client_id,
                            os_audio_stream_stats_t *stats);

#endif
