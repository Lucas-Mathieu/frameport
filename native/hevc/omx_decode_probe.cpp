// Exercises the actual Android OMX component, including port renegotiation,
// output conversion, timestamps and normal state/buffer lifetimes.
#include <media/hardware/OMXPluginBase.h>
#include <media/openmax/OMX_Component.h>
#include <media/openmax/OMX_IndexExt.h>
#include <utils/String8.h>
#include <pthread.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdlib>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <libavutil/time.h>
}
struct Test {
    pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
    OMX_COMPONENTTYPE *component=nullptr;
    OMX_BUFFERHEADERTYPE *input_free[64],*output_free[64];
    unsigned in_count=0,out_count=0,frames=0;
    bool resize=false,eos=false,error=false;
    unsigned state=OMX_StateLoaded,port_event=0;
    OMX_COMMANDTYPE port_command=OMX_CommandStateSet;
    int64_t last_pts=-1;
};
static OMX_ERRORTYPE event(OMX_HANDLETYPE,OMX_PTR data,OMX_EVENTTYPE type,OMX_U32 a,OMX_U32 b,OMX_PTR) {
    auto &t=*(Test*)data;pthread_mutex_lock(&t.lock);
    if(type==OMX_EventError){fprintf(stderr,"OMX error: %x %x\n",a,b);t.error=true;}
    if(type==OMX_EventPortSettingsChanged && a==1)t.resize=true;
    if(type==OMX_EventCmdComplete) {
        if(a==OMX_CommandStateSet)t.state=b;
        else {t.port_event=b==OMX_ALL?2:b+1;t.port_command=(OMX_COMMANDTYPE)a;}
    }
    pthread_cond_broadcast(&t.changed);pthread_mutex_unlock(&t.lock);return OMX_ErrorNone;
}
static OMX_ERRORTYPE empty(OMX_HANDLETYPE,OMX_PTR data,OMX_BUFFERHEADERTYPE *header) {
    auto &t=*(Test*)data;pthread_mutex_lock(&t.lock);assert(t.in_count<64);
    t.input_free[t.in_count++]=header;pthread_cond_broadcast(&t.changed);pthread_mutex_unlock(&t.lock);return OMX_ErrorNone;
}
static OMX_ERRORTYPE fill(OMX_HANDLETYPE,OMX_PTR data,OMX_BUFFERHEADERTYPE *header) {
    auto &t=*(Test*)data;pthread_mutex_lock(&t.lock);assert(t.out_count<64);
    if(header->nFilledLen) {
        if(header->nTimeStamp<t.last_pts){fprintf(stderr,"PTS went backwards\n");t.error=true;}
        t.last_pts=header->nTimeStamp;t.frames++;
    }
    if(header->nFlags&OMX_BUFFERFLAG_EOS)t.eos=true;
    t.output_free[t.out_count++]=header;
    pthread_cond_broadcast(&t.changed);pthread_mutex_unlock(&t.lock);return OMX_ErrorNone;
}
template<class T>static T parameter(unsigned port) {T p={};p.nSize=sizeof(p);p.nVersion.s.nVersionMajor=1;p.nPortIndex=port;return p;}
static void wait_state(Test &t,unsigned state) {
    pthread_mutex_lock(&t.lock);while(!t.error && t.state!=state)pthread_cond_wait(&t.changed,&t.lock);
    assert(!t.error);pthread_mutex_unlock(&t.lock);
}
static void wait_port(Test &t,OMX_COMMANDTYPE command) {
    pthread_mutex_lock(&t.lock);while(!t.error && !(t.port_event==2 && t.port_command==command))pthread_cond_wait(&t.changed,&t.lock);
    assert(!t.error);t.port_event=0;pthread_mutex_unlock(&t.lock);
}
static void allocate(Test &t,unsigned port,OMX_BUFFERHEADERTYPE **headers,unsigned &count) {
    auto def=parameter<OMX_PARAM_PORTDEFINITIONTYPE>(port);
    assert(t.component->GetParameter(t.component,OMX_IndexParamPortDefinition,&def)==OMX_ErrorNone);
    count=def.nBufferCountActual;assert(count<=16);
    printf("Allocate port %u: %ux%u, %u buffers x %u bytes\n",port,def.format.video.nFrameWidth,def.format.video.nFrameHeight,count,def.nBufferSize);fflush(stdout);
    for(unsigned i=0;i<count;i++)assert(t.component->AllocateBuffer(t.component,&headers[i],port,nullptr,def.nBufferSize)==OMX_ErrorNone);
}
static void queue_output(Test &t) {
    for(;;) {
        pthread_mutex_lock(&t.lock);
        if(!t.out_count || t.resize || t.eos){pthread_mutex_unlock(&t.lock);return;}
        auto *header=t.output_free[--t.out_count];pthread_mutex_unlock(&t.lock);
        header->nFilledLen=0;header->nFlags=0;
        assert(t.component->FillThisBuffer(t.component,header)==OMX_ErrorNone);
    }
}
int main(int argc,char **argv) {
    assert(argc==2 || argc==3);void *lib=dlopen("/probe/libstagefrighthw.so",RTLD_NOW);
    if(!lib){fprintf(stderr,"%s\n",dlerror());return 1;}
    auto factory=(android::OMXPluginBase*(*)())dlsym(lib,"createOMXPlugin");assert(factory);
    auto *plugin=factory();Test t;OMX_CALLBACKTYPE callbacks={event,empty,fill};
    assert(plugin->makeComponentInstance("OMX.frameport.hevc.decoder",&callbacks,&t,&t.component)==OMX_ErrorNone);
    if(argc==3) {
        auto mode=parameter<OMX_PARAM_U32TYPE>(1);
        assert(t.component->GetParameter(t.component,(OMX_INDEXTYPE)OMX_IndexParamVideoAndroidRequiresSwRenderer,&mode)==OMX_ErrorNone);
        assert(mode.nU32==1);
    }
    AVFormatContext *format=nullptr;assert(avformat_open_input(&format,argv[1],nullptr,nullptr)>=0);
    unsigned video=0;while(video<format->nb_streams && format->streams[video]->codecpar->codec_type!=AVMEDIA_TYPE_VIDEO)video++;
    assert(video<format->nb_streams);auto *stream=format->streams[video];
    AVBSFContext *bsf=nullptr;assert(av_bsf_alloc(av_bsf_get_by_name("hevc_mp4toannexb"),&bsf)>=0);
    assert(avcodec_parameters_copy(bsf->par_in,stream->codecpar)>=0);bsf->time_base_in=stream->time_base;assert(av_bsf_init(bsf)>=0);
    auto input_def=parameter<OMX_PARAM_PORTDEFINITIONTYPE>(0);
    assert(t.component->GetParameter(t.component,OMX_IndexParamPortDefinition,&input_def)==OMX_ErrorNone);
    input_def.format.video.nFrameWidth=stream->codecpar->width;input_def.format.video.nFrameHeight=stream->codecpar->height;
    assert(t.component->SetParameter(t.component,OMX_IndexParamPortDefinition,&input_def)==OMX_ErrorNone);
    OMX_BUFFERHEADERTYPE *inputs[16],*outputs[16];unsigned input_count,output_count;
    assert(t.component->SendCommand(t.component,OMX_CommandStateSet,OMX_StateIdle,nullptr)==OMX_ErrorNone);
    allocate(t,0,inputs,input_count);allocate(t,1,outputs,output_count);wait_state(t,OMX_StateIdle);
    for(unsigned i=0;i<input_count;i++)t.input_free[t.in_count++]=inputs[i];
    for(unsigned i=0;i<output_count;i++)t.output_free[t.out_count++]=outputs[i];
    assert(t.component->SendCommand(t.component,OMX_CommandStateSet,OMX_StateExecuting,nullptr)==OMX_ErrorNone);wait_state(t,OMX_StateExecuting);
    AVPacket *packet=av_packet_alloc();
    for(unsigned phase=0;phase<2;phase++) {
    bool sent_config=false,sent_eos=false;unsigned submitted=0;
    unsigned goal=phase?120:600;
    int64_t start=av_gettime_relative();
    for(;;) {
        queue_output(t);
        pthread_mutex_lock(&t.lock);
        while(!t.error && !t.resize && !t.eos && (sent_eos || !t.in_count) && !t.out_count)pthread_cond_wait(&t.changed,&t.lock);
        assert(!t.error);
        bool resized=t.resize;t.resize=false;bool finished=t.eos;
        pthread_mutex_unlock(&t.lock);
        if(finished)break;
        if(resized) {
            assert(t.component->SendCommand(t.component,OMX_CommandPortDisable,1,nullptr)==OMX_ErrorNone);
            pthread_mutex_lock(&t.lock);while(t.out_count<output_count && !t.error)pthread_cond_wait(&t.changed,&t.lock);assert(!t.error);t.out_count=0;pthread_mutex_unlock(&t.lock);
            for(unsigned i=0;i<output_count;i++)assert(t.component->FreeBuffer(t.component,1,outputs[i])==OMX_ErrorNone);
            wait_port(t,OMX_CommandPortDisable);
            assert(t.component->SendCommand(t.component,OMX_CommandPortEnable,1,nullptr)==OMX_ErrorNone);
            allocate(t,1,outputs,output_count);wait_port(t,OMX_CommandPortEnable);
            pthread_mutex_lock(&t.lock);for(unsigned i=0;i<output_count;i++)t.output_free[t.out_count++]=outputs[i];pthread_mutex_unlock(&t.lock);
            continue;
        }
        if(sent_eos)continue;
        pthread_mutex_lock(&t.lock);
        auto *header=t.in_count?t.input_free[--t.in_count]:nullptr;pthread_mutex_unlock(&t.lock);
        if(!header)continue;
        header->nOffset=0;header->nFilledLen=0;header->nTimeStamp=0;header->nFlags=0;
        if(!sent_config) {
            assert((unsigned)bsf->par_out->extradata_size<=header->nAllocLen);
            memcpy(header->pBuffer,bsf->par_out->extradata,bsf->par_out->extradata_size);
            header->nFilledLen=bsf->par_out->extradata_size;header->nFlags=OMX_BUFFERFLAG_CODECCONFIG;sent_config=true;
        } else if(submitted>=goal) {
            header->nFlags=OMX_BUFFERFLAG_EOS;header->nTimeStamp=goal*1000000/60;sent_eos=true;
        } else {
            int result;
            do{result=av_read_frame(format,packet);assert(result>=0);if(packet->stream_index!=(int)video)av_packet_unref(packet);}while(packet->stream_index!=(int)video);
            assert(av_bsf_send_packet(bsf,packet)>=0);assert(av_bsf_receive_packet(bsf,packet)>=0);
            av_packet_rescale_ts(packet,stream->time_base,AVRational{1,1000000});
            assert((unsigned)packet->size<=header->nAllocLen);memcpy(header->pBuffer,packet->data,packet->size);header->nFilledLen=packet->size;
            header->nTimeStamp=packet->pts;header->nFlags=OMX_BUFFERFLAG_ENDOFFRAME;
            if(packet->flags&AV_PKT_FLAG_KEY)header->nFlags|=OMX_BUFFERFLAG_SYNCFRAME;
            av_packet_unref(packet);submitted++;
        }
        assert(t.component->EmptyThisBuffer(t.component,header)==OMX_ErrorNone);
    }
    double seconds=(av_gettime_relative()-start)/1000000.0;
    printf("OMX hardware + full-size planar delivery: %u frames in %.3fs = %.1ffps\n",t.frames,seconds,t.frames/seconds);fflush(stdout);
    assert(t.frames==goal);
    if(!phase) {
        assert(t.component->SendCommand(t.component,OMX_CommandFlush,OMX_ALL,nullptr)==OMX_ErrorNone);
        wait_port(t,OMX_CommandFlush);
        assert(av_seek_frame(format,video,0,AVSEEK_FLAG_BACKWARD)>=0);av_bsf_flush(bsf);
        pthread_mutex_lock(&t.lock);t.eos=false;t.frames=0;t.last_pts=-1;pthread_mutex_unlock(&t.lock);
        puts("Replaying after a complete OMX flush and seek to the beginning.");
    }
    }
    assert(t.component->SendCommand(t.component,OMX_CommandStateSet,OMX_StateIdle,nullptr)==OMX_ErrorNone);wait_state(t,OMX_StateIdle);
    assert(t.component->SendCommand(t.component,OMX_CommandStateSet,OMX_StateLoaded,nullptr)==OMX_ErrorNone);
    for(unsigned i=0;i<input_count;i++)assert(t.component->FreeBuffer(t.component,0,inputs[i])==OMX_ErrorNone);
    for(unsigned i=0;i<output_count;i++)assert(t.component->FreeBuffer(t.component,1,outputs[i])==OMX_ErrorNone);
    wait_state(t,OMX_StateLoaded);assert(plugin->destroyComponentInstance(t.component)==OMX_ErrorNone);
    delete plugin;dlclose(lib);av_packet_free(&packet);av_bsf_free(&bsf);avformat_close_input(&format);
    puts("PASS: hardware OMX decode, planar frames, ordered timestamps, EOS and shutdown.");
}
