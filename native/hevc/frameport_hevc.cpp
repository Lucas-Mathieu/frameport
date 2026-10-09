// SPDX-License-Identifier: GPL-3.0-only
// Android OMX integration for the Iris stateful V4L2 HEVC decoder. FFmpeg is
// built with only its hardware wrapper; this component never transcodes video.
#define LOG_TAG "FramePortHEVC"
#include <media/stagefright/omx/SoftVideoDecoderOMXComponent.h>
#include <media/hardware/OMXPluginBase.h>
#include <media/openmax/OMX_IndexExt.h>
#include <media/hardware/HardwareAPI.h>
#include <android/hardware_buffer.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <utils/String8.h>
#include <arm_neon.h>
#include <cstring>
#include <cerrno>
#include <pthread.h>
#include <time.h>
#include "yuv_copy.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
struct YuvConstants;
extern const YuvConstants kYvuH709Constants, kYvuI601Constants;
void NV21ToARGBRow_Any_NEON(const uint8_t*,const uint8_t*,uint8_t*,const YuvConstants*,int);
AHardwareBuffer *ANativeWindowBuffer_getHardwareBuffer(ANativeWindowBuffer*);
}

namespace android {
static const CodecProfileLevel profiles[]={{OMX_VIDEO_HEVCProfileMain,OMX_VIDEO_HEVCMainTierLevel62}};
class ColorWorkers {
    struct Worker {ColorWorkers *owner;unsigned band;pthread_t thread;};
    Worker workers[3];unsigned count=0,generation=0,complete=0;
    bool started=false,stopping=false;
    pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t work=PTHREAD_COND_INITIALIZER,done=PTHREAD_COND_INITIALIZER;
    AVFrame *frame=nullptr;uint8_t *output=nullptr;size_t stride=0;
    const YuvConstants *matrix=nullptr;
    FramePortYuvPlane planes[3]={};bool native_yuv=false;
    void rows(unsigned band) {
        unsigned bands=count+1;
        if(native_yuv){
            frameport_yuv_copy_band(frame->data[0],frame->data[1],frame->width,frame->height,
                frame->linesize[0],frame->linesize[1],planes,band,bands);return;
        }
        int begin=frame->height*band/bands,end=frame->height*(band+1)/bands;
        for(int row=begin;row<end;row++)
            NV21ToARGBRow_Any_NEON(frame->data[0]+row*frame->linesize[0],
                frame->data[1]+(row/2)*frame->linesize[1],output+row*stride*4,matrix,frame->width);
    }
    static void *thread(void *arg) {
        auto &worker=*static_cast<Worker*>(arg);auto &pool=*worker.owner;unsigned seen=0;
        pthread_mutex_lock(&pool.mutex);
        for(;;) {
            while(!pool.stopping && seen==pool.generation)pthread_cond_wait(&pool.work,&pool.mutex);
            if(pool.stopping)break;
            seen=pool.generation;pthread_mutex_unlock(&pool.mutex);
            pool.rows(worker.band);
            pthread_mutex_lock(&pool.mutex);
            if(++pool.complete==pool.count)pthread_cond_signal(&pool.done);
        }
        pthread_mutex_unlock(&pool.mutex);return nullptr;
    }
public:
    void convert(AVFrame *source,uint8_t *destination,size_t pitch,const YuvConstants *constants,
        const AHardwareBuffer_Planes *native=nullptr) {
        if(!started) {
            started=true;
            if(source->width*source->height>=4096*2048)
                for(unsigned i=0;i<3;i++) {
                    workers[i]={this,i+1,{}};
                    if(pthread_create(&workers[i].thread,nullptr,thread,&workers[i]))break;
                    count++;
                }
        }
        pthread_mutex_lock(&mutex);
        frame=source;output=destination;stride=pitch;matrix=constants;complete=0;generation++;
        native_yuv=native!=nullptr;
        if(native)for(unsigned i=0;i<3;i++)planes[i]={static_cast<uint8_t*>(native->planes[i].data),
            native->planes[i].rowStride,native->planes[i].pixelStride};
        pthread_cond_broadcast(&work);pthread_mutex_unlock(&mutex);
        rows(0);
        pthread_mutex_lock(&mutex);
        while(complete<count)pthread_cond_wait(&done,&mutex);
        pthread_mutex_unlock(&mutex);
    }
    ~ColorWorkers() {
        pthread_mutex_lock(&mutex);stopping=true;pthread_cond_broadcast(&work);pthread_mutex_unlock(&mutex);
        for(unsigned i=0;i<count;i++)pthread_join(workers[i].thread,nullptr);
        pthread_cond_destroy(&work);pthread_cond_destroy(&done);pthread_mutex_destroy(&mutex);
    }
};
class FramePortHEVC final : public SoftVideoDecoderOMXComponent {
    ColorWorkers color;
    AVCodecContext *codec=nullptr;
    AVFrame *frame=av_frame_alloc();
    uint8_t *config=nullptr;
    size_t config_size=0;
    bool failed=false,held=false,drained=false,replace_config=false,surface=false;
    bool native_enabled=false,metadata=false;
    static constexpr OMX_INDEXTYPE native_index=(OMX_INDEXTYPE)0x7f010001;
    static constexpr OMX_INDEXTYPE metadata_index=(OMX_INDEXTYPE)0x7f010002;
    static constexpr OMX_INDEXTYPE usage_index=(OMX_INDEXTYPE)0x7f010003;
    int64_t eos_pts=0;
    static int64_t clockNs() {timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec*1000000000ll+ts.tv_nsec;}
    int64_t stats_start=0, stats_lock=0, stats_convert=0, stats_unlock=0;
    unsigned stats_frames=0;
    void fail(int result,const char *operation) {
        char message[128];av_strerror(result,message,sizeof(message));
        ALOGE("%s failed: %s",operation,message);
        failed=true;notify(OMX_EventError,OMX_ErrorHardware,0,nullptr);
    }
    bool openDecoder() {
        if(codec)return true;
        const AVCodec *hardware=avcodec_find_decoder_by_name("hevc_v4l2m2m");
        if(!hardware || !frame){fail(AVERROR(ENOMEM),"decoder allocation");return false;}
        codec=avcodec_alloc_context3(hardware);
        if(!codec){fail(AVERROR(ENOMEM),"codec context");return false;}
        const auto &input=editPortInfo(kInputPortIndex)->mDef.format.video;
        codec->width=codec->coded_width=input.nFrameWidth;
        codec->height=codec->coded_height=input.nFrameHeight;
        codec->pkt_timebase=AVRational{1,1000000};
        codec->pix_fmt=AV_PIX_FMT_NV12;
        if(config_size) {
            codec->extradata=(uint8_t*)av_mallocz(config_size+AV_INPUT_BUFFER_PADDING_SIZE);
            if(!codec->extradata){fail(AVERROR(ENOMEM),"codec configuration");return false;}
            memcpy(codec->extradata,config,config_size);codec->extradata_size=(int)config_size;
        }
        AVDictionary *options=nullptr;
        av_dict_set(&options,"num_output_buffers","2",0);
        av_dict_set(&options,"num_capture_buffers","4",0);
        int result=avcodec_open2(codec,hardware,&options);av_dict_free(&options);
        if(result<0){fail(result,"Iris hardware initialization");return false;}
        ALOGI("Iris hardware HEVC decoder active: %dx%d",codec->coded_width,codec->coded_height);
        return true;
    }
    void consumeInput(BufferInfo *info) {
        OMX_BUFFERHEADERTYPE *header=info->mHeader;
        getPortQueue(kInputPortIndex).erase(getPortQueue(kInputPortIndex).begin());
        header->nOffset=0;header->nFilledLen=0;info->mOwnedByUs=false;
        notifyEmptyBufferDone(header);
    }
    bool outputFrame() {
        bool reset=false;
        handlePortSettingsChange(&reset,frame->width,frame->height,
            native_enabled?(OMX_COLOR_FORMATTYPE)AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420:
                surface?OMX_COLOR_Format32BitRGBA8888:OMX_COLOR_FormatYUV420Planar);
        rgbaPort();
        if(reset)return false;
        auto &queue=getPortQueue(kOutputPortIndex);
        if(queue.empty())return false;
        BufferInfo *info=*queue.begin();OMX_BUFFERHEADERTYPE *header=info->mHeader;
        size_t stride=outputBufferWidth(),height=outputBufferHeight();
        size_t bytes=native_enabled?sizeof(VideoNativeMetadata):surface?stride*height*4:stride*height*3/2;
        if(bytes>header->nAllocLen || frame->format!=AV_PIX_FMT_NV12 ||
           frame->width<1 || frame->height<1 || (frame->width&1) || (frame->height&1) ||
           (size_t)frame->width>stride || (size_t)frame->height>height) {
            fail(AVERROR(EINVAL),"output buffer format");return false;
        }
        // Native surfaces keep YUV planes for GPU colour conversion. Legacy
        // software surfaces use parallel NEON RGBA conversion, bypassing
        // Lepton's slow scalar RGB565 renderer.
        if(surface || native_enabled) {
            int64_t before=clockNs(),locked=before,converted=before;
            const YuvConstants *matrix=mDefaultColorAspects.mMatrixCoeffs==ColorAspects::MatrixBT709_5
                ?&kYvuH709Constants:&kYvuI601Constants;
            uint8_t *destination=header->pBuffer;
            AHardwareBuffer *hardware=nullptr;VideoNativeMetadata *native=nullptr;
            AHardwareBuffer_Planes planes={};
            if(native_enabled) {
                native=reinterpret_cast<VideoNativeMetadata*>(header->pBuffer);
                if(!metadata || native->eType!=kMetadataBufferTypeANWBuffer || !native->pBuffer) {
                    fail(AVERROR(EINVAL),"native output metadata");return false;
                }
                hardware=ANativeWindowBuffer_getHardwareBuffer(native->pBuffer);
                if(!hardware){fail(AVERROR(EINVAL),"native hardware buffer");return false;}
                AHardwareBuffer_Desc desc={};AHardwareBuffer_describe(hardware,&desc);
                if(desc.format!=AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420 || desc.width<(unsigned)frame->width ||
                    desc.height<(unsigned)frame->height) {
                    fail(AVERROR(EINVAL),"native output geometry");return false;
                }
                int result=AHardwareBuffer_lockPlanes(hardware,AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                    native->nFenceFd,nullptr,&planes);
                if(native->nFenceFd>=0)close(native->nFenceFd);
                native->nFenceFd=-1;
                if(result){fail(AVERROR(EIO),"native output lock");return false;}
                if(planes.planeCount!=3 || planes.planes[0].pixelStride!=1 ||
                    planes.planes[0].rowStride<(unsigned)frame->width ||
                    !planes.planes[0].data || !planes.planes[1].data || !planes.planes[2].data ||
                    !planes.planes[1].pixelStride || !planes.planes[2].pixelStride ||
                    planes.planes[1].rowStride<(frame->width/2-1)*planes.planes[1].pixelStride+1 ||
                    planes.planes[2].rowStride<(frame->width/2-1)*planes.planes[2].pixelStride+1) {
                    AHardwareBuffer_unlock(hardware,&native->nFenceFd);
                    fail(AVERROR(EINVAL),"native YUV planes");return false;
                }
            }
            locked=clockNs();
            if(native_enabled)color.convert(frame,nullptr,0,nullptr,&planes);
            else color.convert(frame,destination,stride,matrix);
            converted=clockNs();
            if(hardware && AHardwareBuffer_unlock(hardware,&native->nFenceFd)) {
                fail(AVERROR(EIO),"native output unlock");return false;
            }
            int64_t after=clockNs();
            stats_lock+=locked-before;stats_convert+=converted-locked;stats_unlock+=after-converted;
            if(!stats_start)stats_start=before;
            stats_frames++;
            if(after-stats_start>=5000000000ll) {
                ALOGI("output %.1f fps, native=%d, lock %.2f ms, copy/convert %.2f ms, unlock %.2f ms, pts=%lld",
                    stats_frames*1e9/(after-stats_start),native_enabled,
                    stats_lock/(1e6*stats_frames),stats_convert/(1e6*stats_frames),stats_unlock/(1e6*stats_frames),
                    (long long)frame->pts);
                stats_start=after;stats_frames=0;stats_lock=stats_convert=stats_unlock=0;
            }
        } else {
            uint8_t *y=header->pBuffer,*u=y+stride*height,*v=u+stride*height/4;
            for(int row=0;row<frame->height;row++)
                memcpy(y+row*stride,frame->data[0]+row*frame->linesize[0],frame->width);
            for(int row=0;row<frame->height/2;row++) {
                const uint8_t *src=frame->data[1]+row*frame->linesize[1];
                uint8_t *du=u+row*(stride/2),*dv=v+row*(stride/2);int x=0;
                for(;x+16<=frame->width/2;x+=16) {
                    uint8x16x2_t pair=vld2q_u8(src+2*x);vst1q_u8(du+x,pair.val[0]);vst1q_u8(dv+x,pair.val[1]);
                }
                for(;x<frame->width/2;x++){du[x]=src[2*x];dv[x]=src[2*x+1];}
            }
        }
        header->nOffset=0;header->nFilledLen=(OMX_U32)bytes;
        header->nTimeStamp=frame->pts==AV_NOPTS_VALUE?frame->best_effort_timestamp:frame->pts;
        header->nFlags=OMX_BUFFERFLAG_ENDOFFRAME;
        queue.erase(queue.begin());info->mOwnedByUs=false;notifyFillBufferDone(header);
        av_frame_unref(frame);held=false;return true;
    }
protected:
    void rgbaPort() {
        if(native_enabled) {
            auto &def=editPortInfo(kOutputPortIndex)->mDef;
            def.format.video.eColorFormat=(OMX_COLOR_FORMATTYPE)AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
            def.nBufferSize=sizeof(VideoNativeMetadata);return;
        }
        if(mOutputFormat!=OMX_COLOR_Format32BitRGBA8888)return;
        auto &def=editPortInfo(kOutputPortIndex)->mDef;
        def.format.video.eColorFormat=OMX_COLOR_Format32BitRGBA8888;
        def.nBufferSize=outputBufferWidth()*outputBufferHeight()*4;
    }
    OMX_ERRORTYPE internalGetParameter(OMX_INDEXTYPE index, OMX_PTR params) override {
        if(index==(OMX_INDEXTYPE)OMX_IndexParamVideoAndroidRequiresSwRenderer) {
            auto *value=static_cast<OMX_PARAM_U32TYPE*>(params);
            if(!value || value->nSize<sizeof(*value) || value->nPortIndex!=kOutputPortIndex)
                return OMX_ErrorBadParameter;
            // Decoding happens on Iris; Android's Surface renderer consumes
            // our ordinary planar buffers rather than vendor ANW metadata.
            surface=!native_enabled;value->nU32=native_enabled?0:1;return OMX_ErrorNone;
        }
        if(index==usage_index) {
            auto *value=static_cast<GetAndroidNativeBufferUsageParams*>(params);
            if(!value || value->nSize<sizeof(*value) || value->nPortIndex!=kOutputPortIndex)return OMX_ErrorBadParameter;
            value->nUsage=AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN|AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
            return OMX_ErrorNone;
        }
        if(index==OMX_IndexParamPortDefinition)rgbaPort();
        OMX_ERRORTYPE result=SoftVideoDecoderOMXComponent::internalGetParameter(index,params);
        if(result==OMX_ErrorNone && index==OMX_IndexParamVideoPortFormat) {
            auto *value=static_cast<OMX_VIDEO_PARAM_PORTFORMATTYPE*>(params);
            if(value->nPortIndex==kOutputPortIndex)value->eColorFormat=native_enabled?
                (OMX_COLOR_FORMATTYPE)AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420:mOutputFormat;
        }
        return result;
    }
    OMX_ERRORTYPE getExtensionIndex(const char *name,OMX_INDEXTYPE *index) override {
        if(!strcmp(name,"OMX.google.android.index.enableAndroidNativeBuffers"))*index=native_index;
        else if(!strcmp(name,"OMX.google.android.index.storeANWBufferInMetadata"))*index=metadata_index;
        else if(!strcmp(name,"OMX.google.android.index.getAndroidNativeBufferUsage"))*index=usage_index;
        else return SoftVideoDecoderOMXComponent::getExtensionIndex(name,index);
        return OMX_ErrorNone;
    }
    OMX_ERRORTYPE internalSetParameter(OMX_INDEXTYPE index,OMX_PTR params) override {
        if(index==native_index || index==metadata_index) {
            auto *value=static_cast<EnableAndroidNativeBuffersParams*>(params);
            if(!value || value->nSize<sizeof(*value) || value->nPortIndex!=kOutputPortIndex)return OMX_ErrorBadParameter;
            if(index==native_index)native_enabled=value->enable;
            else metadata=value->enable;
            rgbaPort();return OMX_ErrorNone;
        }
        if(index==OMX_IndexParamVideoPortFormat && native_enabled) {
            auto *value=static_cast<OMX_VIDEO_PARAM_PORTFORMATTYPE*>(params);
            if(value && value->nSize>=sizeof(*value) && value->nPortIndex==kOutputPortIndex) {
                auto copy=*value;copy.eColorFormat=OMX_COLOR_FormatYUV420Planar;
                return SoftVideoDecoderOMXComponent::internalSetParameter(index,&copy);
            }
        }
        OMX_ERRORTYPE result=SoftVideoDecoderOMXComponent::internalSetParameter(index,params);
        rgbaPort();return result;
    }
    int getColorAspectPreference() override {return kPreferContainer;}
    void onQueueFilled(OMX_U32) override {
        if(failed || mOutputPortSettingsChange!=NONE || drained)return;
        auto &inputs=getPortQueue(kInputPortIndex);auto &outputs=getPortQueue(kOutputPortIndex);
        while(!outputs.empty()) {
            if(held){if(!outputFrame())return;continue;}
            if(codec) {
                int result=avcodec_receive_frame(codec,frame);
                if(result==0){held=true;continue;}
                if(result==AVERROR_EOF) {
                    BufferInfo *info=*outputs.begin();auto *header=info->mHeader;
                    header->nOffset=0;header->nFilledLen=0;header->nTimeStamp=eos_pts;
                    header->nFlags=OMX_BUFFERFLAG_EOS;outputs.erase(outputs.begin());
                    info->mOwnedByUs=false;notifyFillBufferDone(header);drained=true;return;
                }
                if(result!=AVERROR(EAGAIN)){fail(result,"hardware frame retrieval");return;}
            }
            if(inputs.empty())return;
            BufferInfo *info=*inputs.begin();auto *header=info->mHeader;
            if(header->nOffset>header->nAllocLen || header->nFilledLen>header->nAllocLen-header->nOffset) {
                fail(AVERROR(EINVAL),"input buffer bounds");return;
            }
            if(header->nFlags&OMX_BUFFERFLAG_CODECCONFIG) {
                if(replace_config){av_freep(&config);config_size=0;replace_config=false;}
                if(codec || config_size+header->nFilledLen>1024*1024) {
                    fail(AVERROR(EINVAL),"late or oversized codec configuration");return;
                }
                if(header->nFilledLen) {
                    void *grown=av_realloc(config,config_size+header->nFilledLen);
                    if(!grown){fail(AVERROR(ENOMEM),"codec configuration storage");return;}
                    config=(uint8_t*)grown;
                    memcpy(config+config_size,header->pBuffer+header->nOffset,header->nFilledLen);
                    config_size+=header->nFilledLen;
                }
                consumeInput(info);continue;
            }
            if(!openDecoder())return;
            if(header->nFilledLen) {
                AVPacket *packet=av_packet_alloc();
                if(!packet){fail(AVERROR(ENOMEM),"input packet");return;}
                int result=av_new_packet(packet,(int)header->nFilledLen);
                if(result>=0) {
                    memcpy(packet->data,header->pBuffer+header->nOffset,header->nFilledLen);
                    packet->pts=packet->dts=header->nTimeStamp;
                    if(header->nFlags&OMX_BUFFERFLAG_SYNCFRAME)packet->flags|=AV_PKT_FLAG_KEY;
                    result=avcodec_send_packet(codec,packet);
                }
                av_packet_free(&packet);
                if(result==AVERROR(EAGAIN))return;
                if(result<0){fail(result,"hardware packet submission");return;}
                header->nFilledLen=0;
            }
            if(header->nFlags&OMX_BUFFERFLAG_EOS) {
                eos_pts=header->nTimeStamp;
                int result=avcodec_send_packet(codec,nullptr);
                if(result==AVERROR(EAGAIN))return;
                if(result<0){fail(result,"hardware stream drain");return;}
            }
            consumeInput(info);
        }
    }
    void onPortFlushCompleted(OMX_U32 port) override {
        if(port==kInputPortIndex) {
            // A fresh hardware session avoids relying on partially drained
            // firmware state when ExoPlayer seeks or starts another clip.
            avcodec_free_context(&codec);av_frame_unref(frame);
            held=false;drained=false;failed=false;replace_config=true;
        }
    }
    void onReset() override {
        avcodec_free_context(&codec);av_frame_unref(frame);av_freep(&config);config_size=0;
        held=false;drained=false;failed=false;replace_config=false;
        SoftVideoDecoderOMXComponent::onReset();
        surface=false;native_enabled=false;metadata=false;mOutputFormat=OMX_COLOR_FormatYUV420Planar;
        updatePortDefinitions(true,false);
    }
    ~FramePortHEVC() override {av_frame_free(&frame);avcodec_free_context(&codec);av_freep(&config);}
public:
    FramePortHEVC(const char *name,const OMX_CALLBACKTYPE *callbacks,OMX_PTR appData,OMX_COMPONENTTYPE **component)
        :SoftVideoDecoderOMXComponent(name,"video_decoder.hevc",OMX_VIDEO_CodingHEVC,
            profiles,1,320,240,callbacks,appData,component) {
        initPorts(8,2*1024*1024,4,"video/hevc",1);
    }
};
class FramePortOMXPlugin final : public OMXPluginBase {
    bool available;
    static bool probeCapacity() {
        // Some SteamOS Iris builds count the new session's geometry for every
        // open session. A concurrent Steam UI decoder can therefore reject 8K.
        // Retain the stock HEVC codec if the driver's admission check fails.
        int fd=open("/dev/video-dec0",O_RDWR|O_NONBLOCK);
        if(fd<0)return false;
        v4l2_format format={};format.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        format.fmt.pix_mp.width=8192;format.fmt.pix_mp.height=4096;
        format.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_HEVC;format.fmt.pix_mp.num_planes=1;
        bool ok=ioctl(fd,VIDIOC_S_FMT,&format)==0;
        format.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;format.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_NV12;
        if(ok)ok=ioctl(fd,VIDIOC_S_FMT,&format)==0;
        v4l2_requestbuffers buffers={};buffers.count=2;
        buffers.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;buffers.memory=V4L2_MEMORY_MMAP;
        if(ok)ok=ioctl(fd,VIDIOC_REQBUFS,&buffers)==0;
        close(fd);return ok;
    }
public:
    FramePortOMXPlugin():available(probeCapacity()) {
        if(!available)ALOGW("Iris 8K admission unavailable; keeping Android's stock HEVC decoder");
    }
    OMX_ERRORTYPE makeComponentInstance(const char *name,const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,OMX_COMPONENTTYPE **component) override {
        if(!available || strcmp(name,"OMX.frameport.hevc.decoder"))return OMX_ErrorInvalidComponentName;
        auto *decoder=new FramePortHEVC(name,callbacks,appData,component);
        decoder->incStrong(this);return decoder->initCheck();
    }
    OMX_ERRORTYPE destroyComponentInstance(OMX_COMPONENTTYPE *component) override {
        auto *decoder=static_cast<SoftOMXComponent*>(component->pComponentPrivate);
        decoder->prepareForDestruction();decoder->decStrong(this);return OMX_ErrorNone;
    }
    OMX_ERRORTYPE enumerateComponents(OMX_STRING name,size_t size,OMX_U32 index) override {
        if(index || !available)return OMX_ErrorNoMore;
        if(size<sizeof("OMX.frameport.hevc.decoder"))return OMX_ErrorBadParameter;
        strcpy(name,"OMX.frameport.hevc.decoder");return OMX_ErrorNone;
    }
    OMX_ERRORTYPE getRolesOfComponent(const char *name,Vector<String8> *roles) override {
        if(!available || strcmp(name,"OMX.frameport.hevc.decoder"))return OMX_ErrorInvalidComponentName;
        roles->clear();roles->push(String8("video_decoder.hevc"));return OMX_ErrorNone;
    }
};
}
android::OMXPluginBase *createOMXPlugin(){return new android::FramePortOMXPlugin;}
// Android loaders support both historical factory ABI spellings.
extern "C" android::OMXPluginBase *createFramePortOMXPlugin() __asm__("createOMXPlugin");
extern "C" android::OMXPluginBase *createFramePortOMXPlugin(){return createOMXPlugin();}
extern "C" void destroyOMXPlugin(android::OMXPluginBase *plugin){delete plugin;}
