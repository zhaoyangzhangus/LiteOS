"""Execute the production bridge with deterministic FFmpeg/Vulkan stand-ins.

Tests ownership/error unwinding only; this does not establish hardware decode.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/nvidia/vulkan_video_decoder.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
fixture = r'''
#include <uapi/video_decoder.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#define AVERROR(e) (-(e))
#define AV_INPUT_BUFFER_PADDING_SIZE 64
#define AV_NOPTS_VALUE INT64_MIN
#define AV_CODEC_ID_H264 1
enum AVPixelFormat { AV_PIX_FMT_NONE=-1, AV_PIX_FMT_NV12=0, AV_PIX_FMT_VULKAN=1 };
typedef struct { int num, den; } AVRational;
typedef struct { int token; } AVBufferRef;
typedef struct { int token; } AVCodec;
typedef struct AVCodecContext {
 enum AVPixelFormat (*get_format)(struct AVCodecContext*,const enum AVPixelFormat*);
 AVBufferRef *hw_device_ctx; AVRational pkt_timebase; int thread_count;
 uint8_t *extradata; int extradata_size;
} AVCodecContext;
typedef struct { int format,width,height,linesize[2]; uint8_t *data[2];
                 AVBufferRef *buf[1];
                 int64_t best_effort_timestamp; } AVFrame;
typedef struct { uint8_t *data; int size; int64_t pts,dts; } AVPacket;
static unsigned allocations, fail_at, live, flushes, sends;
static unsigned download_allocations;
static int device_result, open_result, send_result, receive_result, transfer_result;
static int hardware_format=AV_PIX_FMT_VULKAN, cpu_format=AV_PIX_FMT_NV12;
static uint8_t y[8],uv[4];
static void *allocate(size_t n,size_t size) {
 if(++allocations==fail_at)return NULL;
 void *p=calloc(n,size);if(p)++live;return p;
}
static void release(void *p) {if(p){assert(live);--live;free(p);}}
static AVBufferRef *av_buffer_ref(AVBufferRef *p) {assert(p);return allocate(1,sizeof(*p));}
static void av_buffer_unref(AVBufferRef **p) {release(*p);*p=NULL;}
static int video_device_create_mock(AVBufferRef **p) {
 if(device_result)return device_result;
 *p=allocate(1,sizeof(**p));return *p?0:AVERROR(ENOMEM);
}
int video_device_create(AVBufferRef **p) {return video_device_create_mock(p);}
static const AVCodec *avcodec_find_decoder(int id) {static const AVCodec c={1};assert(id==1);return &c;}
static AVCodecContext *avcodec_alloc_context3(const AVCodec *p) {assert(p);return allocate(1,sizeof(AVCodecContext));}
static int avcodec_open2(AVCodecContext *p,const AVCodec *c,void *o) {
 assert(p&&c&&!o&&p->thread_count==1&&p->hw_device_ctx);
 const enum AVPixelFormat f[]={AV_PIX_FMT_NV12,AV_PIX_FMT_VULKAN,AV_PIX_FMT_NONE};
 assert(p->get_format(p,f)==AV_PIX_FMT_VULKAN);return open_result;
}
static void avcodec_free_context(AVCodecContext **p) {
 if(*p){release((*p)->extradata);av_buffer_unref(&(*p)->hw_device_ctx);release(*p);*p=NULL;}
}
static AVFrame *av_frame_alloc(void) {return allocate(1,sizeof(AVFrame));}
static void av_frame_unref(AVFrame *p) {memset(p,0,sizeof(*p));}
static void av_frame_free(AVFrame **p) {release(*p);*p=NULL;}
static AVPacket *av_packet_alloc(void) {return allocate(1,sizeof(AVPacket));}
static void av_packet_unref(AVPacket *p) {release(p->data);memset(p,0,sizeof(*p));}
static void av_packet_free(AVPacket **p) {if(*p){av_packet_unref(*p);release(*p);*p=NULL;}}
static void *av_mallocz(size_t n) {return allocate(1,n);}
static int av_new_packet(AVPacket *p,int n) {p->data=allocate(1,n+64);p->size=n;return p->data?0:AVERROR(ENOMEM);}
static int avcodec_send_packet(AVCodecContext *p,const AVPacket *packet) {
 assert(p);++sends;
 if(packet){assert(packet->size==3&&packet->pts==123&&packet->dts==AV_NOPTS_VALUE);
  assert(!memcmp(packet->data,"abc",3));for(int i=0;i<64;++i)assert(!packet->data[3+i]);}
 return send_result;
}
static int avcodec_receive_frame(AVCodecContext *p,AVFrame *f) {
 assert(p);if(receive_result)return receive_result;
 f->format=hardware_format;f->width=4;f->height=2;f->best_effort_timestamp=123;return 0;
}
static int av_hwframe_transfer_data(AVFrame *to,const AVFrame *from,int flags) {
 assert(from->format==AV_PIX_FMT_VULKAN&&!flags);
 if(transfer_result)return transfer_result;
 static AVBufferRef buffer;
 if(!to->buf[0]){++download_allocations;to->buf[0]=&buffer;}
 to->format=cpu_format;to->width=4;to->height=2;to->data[0]=y;to->data[1]=uv;
 to->linesize[0]=to->linesize[1]=4;return 0;
}
static void avcodec_flush_buffers(AVCodecContext *p) {assert(p);++flushes;}
#define calloc allocate
#define free release
'''
checks = r'''
int main(void) {
 const uint8_t extra[]={1,2,3}; os_video_decoder_t *d=NULL;
 assert(!os_video_decoder_create_h264(extra,3,&d)&&d);
 assert(d->codec->extradata_size==3&&!memcmp(d->codec->extradata,extra,3));
 for(unsigned i=0;i<64;++i)assert(!d->codec->extradata[3+i]);
 unsigned owned=live;
 assert(!os_video_decoder_send(d,(const uint8_t*)"abc",3,123)&&live==owned);
 send_result=OS_VIDEO_AGAIN;
 assert(os_video_decoder_send(d,(const uint8_t*)"abc",3,123)==OS_VIDEO_AGAIN&&live==owned);
 send_result=0;assert(!os_video_decoder_send(d,NULL,0,0));
 assert(os_video_decoder_send(d,NULL,1,0)==-EINVAL);
 assert(os_video_decoder_send(d,(const uint8_t*)"abc",UINT_MAX,0)==-EINVAL);
 os_video_frame_t f;
 assert(!os_video_decoder_receive(d,&f)&&f.width==4&&f.height==2&&f.timestamp_us==123&&f.y==y&&f.uv==uv);
 for(unsigned i=0;i<1000;++i)assert(!os_video_decoder_receive(d,&f));
 assert(download_allocations==1);
 hardware_format=AV_PIX_FMT_NV12;
 assert(os_video_decoder_receive(d,&f)==-ENOSYS&&!f.y&&!f.width);
 hardware_format=AV_PIX_FMT_VULKAN;cpu_format=AV_PIX_FMT_VULKAN;
 assert(os_video_decoder_receive(d,&f)==-ENOSYS&&!f.y);
 cpu_format=AV_PIX_FMT_NV12;receive_result=OS_VIDEO_AGAIN;
 assert(os_video_decoder_receive(d,&f)==OS_VIDEO_AGAIN&&!f.y);
 receive_result=OS_VIDEO_EOF;assert(os_video_decoder_receive(d,&f)==OS_VIDEO_EOF);
 receive_result=0;transfer_result=-EIO;
 assert(os_video_decoder_receive(d,&f)==-EIO&&!f.y);transfer_result=0;
 os_video_decoder_reset(d);assert(flushes==1&&!d->packet->data);
 assert(!os_video_decoder_receive(d,&f)&&download_allocations==2);
 os_video_decoder_destroy(d);assert(!live);
 for(unsigned failure=1;failure<=8;++failure) {
  allocations=0;fail_at=failure;d=(void*)1234;
  int result=os_video_decoder_create_h264(extra,3,&d);
  assert(result==-ENOMEM&&!d&&!live);
 }
 fail_at=0;device_result=-EIO;assert(os_video_decoder_create_h264(extra,3,&d)==-EIO&&!live);
 device_result=0;open_result=-EIO;assert(os_video_decoder_create_h264(extra,3,&d)==-EIO&&!live);
 os_video_decoder_reset(NULL);os_video_decoder_destroy(NULL);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture + source + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-no-pie', '-I' + str(root / 'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Actual bridge lifecycle: padded packets/extradata, hardware-only frames, EAGAIN/EOS, reset and allocation/device/open failure cleanup passed')
