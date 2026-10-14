#include <uapi/video_decoder.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct os_video_decoder {
    AVCodecContext *codec;
    AVBufferRef *device;
    AVFrame *hardware, *download;
    AVPacket *packet;
};

int video_device_create(AVBufferRef **output);

static enum AVPixelFormat select_hardware(AVCodecContext *codec,
                                         const enum AVPixelFormat *formats) {
    (void)codec;
    for (; *formats != AV_PIX_FMT_NONE; ++formats)
        if (*formats == AV_PIX_FMT_VULKAN) return *formats;
    /* Never permit FFmpeg to silently decode H.264 on the CPU. */
    return AV_PIX_FMT_NONE;
}

uint32_t os_video_decoder_abi(void) { return OS_VIDEO_DECODER_ABI; }

void os_video_decoder_destroy(os_video_decoder_t *decoder) {
    if (!decoder) return;
    av_packet_free(&decoder->packet);
    av_frame_free(&decoder->download);
    av_frame_free(&decoder->hardware);
    avcodec_free_context(&decoder->codec);
    av_buffer_unref(&decoder->device);
    free(decoder);
}

int os_video_decoder_create_h264(const uint8_t *extra, uint32_t extra_size,
                                os_video_decoder_t **output) {
    if (!output) return AVERROR(EINVAL);
    *output = NULL;
    if ((!extra && extra_size) || extra_size > 16U * 1024U * 1024U)
        return AVERROR(EINVAL);
    os_video_decoder_t *decoder = calloc(1, sizeof(*decoder));
    if (!decoder) return AVERROR(ENOMEM);
    int result = video_device_create(&decoder->device);
    if (result < 0) goto fail;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { result = AVERROR(ENOSYS); goto fail; }
    decoder->codec = avcodec_alloc_context3(codec);
    decoder->hardware = av_frame_alloc();
    decoder->download = av_frame_alloc();
    decoder->packet = av_packet_alloc();
    if (!decoder->codec || !decoder->hardware || !decoder->download ||
        !decoder->packet) { result = AVERROR(ENOMEM); goto fail; }
    decoder->codec->get_format = select_hardware;
    decoder->codec->hw_device_ctx = av_buffer_ref(decoder->device);
    if (!decoder->codec->hw_device_ctx) { result = AVERROR(ENOMEM); goto fail; }
    decoder->codec->pkt_timebase = (AVRational){1, 1000000};
    decoder->codec->thread_count = 1;
    if (extra_size) {
        decoder->codec->extradata = av_mallocz(
            (size_t)extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!decoder->codec->extradata) { result = AVERROR(ENOMEM); goto fail; }
        memcpy(decoder->codec->extradata, extra, extra_size);
        decoder->codec->extradata_size = (int)extra_size;
    }
    result = avcodec_open2(decoder->codec, codec, NULL);
    if (result < 0) goto fail;
    *output = decoder;
    return 0;
fail:
    os_video_decoder_destroy(decoder);
    return result;
}

int os_video_decoder_send(os_video_decoder_t *decoder, const uint8_t *data,
                          uint32_t size, int64_t timestamp_us) {
    if (!decoder || (!data && size) || size > INT_MAX)
        return AVERROR(EINVAL);
    if (!data && !size) return avcodec_send_packet(decoder->codec, NULL);
    if (!size) return AVERROR(EINVAL);
    av_packet_unref(decoder->packet);
    int result = av_new_packet(decoder->packet, (int)size);
    if (result < 0) return result;
    memcpy(decoder->packet->data, data, size);
    decoder->packet->pts = timestamp_us;
    decoder->packet->dts = AV_NOPTS_VALUE;
    result = avcodec_send_packet(decoder->codec, decoder->packet);
    av_packet_unref(decoder->packet);
    return result;
}

int os_video_decoder_receive(os_video_decoder_t *decoder,
                             os_video_frame_t *output) {
    if (!decoder || !output) return AVERROR(EINVAL);
    memset(output, 0, sizeof(*output));
    av_frame_unref(decoder->hardware);
    int result = avcodec_receive_frame(decoder->codec, decoder->hardware);
    if (result < 0) return result;
    if (decoder->hardware->format != AV_PIX_FMT_VULKAN)
        return AVERROR(ENOSYS);
    /* The caller copies borrowed pixels before receiving another frame.
     * Keep writable NV12 storage until reset or an actual size change. */
    if (decoder->download->buf[0] &&
        (decoder->download->width != decoder->hardware->width ||
         decoder->download->height != decoder->hardware->height))
        av_frame_unref(decoder->download);
    result = av_hwframe_transfer_data(decoder->download, decoder->hardware, 0);
    if (result < 0) return result;
    AVFrame *frame = decoder->download;
    if (frame->format != AV_PIX_FMT_NV12 || frame->width <= 0 ||
        frame->height <= 0 || !frame->data[0] || !frame->data[1] ||
        frame->linesize[0] < frame->width ||
        frame->width == INT_MAX ||
        frame->linesize[1] < ((frame->width + 1) / 2) * 2)
        return AVERROR(ENOSYS);
    *output = (os_video_frame_t){
        .width = (uint32_t)frame->width, .height = (uint32_t)frame->height,
        .stride_y = frame->linesize[0], .stride_uv = frame->linesize[1],
        .y = frame->data[0], .uv = frame->data[1],
        .timestamp_us = decoder->hardware->best_effort_timestamp,
    };
    return 0;
}

void os_video_decoder_reset(os_video_decoder_t *decoder) {
    if (!decoder) return;
    avcodec_flush_buffers(decoder->codec);
    av_frame_unref(decoder->hardware);
    av_frame_unref(decoder->download);
    av_packet_unref(decoder->packet);
}
