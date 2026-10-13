/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "upstream_safe.h"
#include <ctype.h>
#include <string.h>
static int hex(char c) {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
int ps4mount_url_decode(const char *src,size_t len,char *out,size_t cap){
    size_t n=0;
    if(!cap)return -1;
    for(size_t i=0;i<len;i++){
        unsigned char c=(unsigned char)src[i];
        if(c=='%'){
            if(i+2>=len)return -1;
            int h=hex(src[i+1]),l=hex(src[i+2]);
            if(h<0||l<0)return -1;
            c=(unsigned char)(h*16+l);i+=2;
        }else if(c=='+')c=' ';
        if(!c||c==127|| (c<32&&c!='\n'&&c!='\r'))return -1;
        if(n+1>=cap)return -1;
        out[n++]=(char)c;
    }
    out[n]=0;return 0;
}
int ps4mount_safe_path(const char *in,char *out,size_t cap){
    if(!in||!out||!cap)return -1;
    while(*in==' '||*in=='/')in++;
    size_t len=strlen(in);
    while(len&&(in[len-1]=='/'||in[len-1]==' '||in[len-1]=='\r'||in[len-1]=='\t'))len--;
    if(len>=120||len>=cap)return -1;
    for(size_t i=0;i<len;i++) {
        unsigned char c=(unsigned char)in[i];
        if(c<32||c==127||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|')return -1;
        if(c=='/'&&(i==0||in[i-1]=='/'))return -1;
    }
    memcpy(out,in,len);out[len]=0;
    for(size_t i=0;i<=len;){
        size_t j=i;
        while(j<len&&out[j]!='/')j++;
        if((j-i==1&&out[i]=='.')||(j-i==2&&out[i]=='.'&&out[i+1]=='.'))return -1;
        i=j+1;
    }
    /* canonical folders are searched automatically and never excluded. */
    if(len>=3&&tolower((unsigned char)out[0])=='p'&&tolower((unsigned char)out[1])=='s'&&
       out[2]=='4'&&(len==3||out[3]=='/'))return -1;
    return 0;
}
