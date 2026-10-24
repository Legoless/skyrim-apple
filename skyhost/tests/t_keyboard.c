#include "keyboard.h"
#include "skyhost.h"
#include <assert.h>
#include <mach-o/getsect.h>

extern const struct mach_header_64 _mh_execute_header;
typedef uint32_t (*show_fn)(void *,const void *);
static show_fn forced_entry(void) {
    typedef struct {const char *name,*why;void *function;} entry_t;
    unsigned long size=0;const entry_t *entries=(const entry_t *)getsectiondata(&_mh_execute_header,"__DATA","swforce",&size);
    assert(entries&&size%sizeof *entries==0);
    for(size_t i=0;i<size/sizeof *entries;i++)if(!strcmp(entries[i].name,
        "_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE"))return (show_fn)entries[i].function;
    assert(!"missing Skyrim keyboard ABI override");return NULL;
}
static void put32(uint8_t *p,int32_t v) {memcpy(p,&v,4);}
static void put64(uint8_t *p,uint64_t v) {memcpy(p,&v,8);}
static void put16(uint8_t *p,const uint16_t *text,size_t units) {memcpy(p,text,units*2);}
static uint8_t argument[0x500],work[4096];
static const char *answer="Lydia";static int accept=1,calls,unterminated,unregister_inside;
static size_t expected_min=1,expected_max=12;
static int presenter(const sky_keyboard_request_t *request,char *out,size_t capacity) {
    calls++;assert(request->min_length==expected_min&&request->max_length==expected_max);
    assert(!strcmp(request->header,"Name")&&!strcmp(request->subtext,"Skyrim")&&!strcmp(request->guide,"Choose"));
    assert(!strcmp(request->initial_text,"Æ漢😀"));
    if(unregister_inside)sky_keyboard_set_presenter(NULL);
    if(unterminated){memset(out,'a',capacity);return 1;}
    assert(strlen(answer)+1<=capacity);memcpy(out,answer,strlen(answer)+1);return accept;
}
static void configure(void) {
    memset(argument,0,sizeof argument);memset(work,0,sizeof work);
    const uint16_t header[]={'N','a','m','e',0},sub[]={'S','k','y','r','i','m',0},guide[]={'C','h','o','o','s','e',0};
    const uint16_t initial[]={0xc6,0x6f22,0xd83d,0xde00,0};
    put16(argument+0x24,header,sizeof header/2);put16(argument+0xa6,sub,sizeof sub/2);put16(argument+0x1a8,guide,sizeof guide/2);
    put32(argument+0x3ac,12);put32(argument+0x3b0,1);argument[0x3bd]=1;
    put32(argument+0x3c0,0x14);put32(argument+0x3c4,4);put16(work+0x14,initial,sizeof initial/2);
    put64(argument+0x4d0,(uintptr_t)work);put64(argument+0x4d8,sizeof work);
    answer="Lydia";accept=1;unterminated=unregister_inside=0;expected_min=1;expected_max=12;sky_keyboard_set_presenter(presenter);
}
typedef struct {uint64_t before;void *buffer;uint64_t capacity,after;} guest_string_t;
static uint8_t output[96];
static guest_string_t string;
static void reset_output(size_t capacity) {
    memset(output,0xa5,sizeof output);string=(guest_string_t){UINT64_C(0x1122334455667788),output+8,capacity,UINT64_C(0x8877665544332211)};
}
static void check_guards(void) {
    assert(string.before==UINT64_C(0x1122334455667788)&&string.after==UINT64_C(0x8877665544332211));
    assert(string.buffer==output+8);
    for(unsigned i=0;i<8;i++)assert(output[i]==0xa5);
    for(size_t i=8+string.capacity;i<sizeof output;i++)assert(output[i]==0xa5);
}
static void expect_cancel(show_fn show) {
    uint8_t before[sizeof output];memcpy(before,output,sizeof output);guest_string_t original=string;
    assert(show(&string.buffer,argument)==0x29f);assert(!memcmp(before,output,sizeof output)&&!memcmp(&string,&original,sizeof string));
}
int main(void) {
    show_fn show=forced_entry();configure();reset_output(64);uint8_t original_arg[sizeof argument],original_work[sizeof work];
    memcpy(original_arg,argument,sizeof argument);memcpy(original_work,work,sizeof work);
    answer="Łydia 😀";assert(show(&string.buffer,argument)==0);assert(!strcmp((char *)string.buffer,answer));check_guards();
    assert(!memcmp(original_arg,argument,sizeof argument)&&!memcmp(original_work,work,sizeof work));
    assert(!strcmp(sky_checkpoint_name(),"post_nnMain_keyboard_accepted"));
    argument[0x3bd]=0;reset_output(64);assert(show(&string.buffer,argument)==0);
    const uint16_t expected[]={0x141,'y','d','i','a',' ',0xd83d,0xde00,0};assert(!memcmp(string.buffer,expected,sizeof expected));check_guards();
    configure();reset_output(64);accept=0;expect_cancel(show);sky_keyboard_set_presenter(NULL);expect_cancel(show);
    configure();unregister_inside=1;assert(show(&string.buffer,argument)==0);expect_cancel(show);
    /* SDK ReadCloseResultAndString skips the copy if either storage field is
     * zero; acceptance still succeeds and the caller's other bytes survive. */
    configure();reset_output(64);string.buffer=NULL;assert(!show(&string.buffer,argument));
    for(unsigned i=0;i<sizeof output;i++)assert(output[i]==0xa5);
    reset_output(0);assert(!show(&string.buffer,argument));check_guards();
    reset_output(64);string.capacity=2005;expect_cancel(show);reset_output(64);
    const char *invalid[]={"\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\xe2\x82"};
    for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++){configure();reset_output(64);answer=invalid[i];expect_cancel(show);}
    configure();reset_output(4);expected_max=3;unterminated=1;expect_cancel(show);
    configure();reset_output(64);answer="";expect_cancel(show);answer="ThirteenChars!";expect_cancel(show);
    configure();reset_output(64);answer="7 空-_";assert(!show(&string.buffer,argument));check_guards();
    configure();put32(argument+0x3ac,0);reset_output(8);expected_max=7;answer="1234567";assert(!show(&string.buffer,argument));check_guards();
    configure();argument[0x3bd]=0;reset_output(5);expected_max=1;answer="Æ";assert(!show(&string.buffer,argument));check_guards();
    reset_output(7);put32(argument+0x3ac,1);answer="😀";expect_cancel(show);
    configure();reset_output(1);put32(argument+0x3b0,0);expected_min=expected_max=0;answer="";assert(!show(&string.buffer,argument));check_guards();
    configure();reset_output(64);int count=calls;put32(argument+0x3c0,sizeof work-1);expect_cancel(show);assert(calls==count);
    configure();put32(argument+0x3c4,501);expect_cancel(show);configure();put64(argument+0x4d0,UINTPTR_MAX-1);expect_cancel(show);
    configure();put16(work+0x14,(uint16_t[]){0xd800},1);put32(argument+0x3c4,1);expect_cancel(show);
    configure();put32(argument+0x3ac,1);put32(argument+0x3b0,2);expect_cancel(show);
    configure();put32(argument+0x3ac,-1);expect_cancel(show);configure();put32(argument+0x3b0,-1);expect_cancel(show);
    configure();assert(show(NULL,argument)==0x29f&&show(&string.buffer,NULL)==0x29f);
    sky_keyboard_set_presenter(NULL);
    puts("T_KEYBOARD_OK forced-Skyrim-ABI-byte-capacity-UTF8-UTF16-surrogates-cancel-bounds-and-reentrant-registration");return 0;
}
