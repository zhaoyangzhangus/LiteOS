#include "../user/audiod/mixer.h"

#include <stdio.h>

static os_audio_stream_config_t test_config(uint16_t sample_format) {
    os_audio_stream_config_t config = {0};
    config.hdr.size = sizeof(config);
    config.hdr.version = 1U;
    config.direction = OS_AUDIO_PLAYBACK;
    config.sample_rate = 48000U;
    config.channels = 2U;
    config.sample_format = sample_format;
    config.period_frames = 128U;
    config.period_count = 4U;
    return config;
}

int main(void) {
    audiod_server_t server = {0};
    os_audio_stream_config_t config = test_config(OS_AUDIO_SAMPLE_S16_LE);
    os_audio_stream_stats_t stats = {0};
    uint32_t left = AUDIOD_INVALID_CLIENT;
    uint32_t right = AUDIOD_INVALID_CLIENT;
    int16_t first[] = {1000, -1000};
    int16_t second[] = {2000, 1000};
    int16_t mixed[2] = {0};
    stats.hdr.size = sizeof(stats);
    stats.hdr.version = 1U;
    if (audiod_server_init(&server) != AUDIOD_OK ||
        audiod_client_open(&server, &config, &left) != AUDIOD_OK ||
        audiod_client_open(&server, &config, &right) != AUDIOD_OK ||
        audiod_client_submit(&server, left, first, 1U) != AUDIOD_OK ||
        audiod_client_submit(&server, right, second, 1U) != AUDIOD_OK ||
        audiod_mix(&server, mixed, sizeof(mixed), 1U) != AUDIOD_OK ||
        mixed[0] != 3000 || mixed[1] != 0) {
        puts("audiod: fail");
        return 1;
    }
    if (audiod_mix(&server, mixed, sizeof(mixed), 1U) != AUDIOD_OK ||
        audiod_client_get_stats(&server, left, &stats) != AUDIOD_OK ||
        stats.underruns == 0U || stats.mixed_frames != 2U ||
        audiod_device_disconnect(&server) != AUDIOD_OK ||
        audiod_client_submit(&server, left, first, 1U) != AUDIOD_EDEVREMOVED ||
        audiod_device_recover(&server) != AUDIOD_OK ||
        audiod_client_get_stats(&server, left, &stats) != AUDIOD_OK ||
        stats.device_generation != 3U || stats.state != OS_AUDIO_STATE_READY ||
        audiod_client_submit(&server, left, first, 1U) != AUDIOD_OK ||
        audiod_client_close(&server, left) != AUDIOD_OK ||
        audiod_client_close(&server, right) != AUDIOD_OK) {
        puts("audiod: fail");
        return 1;
    }
    puts("audiod: ok");
    return 0;
}
