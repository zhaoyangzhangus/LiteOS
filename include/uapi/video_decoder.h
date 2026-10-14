#pragma once
#include <stdint.h>

/* Isolated Vulkan Video decoder ABI. All calls on one instance are serialized.
 * Output storage is borrowed until the next receive/reset/destroy call.
 * NV12 readback is not zero-copy; decoding itself must run on Vulkan hardware. */
#define OS_VIDEO_DECODER_ABI 1U
#define OS_VIDEO_AGAIN (-11)
#define OS_VIDEO_EOF (-541478725)
typedef struct os_video_decoder os_video_decoder_t;
typedef struct os_video_frame {
    uint32_t width, height;
    int32_t stride_y, stride_uv;
    const uint8_t *y, *uv;
    int64_t timestamp_us;
} os_video_frame_t;

/* Validate the borrowed plane layout before any cross-library pixel copy.
 * NV12 needs two interleaved bytes for the last chroma sample of odd widths. */
static inline int os_video_frame_layout_valid(const os_video_frame_t *frame) {
    if (!frame || !frame->width || !frame->height ||
        frame->width >= INT32_MAX || frame->height > INT32_MAX ||
        !frame->y || !frame->uv || frame->stride_y <= 0 || frame->stride_uv <= 0)
        return 0;
    uint32_t chroma_bytes = ((frame->width + 1U) / 2U) * 2U;
    return (uint32_t)frame->stride_y >= frame->width &&
           (uint32_t)frame->stride_uv >= chroma_bytes;
}

#ifdef __cplusplus
extern "C" {
#endif
uint32_t os_video_decoder_abi(void);
int os_video_decoder_create_h264(const uint8_t *extra, uint32_t extra_size,
                                os_video_decoder_t **output);
/* Null packet and zero length drain the stream. */
int os_video_decoder_send(os_video_decoder_t *, const uint8_t *, uint32_t,
                          int64_t timestamp_us);
int os_video_decoder_receive(os_video_decoder_t *, os_video_frame_t *);
void os_video_decoder_reset(os_video_decoder_t *);
void os_video_decoder_destroy(os_video_decoder_t *);
#ifdef __cplusplus
}
#endif
