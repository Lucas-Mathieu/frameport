// SPDX-License-Identifier: GPL-3.0-only
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
/* Diagnostic only: dim location-0 vec4 RGB where FragSizeEXT != (1,1).
 * Unsupported interfaces pass through; alpha and all other outputs are retained. */
static uint32_t *fdm_darken(const uint32_t *w,size_t n,size_t *out_n) {
 if(n<5||n>4000000||w[0]!=0x07230203||!w[3]||w[3]>1000000)return NULL;
 uint32_t bound=w[3],output=0,ptr=0,vec=0,flt=0; int entries=0,stores=0;
 size_t cap_at=0,ext_at=0,ann_at=0,fn_at=0;
 for(size_t i=5;i<n;) {uint32_t len=w[i]>>16,op=w[i]&65535;if(!len||i+len>n)return NULL;
  if(op==17)cap_at=i+len;
  if(op==10)ext_at=i+len;
  if(op==15){if(w[i+1]!=4)return NULL;entries++;}
  if(op==71||op==72)ann_at=i+len;
  if(op==71&&len==4&&w[i+2]==30&&w[i+3]==0){
   /* Location zero can also decorate an input; choose only the output variable. */
   for(size_t j=5;j<n;){uint32_t l=w[j]>>16;if(!l||j+l>n)return NULL;
    if((w[j]&65535)==59&&l>=4&&w[j+2]==w[i+1]&&w[j+3]==3)output=w[i+1];
    j+=l;
   }
  }
  if(op==54&&!fn_at)fn_at=i;
  i+=len;
 }
 if(entries!=1||!output||!cap_at||!ann_at||!fn_at)return NULL;
 for(size_t i=5;i<n;i+=w[i]>>16)if((w[i]&65535)==59&&w[i+2]==output&&w[i+3]==3)ptr=w[i+1];
 for(size_t i=5;i<n;i+=w[i]>>16)if((w[i]&65535)==32&&w[i+1]==ptr&&w[i+2]==3)vec=w[i+3];
 for(size_t i=5;i<n;i+=w[i]>>16)if((w[i]&65535)==23&&w[i+1]==vec&&w[i+3]==4)flt=w[i+2];
 int is_float=0;
 for(size_t i=5;i<n;i+=w[i]>>16){if((w[i]&65535)==22&&w[i+1]==flt&&w[i+2]==32)is_float=1;
  if((w[i]&65535)==62&&w[i+1]==output)stores++;
  if((w[i]&65535)==71&&(w[i]>>16)>=4&&w[i+2]==11&&w[i+3]==5292)return NULL;
 }
 if(!is_float||!stores)return NULL;
 /* Reuse scalar/vector/pointer types: Vulkan validation prohibits duplicates. */
 uint32_t ui=0,bv=0,v2=0,ip=0;
 for(size_t i=5;i<n;i+=w[i]>>16){uint32_t op=w[i]&65535;
  if(op==21&&w[i+2]==32&&w[i+3]==0)ui=w[i+1];
  if(op==20)bv=w[i+1];
 }
 for(size_t i=5;i<n;i+=w[i]>>16)if((w[i]&65535)==23&&w[i+2]==ui&&w[i+3]==2)v2=w[i+1];
 for(size_t i=5;i<n;i+=w[i]>>16)if((w[i]&65535)==32&&w[i+2]==1&&w[i+3]==v2)ip=w[i+1];
 int nui=!ui,nbv=!bv,nv2=!v2,nip=!ip;
 if(nui)ui=bound++;if(nbv)bv=bound++;if(nv2)v2=bound++;if(nip)ip=bound++;
 uint32_t frag=bound++,one=bound++,bright=bound++,dark=bound++;
 uint32_t *out=calloc(n+200+stores*64,sizeof(uint32_t));if(!out)return NULL;size_t k=0;
#define EMIT(...) do{const uint32_t a[]={__VA_ARGS__};memcpy(out+k,a,sizeof a);k+=sizeof a/4;}while(0)
 memcpy(out,w,20);k=5;if(!ext_at)ext_at=cap_at;
 for(size_t i=5;i<=n;) {
  if(i==cap_at)EMIT((2u<<16)|17,5291);
  if(i==ext_at){const char s[]="SPV_EXT_fragment_invocation_density";size_t z=(sizeof s+3)/4;out[k++]=((uint32_t)(1+z)<<16)|10;memcpy(out+k,s,sizeof s);k+=z;}
  if(i==ann_at){EMIT((4u<<16)|71,frag,11,5292);EMIT((3u<<16)|71,frag,14);}
  if(i==fn_at){
   if(nui)EMIT((4u<<16)|21,ui,32,0);if(nbv)EMIT((2u<<16)|20,bv);
   if(nv2)EMIT((4u<<16)|23,v2,ui,2);if(nip)EMIT((4u<<16)|32,ip,1,v2);
   EMIT((4u<<16)|43,ui,one,1);EMIT((4u<<16)|43,flt,bright,0x3f800000);
   EMIT((4u<<16)|43,flt,dark,0x3e4ccccd); /* 0.2 RGB, alpha unchanged */
   EMIT((4u<<16)|59,ip,frag,1);
  }
  if(i==n)break;uint32_t len=w[i]>>16,op=w[i]&65535;
  if(op==15){out[k++]=((len+1)<<16)|15;memcpy(out+k,w+i+1,(len-1)*4);k+=len-1;out[k++]=frag;}
  else if(op==62&&w[i+1]==output){
   uint32_t sz=bound++,x=bound++,y=bound++,area=bound++,low=bound++,factor=bound++,factors=bound++,color=bound++;
   EMIT((4u<<16)|61,v2,sz,frag);EMIT((5u<<16)|81,ui,x,sz,0);EMIT((5u<<16)|81,ui,y,sz,1);
   EMIT((5u<<16)|132,ui,area,x,y);EMIT((5u<<16)|171,bv,low,area,one);
   EMIT((6u<<16)|169,flt,factor,low,dark,bright);
   EMIT((7u<<16)|80,vec,factors,factor,factor,factor,bright);
   EMIT((5u<<16)|133,vec,color,w[i+2],factors);
   memcpy(out+k,w+i,len*4);out[k+2]=color;k+=len;
  }else{memcpy(out+k,w+i,len*4);k+=len;}
  i+=len;
 }
 out[3]=bound;*out_n=k;
#undef EMIT
 return out;
}
