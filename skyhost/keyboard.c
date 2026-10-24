#include "keyboard.h"
#include "skyhost.h"

/* Verified in Skyrim's original SDK/main, not the D2R keyboard adapter:
 * sdk+3d9930 copies these ShowKeyboardArg fields; sdk+3d95f4 reads String's
 * pointer and byte capacity. main+adfb34 requests UTF-8 and distinguishes
 * success 0 from cancellation 0x29f at main+adfb38. */
enum {
    ARG_HEADER=0x24,ARG_SUBTEXT=0xa6,ARG_GUIDE=0x1a8,ARG_MAX=0x3ac,ARG_MIN=0x3b0,
    ARG_UTF8=0x3bd,ARG_INITIAL_OFFSET=0x3c0,ARG_INITIAL_LENGTH=0x3c4,
    ARG_WORK=0x4d0,ARG_WORK_SIZE=0x4d8,STRING_BYTES=2004,INITIAL_UNITS=500,
    RESULT_CANCELLED=0x29f,
};
static pthread_mutex_t g_presenter_lock=PTHREAD_MUTEX_INITIALIZER;
static sky_keyboard_presenter_t g_presenter;

void sky_keyboard_set_presenter(sky_keyboard_presenter_t presenter) {
    pthread_mutex_lock(&g_presenter_lock);g_presenter=presenter;pthread_mutex_unlock(&g_presenter_lock);
}

static uint16_t read_u16(const uint8_t *p) {uint16_t v;memcpy(&v,p,2);return v;}
static int append_utf8(uint32_t c,char *out,size_t capacity,size_t *used) {
    unsigned n=c<0x80?1:c<0x800?2:c<0x10000?3:4;
    if(n>=capacity-*used)return 0;
    if(n==1)out[(*used)++]=(char)c;
    else {
        out[(*used)++]=(char)((n==2?0xc0:n==3?0xe0:0xf0)|(c>>(6*(n-1))));
        for(unsigned i=n-1;i-->0;)out[(*used)++]=(char)(0x80|((c>>(6*i))&63));
    }
    return 1;
}
static int from_utf16(const uint8_t *text,size_t units,char *out,size_t capacity) {
    size_t used=0;
    for(size_t i=0;i<units;i++) {
        uint32_t c=read_u16(text+2*i);if(!c)break;
        if(c>=0xd800&&c<=0xdbff) {
            if(i+1>=units)return 0;
            uint32_t low=read_u16(text+2*(i+1));if(low<0xdc00||low>0xdfff)return 0;
            c=0x10000+((c-0xd800)<<10)+(low-0xdc00);i++;
        } else if(c>=0xdc00&&c<=0xdfff)return 0;
        if(!append_utf8(c,out,capacity,&used))return 0;
    }
    out[used]=0;return 1;
}
static int next_utf8(const uint8_t *text,size_t length,size_t *offset,uint32_t *value) {
    uint32_t first=text[(*offset)++],c;unsigned extra;
    if(first<0x80){*value=first;return 1;}
    if(first>=0xc2&&first<=0xdf){c=first&31;extra=1;}
    else if(first>=0xe0&&first<=0xef){c=first&15;extra=2;}
    else if(first>=0xf0&&first<=0xf4){c=first&7;extra=3;}
    else return 0;
    if(extra>length-*offset)return 0;
    for(unsigned i=0;i<extra;i++) {
        uint8_t b=text[(*offset)++];if((b&0xc0)!=0x80)return 0;c=(c<<6)|(b&63);
    }
    if((extra==1&&c<0x80)||(extra==2&&c<0x800)||(extra==3&&c<0x10000)||
        c>0x10ffff||(c>=0xd800&&c<=0xdfff))return 0;
    *value=c;return 1;
}

uint32_t sky_keyboard_show(void *out_string,const void *show_arg) {
    if(!out_string||!show_arg)return RESULT_CANCELLED;
    const uint8_t *arg=show_arg;void *output;uint64_t output_size,work_size;
    const uint8_t *work;int32_t maximum,minimum,initial_offset,initial_length;
    memcpy(&output,out_string,8);memcpy(&output_size,(uint8_t *)out_string+8,8);
    memcpy(&maximum,arg+ARG_MAX,4);memcpy(&minimum,arg+ARG_MIN,4);
    memcpy(&initial_offset,arg+ARG_INITIAL_OFFSET,4);memcpy(&initial_length,arg+ARG_INITIAL_LENGTH,4);
    memcpy(&work,arg+ARG_WORK,8);memcpy(&work_size,arg+ARG_WORK_SIZE,8);
    if(maximum<0||minimum<0||output_size>STRING_BYTES||initial_length<0||initial_length>INITIAL_UNITS)
        return RESULT_CANCELLED;
    int utf8=arg[ARG_UTF8]!=0;
    size_t capacity=output&&output_size?(size_t)output_size:STRING_BYTES;
    if(!utf8&&capacity<2)return RESULT_CANCELLED;
    size_t max_units=utf8?capacity-1:capacity/2-1;
    if(maximum>0&&(size_t)maximum<max_units)max_units=(size_t)maximum;
    if((size_t)minimum>max_units)return RESULT_CANCELLED;
    char header[65*3+1],subtext[129*3+1],guide[257*3+1],initial[INITIAL_UNITS*3+1]="";
    if(!from_utf16(arg+ARG_HEADER,65,header,sizeof header)||
        !from_utf16(arg+ARG_SUBTEXT,129,subtext,sizeof subtext)||
        !from_utf16(arg+ARG_GUIDE,257,guide,sizeof guide))return RESULT_CANCELLED;
    if(initial_length) {
        uint64_t bytes=(uint64_t)initial_length*2;
        if(!work||initial_offset<0||(uint64_t)initial_offset>work_size||
            bytes>work_size-(uint64_t)initial_offset||(uintptr_t)work>UINTPTR_MAX-(uint32_t)initial_offset-bytes||
            !from_utf16(work+initial_offset,(size_t)initial_length,initial,sizeof initial))return RESULT_CANCELLED;
    }
    sky_keyboard_request_t request={header,subtext,guide,initial,(size_t)minimum,max_units};
    pthread_mutex_lock(&g_presenter_lock);sky_keyboard_presenter_t presenter=g_presenter;pthread_mutex_unlock(&g_presenter_lock);
    sky_checkpoint("post_nnMain_keyboard_requested");
    LOG("KEYBOARD_REQUESTED min=%zu max=%zu encoding=%s presenter=%d",request.min_length,request.max_length,utf8?"utf8":"utf16",presenter!=NULL);
    /* UTF-16 output may need three UTF-8 bytes per code unit in the frontend. */
    char text[STRING_BYTES*3+1]={0};size_t text_capacity=utf8?capacity:(capacity/2-1)*3+1;
    if(!presenter||presenter(&request,text,text_capacity)!=1)return RESULT_CANCELLED;
    const char *end=memchr(text,0,text_capacity);if(!end)return RESULT_CANCELLED;
    size_t length=(size_t)(end-text),offset=0,units=0;uint32_t scalar;
    while(offset<length) {
        if(!next_utf8((const uint8_t *)text,length,&offset,&scalar))return RESULT_CANCELLED;
        units+=scalar>=0x10000?2:1;
    }
    if(units<request.min_length||units>request.max_length||
        (utf8?length+1>capacity:(units+1)*2>capacity))return RESULT_CANCELLED;
    if(output&&output_size) {
        memset(output,0,(size_t)output_size);
        if(utf8)memcpy(output,text,length);
        else {
            offset=0;size_t position=0;
            while(offset<length) {
                if(!next_utf8((const uint8_t *)text,length,&offset,&scalar))return RESULT_CANCELLED;
                uint16_t value;
                if(scalar>=0x10000) {
                    scalar-=0x10000;value=(uint16_t)(0xd800|(scalar>>10));memcpy((uint8_t *)output+position,&value,2);position+=2;
                    value=(uint16_t)(0xdc00|(scalar&1023));
                } else value=(uint16_t)scalar;
                memcpy((uint8_t *)output+position,&value,2);position+=2;
            }
        }
    }
    sky_checkpoint("post_nnMain_keyboard_accepted");
    LOG("KEYBOARD_ACCEPTED units=%zu encoding=%s",units,utf8?"utf8":"utf16");
    return 0;
}
