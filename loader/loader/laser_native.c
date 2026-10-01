/* Target libyoyo SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701.
 * ARM softfp; install stopped after relocation/imports. No fast-math. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#ifdef LASER_TEST
#include "collision_stub.h"
#else
#include <kubridge.h>
#include "main.h"
#include "so_util.h"
#endif
#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags, kind; } RV;
typedef RV *ABI (*script_fn)(void *,void *,RV *,int,RV **);
typedef RV *ABI (*ref_fn)(void *,int);
typedef RV *ABI (*choose_fn)(RV *,int,RV **);
typedef double ABI (*random_fn)(double);
typedef int ABI (*compare_fn)(const RV *,const RV *,double,bool);
typedef float ABI (*direction_fn)(float,float,float,float);
typedef float ABI (*length_fn)(float,float);
typedef void ABI (*draw_fn)(void *,int,float,float,float,float,float,float,int,float);
typedef void ABI (*builtin_fn)(RV *,void *,void *,int,RV *);
typedef void ABI (*push_fn)(void *);
typedef void ABI (*pop_fn)(int);
typedef RV *ABI (*array_fn)(RV *,int);
typedef void ABI (*free_fn)(RV *);
typedef RV *ABI (*increment_fn)(RV *,int);
typedef struct { const char *name; builtin_fn call; int argc; } Function;
typedef struct Trace { struct Trace *previous; const char *name; int line; } Trace;
enum { RANDARRAY,I,X,Y,YSCALE,NVARS };
static const char *const var_names[NVARS]={"_randvar_array","i","x","y","image_yscale"};
static const uint32_t *vars[NVARS];
enum { COLOUR,ALPHA,DISTANCE,MEETING,NFUNCS };
static const char *const fn_names[NFUNCS]={"draw_get_colour","draw_get_alpha","point_distance","place_meeting"};
static const char *const fn_symbols[NFUNCS]={
 "_Z14F_DrawGetColorR6RValueP9CInstanceS2_iPS_",
 "_Z14F_DrawGetAlphaR6RValueP9CInstanceS2_iPS_",
 "_Z15F_PointDistanceR6RValueP9CInstanceS2_iPS_",
 "_Z14F_PlaceMeetingR6RValueP9CInstanceS2_iPS_"};
static const uint32_t *descriptors[NFUNCS];
static builtin_fn builtins[NFUNCS];
static Function **functions, **current_function;
static Trace **trace_start;
static const uint8_t *profile;
static const double *epsilon;
static const void *builtin_variables;
static const int *builtin_count;
static uintptr_t xy_get[2];
static ref_fn standard_get,standard_ref;
static choose_fn choose,minimum;
static random_fn random_value;
static compare_fn compare;
static direction_fn direction;
static length_fn length_x,length_y;
static draw_fn draw_sprite;
static push_fn push_context;
static pop_fn pop_context;
static array_fn array_lval;
static free_fn release;
static increment_fn increment;
static script_fn original_draw;
static int installed;
unsigned laser_native_hits,laser_native_fallbacks,laser_native_segments;
unsigned laser_native_probe_hits,laser_native_probe_fallbacks,laser_native_queries;
unsigned laser_native_reject_line[64],laser_native_reject_count[64],laser_native_reject_overflow;
/* The assembly bridge reads these via their addresses. */
uintptr_t laser_probe_original,laser_probe_resume;
static uint32_t word(const void *p,size_t o) { uint32_t v; memcpy(&v,(const uint8_t *)p+o,4); return v; }
static float scalar(const void *p,size_t o) { float v; memcpy(&v,(const uint8_t *)p+o,4); return v; }
static int reject(unsigned line) {
 for(int i=0;i<64;++i) if(!laser_native_reject_line[i]||laser_native_reject_line[i]==line) {
  laser_native_reject_line[i]=line; ++laser_native_reject_count[i]; return 0;
 }
 ++laser_native_reject_overflow; return 0;
}
#define REQUIRE(c) do { if(!(c)) return reject(__LINE__); } while(0)
static RV real(double v) { RV r={v,0,0}; return r; }
static RV asset(int id,uint32_t type) { RV r={0,0,15}; uint32_t b[2]={(uint32_t)id,type}; memcpy(&r.value,b,8); return r; }
static int numeric(const RV *v) { return v&&v->kind==0&&isfinite(v->value)&&fabs(v->value)<1048576; }
typedef struct { const uint8_t *entries; uint32_t mask; } Table;
static int table(void *owner,Table *t) {
 REQUIRE(owner);
 const void *vt=(const void *)(uintptr_t)word(owner,0);
 REQUIRE(vt&&word(vt,8)==(uintptr_t)standard_get&&word(vt,12)==(uintptr_t)standard_ref);
 const void *ht=(const void *)(uintptr_t)word(owner,0x24); REQUIRE(ht);
 t->mask=word(ht,8); t->entries=(const uint8_t *)(uintptr_t)word(ht,16);
 REQUIRE(t->entries&&t->mask<=65535&&!(t->mask&(t->mask+1))); return 1;
}
static RV *peek(const Table *t,int n) {
 uint32_t key=(vars[n][1]+1)&0x7fffffff,i=key&t->mask;
 if(!key) return NULL;
 for(uint32_t d=0;d<=t->mask;++d,i=(i+1)&t->mask) {
  const uint8_t *e=t->entries+12*i; uint32_t k=word(e,8);
  if(k==key) return (RV *)(uintptr_t)word(e,0);
  if(!k||d>((i-(k&t->mask))&t->mask)) return NULL;
 }
 return NULL;
}
static int registry(int first,int end) {
 REQUIRE(*functions);
 for(int i=first;i<end;++i) REQUIRE(descriptors[i][1]<16384&&(*functions)[descriptors[i][1]].call==builtins[i]);
 return 1;
}
static void builtin(int n,RV *out,void *self,void *other,int argc,RV *args) {
 Function *saved=*current_function; uint32_t z=0;
 memcpy(&out->value,&z,4); out->flags=0; out->kind=5;
 *current_function=*functions+descriptors[n][1]; builtins[n](out,self,other,argc,args); *current_function=saved;
}
static RV *get(void *self,int n,int writable) { return (writable?standard_ref:standard_get)(self,(int)vars[n][1]); }
static int draw_admit(void *self,RV *result,int argc,RV **args) {
 Table t;
 REQUIRE(result&&argc>=3&&args&&!*profile&&isfinite(*epsilon)&&*epsilon>=0&&table(self,&t)&&registry(COLOUR,MEETING));
 RV *a=peek(&t,RANDARRAY); REQUIRE(numeric(a)&&a!=result);
 for(int i=0;i<3;++i) REQUIRE(numeric(args[i])&&args[i]!=result&&args[i]!=a);
 REQUIRE(args[2]->value>=0&&args[2]->value<=8192);
 return 1;
}
RV *ABI laser_native_draw(void *self,void *other,RV *result,int argc,RV **args) {
 if(!draw_admit(self,result,argc,args)) { ++laser_native_fallbacks; return original_draw(self,other,result,argc,args); }
 ++laser_native_hits;
 Trace trace={*trace_start,"gml_Script_draw_laser",9}; *trace_start=&trace;
 uint32_t z=0; memcpy(&result->value,&z,4); result->kind=5;
 RV yes=real(1),no=real(0),toggle=real(0); yes.kind=no.kind=13;
 RV *choices[2]={&yes,&no}; choose(&toggle,2,choices);
 double x=args[0]->value,y=args[1]->value,height=args[2]->value;
 RV colour=real(0),alpha=real(0);
 trace.line=16; builtin(COLOUR,&colour,self,other,0,NULL);
 trace.line=17; builtin(ALPHA,&alpha,self,other,0,NULL);
 trace.line=19;
 for(int i=0;i<7;++i) {
  trace.line=23; RV *array=get(self,RANDARRAY,1); double v=random_value(1);
  push_context(self); RV *element=array_lval(array,i);
  push_context((void *)(uintptr_t)word((void *)(uintptr_t)word(array,0),0));
  if(0x46u&(1u<<(element->kind&31))) release(element);
  element->value=v; element->kind=0; pop_context(2); trace.line=24;
 }
 double travelled=0,a=0,b=0; int index=0;
 RV limit=real(height*32),at=real(0);
 trace.line=28;
 for(;;) {
  at.value=travelled;
  if(compare(&at,&limit,*epsilon,true)>=0) break;
  trace.line=30; double spacing=(b*6+5)*1;
  trace.line=31;
  if(toggle.value!=0) {
   trace.line=33; double x1=(x+a)-b;
   trace.line=34; double y1=y+travelled;
   trace.line=35; double x2=(x+b)-a;
   RV end=real(travelled+spacing); RV *mins[2]={&end,&limit}; RV minout=real(0);
   trace.line=36; double y2=y+minimum(&minout,2,mins)->value;
   trace.line=38; double dir=direction((float)x1,(float)y1,(float)x2,(float)y2);
   trace.line=39;
   double px=x1+(double)length_x(2,(float)(dir+90));
   double py=y1+(double)length_y(2,(float)(dir+90));
   RV endpoints[4]={real(x1),real(y1),real(x2),real(y2)},distance=real(0);
   builtin(DISTANCE,&distance,self,other,4,endpoints);
   double opacity=alpha.value*(0.6+a*0.4);
   draw_sprite(self,2184,0,(float)px,(float)py,(float)distance.value,4,(float)dir,(int)colour.value,(float)opacity);
   ++laser_native_segments;
  }
  trace.line=42; toggle.value=toggle.value!=0?0:1;
  trace.line=toggle.value!=0?49:45;
  travelled+=toggle.value!=0?spacing:spacing*1.5;
  trace.line=52;
  RV *array=get(self,RANDARRAY,0);
  const RV *items=(const RV *)(uintptr_t)word((void *)(uintptr_t)word(array,0),4);
  a=items[index%7].value;
  trace.line=53; array=get(self,RANDARRAY,0);
  items=(const RV *)(uintptr_t)word((void *)(uintptr_t)word(array,0),4);
  b=items[(index+1)%7].value; trace.line=54; index+=2;
 }
 trace.line=57; RV *array=get(self,RANDARRAY,1);
 if(0x46u&(1u<<(array->kind&31))) release(array);
 array->value=0; array->kind=0;
 *trace_start=trace.previous; return result;
}
static int probe_admit(void *self,uintptr_t fp,uintptr_t sp) {
 Table t;
 REQUIRE(!*profile&&table(self,&t)&&registry(MEETING,NFUNCS));
 REQUIRE(numeric(peek(&t,I))&&peek(&t,I)->value==0);
 for(int i=0;i<2;++i) {
  REQUIRE(*builtin_count>0&&vars[X+i][1]<(uint32_t)*builtin_count&&word(builtin_variables,16*vars[X+i][1]+4)==xy_get[i]);
  REQUIRE(isfinite(scalar(self,0xb8+4*i)));
 }
 /* FREE_RValue__Pre ba7758..ba778c returns immediately for kind 6.
  * Strings and arrays can own references; delegate if any are live. */
 static const int stack_offsets[]={0x40,0x50,0x60,0x70,0xf0,0x100};
 static const int frame_offsets[]={0xe8,0xd8,0xc8,0xb8,0xa8,0x98,0x88,0x78};
 for(unsigned i=0;i<sizeof(stack_offsets)/sizeof(*stack_offsets);++i)
  REQUIRE(!(0x6u&(1u<<(word((void *)sp,stack_offsets[i]+12)&31))));
 for(unsigned i=0;i<sizeof(frame_offsets)/sizeof(*frame_offsets);++i)
  REQUIRE(!(0x6u&(1u<<(word((void *)(fp-frame_offsets[i]),12)&31))));
 return 1;
}
/* Returns the original ARM continuation to the register-preserving bridge. */
uintptr_t ABI laser_native_probe(uintptr_t fp,uintptr_t sp) {
 void *self=(void *)(uintptr_t)word((void *)(fp-0xec),0);
 void *other=(void *)(uintptr_t)word((void *)(fp-0xf0),0);
 if(!probe_admit(self,fp,sp)) { ++laser_native_probe_fallbacks; return laser_probe_original; }
 ++laser_native_probe_hits;
 for(;;) {
  RV *i=get(self,I,0);
  RV query[3]={real((double)scalar(self,0xb8)),real((double)scalar(self,0xbc)+i->value*32),asset(512,0x01000000)};
  RV out=real(0); ++laser_native_queries; builtin(MEETING,&out,self,other,3,query);
  if(out.value!=0) break;
  i=get(self,I,0);
  query[0]=real((double)scalar(self,0xb8)); query[1]=real((double)scalar(self,0xbc)+i->value*32);
  query[2]=asset(576,0x01000000);
  ++laser_native_queries; builtin(MEETING,&out,self,other,3,query);
  if(out.value!=0) break;
  *(int *)(fp-0xf4)=166; increment(get(self,I,1),1);
 }
 return laser_probe_resume;
}
/* Preserve r0-r12, lr, all VFP/NEON registers and FPSCR. The extra saved
 * word receives the continuation. Restoring pc from it preserves lr too. */
__attribute__((naked)) void laser_native_probe_bridge(void) {
 __asm__ volatile(
  ".fpu neon\n"
  "sub sp,sp,#8\n" "push {r0-r12,lr}\n"
  "vpush {d0-d15}\n" "vpush {d16-d31}\n"
  "vmrs r0,fpscr\n" "push {r0,r1}\n"
  "mov r0,fp\n" "add r1,sp,#328\n" "bl laser_native_probe\n"
  "str r0,[sp,#320]\n" "pop {r0,r1}\n" "vmsr fpscr,r0\n"
  "vpop {d16-d31}\n" "vpop {d0-d15}\n" "pop {r0-r12,lr}\n"
  "ldr pc,[sp],#8\n");
}
int laser_native_install(void *(*rx_alloc)(size_t)) {
 if(installed||!rx_alloc) return 0;
#define RESOLVE(v,t,n) do { v=(t)so_symbol(&yoyoloader_mod,n); if(!v) return 0; } while(0)
 uintptr_t draw,step;
 RESOLVE(draw,uintptr_t,"_Z21gml_Script_draw_laserP9CInstanceS0_R8YYRValueiPPS1_");
 RESOLVE(step,uintptr_t,"_Z35gml_Object_obj_ceiling_laser_Step_0P9CInstanceS0_");
 if((draw&3)||(step&3)||draw<0x1919c70||step-0x3699b90!=draw-0x1919c70) return 0;
 uintptr_t probe=step+0x559c;
 if(word((void *)draw,0)!=0xe92d4ff0||word((void *)draw,4)!=0xe28db01c||
    word((void *)probe,0)!=0xe59d007c||word((void *)probe,4)!=0xe200001f) return 0;
 RESOLVE(profile,const uint8_t *,"g_bProfile"); RESOLVE(epsilon,const double *,"g_GMLMathEpsilon");
 RESOLVE(functions,Function **,"the_functions"); RESOLVE(current_function,Function **,"g_pFunction");
 RESOLVE(trace_start,Trace **,"_ZN13SYYStackTrace8s_pStartE");
 RESOLVE(standard_get,ref_fn,"_ZN12YYObjectBase19InternalGetYYVarRefEi");
 RESOLVE(standard_ref,ref_fn,"_ZN12YYObjectBase20InternalGetYYVarRefLEi");
 RESOLVE(choose,choose_fn,"_Z12YYGML_chooseR8YYRValueiPPS_");
 RESOLVE(minimum,choose_fn,"_Z9YYGML_minR8YYRValueiPPS_");
 RESOLVE(random_value,random_fn,"_Z12YYGML_randomd");
 RESOLVE(compare,compare_fn,"_Z12YYCompareValRK6RValueS1_db");
 RESOLVE(direction,direction_fn,"_Z21YYGML_point_directionffff");
 RESOLVE(length_x,length_fn,"_Z17YYGML_lengthdir_xff"); RESOLVE(length_y,length_fn,"_Z17YYGML_lengthdir_yff");
 RESOLVE(draw_sprite,draw_fn,"_Z21YYGML_draw_sprite_extP9CInstanceiffffffif");
 RESOLVE(push_context,push_fn,"_Z16PushContextStackP12YYObjectBase"); RESOLVE(pop_context,pop_fn,"_Z15PopContextStacki");
 RESOLVE(array_lval,array_fn,"_Z17ARRAY_LVAL_RValueP8YYRValuei"); RESOLVE(release,free_fn,"_Z16FREE_RValue__PreP6RValue");
 RESOLVE(increment,increment_fn,"_ZN8YYRValuepLEi");
 RESOLVE(builtin_variables,const void *,"builtin_variables");
 RESOLVE(builtin_count,const int *,"g_nStartGlobalVariables");
 RESOLVE(xy_get[0],uintptr_t,"_Z4GV_XP9CInstanceiP6RValue"); RESOLVE(xy_get[1],uintptr_t,"_Z4GV_YP9CInstanceiP6RValue");
 char name[64];
 for(int i=0;i<NVARS;++i) { strcpy(name,"g_VAR_"); strcat(name,var_names[i]); RESOLVE(vars[i],const uint32_t *,name); }
 for(int i=0;i<NFUNCS;++i) { strcpy(name,"g_FUNC_"); strcat(name,fn_names[i]); RESOLVE(descriptors[i],const uint32_t *,name); RESOLVE(builtins[i],builtin_fn,fn_symbols[i]); }
#undef RESOLVE
 void *rx=rx_alloc(32); uintptr_t bias=draw-0x1919c70;
 if(bias>UINTPTR_MAX-0x04e70000||!rx||((uintptr_t)rx&3)||(uintptr_t)rx>UINTPTR_MAX-32||
    ((uintptr_t)rx<bias+0x04e70000&&(uintptr_t)rx+32>bias)) return 0;
 uint32_t code[8]={0xe92d4ff0,0xe28db01c,0xe51ff004,(uint32_t)(draw+8),
                    0xe59d007c,0xe200001f,0xe51ff004,(uint32_t)(probe+8)};
 kuKernelCpuUnrestrictedMemcpy(rx,code,sizeof(code)); kuKernelFlushCaches(rx,sizeof(code));
 original_draw=(script_fn)rx; laser_probe_original=(uintptr_t)rx+16; laser_probe_resume=step+0x5bf0;
 hook_addr(draw,(uintptr_t)laser_native_draw); hook_addr(probe,(uintptr_t)laser_native_probe_bridge);
 kuKernelFlushCaches((void *)draw,8); kuKernelFlushCaches((void *)probe,8); installed=1; return 1;
}
