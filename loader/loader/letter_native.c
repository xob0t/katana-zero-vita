/* libyoyo.so SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701.
 * Install stopped after relocation/import resolution. ARMv7 softfp, no fast-math. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#ifdef LETTER_TEST
#include "collision_stub.h"
#else
#include <kubridge.h>
#include "main.h"
#include "so_util.h"
#endif
#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags,kind; } RV;
typedef RV *ABI (*script_fn)(void *,void *,RV *,int,RV **);
typedef void ABI (*event_fn)(void *,void *);
typedef RV *ABI (*ref_fn)(void *,int);
typedef void ABI (*builtin_fn)(RV *,void *,void *,int,RV *);
typedef RV *ABI (*op_fn)(RV *,const RV *);
typedef RV *ABI (*choose_fn)(RV *,int,RV **);
typedef int ABI (*compare_fn)(const RV *,const RV *,double,bool);
typedef double ABI (*random_fn)(double);
typedef void ABI (*int_fn)(int);
typedef int ABI (*convert_int_fn)(const RV *,int);
typedef void ABI (*alpha_fn)(float);
typedef void *ABI (*find_fn)(int);
typedef void ABI (*set_fn)(void *,int,int,RV *);
typedef void ABI (*user_fn)(void *,void *,int);
typedef void ABI (*destroy_fn)(void *,void *,int,RV **);
typedef void ABI (*free_fn)(RV *);
typedef void ABI (*copy_fn)(RV *,const RV *);
typedef void ABI (*push_fn)(void *);
typedef void ABI (*pop_fn)(int);
typedef void ABI (*getter_fn)(void *,int,RV *);
typedef struct { const char *name; builtin_fn call; int argc; } Function;
typedef struct Trace { struct Trace *previous; const char *name; int line; } Trace;
enum { FR,TEXT_SPEED,DIE,TYPE,RAINBOW,SET_OSC,OSC_DELAY,RAINBOW_VAL,OSC_Y,VSP,ENTRANCE,ALPHA,H_OFFSET,V_OFFSET,
 FX_TIMEOUT,FX_ON,FX_SCALE,SPD_FR,SPD,FONT,PARENT,AX,AY,GARBLE,GARBLE_INIT,LETTER,GARBLE_LETTER,GARBLE_TIMER,COL,
 SCALE,ANGLE,DIR,GLOW,STATE,GOTO_ID,HAS_CHOICE,TXT_GLOW_ALPHA,FONT_SCALE,FONT_YOFFSET,
 FONT_CYRILLIC,FONT_CASINO,FONT_VHS,FONT_BIG,FONT_TEXTBOX,FONT_VCR,FONT_VCR_SMALL,FONT_XIROD,FONT_CHINESE,
 GAME_LANGUAGE,X,Y,SPEED,VSPEED,DIRECTION,ROOM_HEIGHT,NVARS };
static const char *const var_names[NVARS]={"fr","text_speed_scaler","die","type","rainbow","set_oscillation",
 "oscillate_delay","rainbow_val","oscillation_y","vsp","entrance","alpha","h_offset","v_offset","fx_timeout",
 "fx_on","fx_scale","spd_fr","spd","font","parent","ax","ay","garble","garble_init","letter","garble_letter",
 "garble_timer","col","scale","angle","dir","glow","state","goto_id","has_choice","txt_glow_alpha",
 "font_scale","font_yoffset","font_cyrillic","font_casino_money","font_vhs","font_big","font_textbox","font_vcr",
 "font_vcr_small","font_xirod_rus","font_chinese","game_language","x","y","speed","vspeed","direction","room_height"};
enum { TEXT,EXISTS,FLOOR,ROUND,CHR,HSV,GET_ALPHA,NFUNCS };
static const char *const fn_names[NFUNCS]={"draw_text_transformed","instance_exists","floor","round","chr","make_colour_hsv","draw_get_alpha"};
static const char *const fn_symbols[NFUNCS]={"_Z21F_DrawTextTransformedR6RValueP9CInstanceS2_iPS_",
 "_Z16F_InstanceExistsR6RValueP9CInstanceS2_iPS_","_Z7F_FloorR6RValueP9CInstanceS2_iPS_","_Z7F_RoundR6RValueP9CInstanceS2_iPS_",
 "_Z5F_ChrR6RValueP9CInstanceS2_iPS_","_Z14F_MakeColorHSVR6RValueP9CInstanceS2_iPS_","_Z14F_DrawGetAlphaR6RValueP9CInstanceS2_iPS_"};
static const uint32_t *vars[NVARS],*descriptors[NFUNCS];
static builtin_fn builtins[NFUNCS];
static Function **functions,**current_function;
static void **global;
static const uint8_t *profile;
static const double *epsilon;
static Trace **trace_start;
static ref_fn standard_get,standard_ref;
static op_fn add,subtract,multiply;
static choose_fn choose;
static compare_fn compare;
static random_fn random_value;
static int_fn set_colour,set_font,set_halign,set_valign;
static convert_int_fn integer_value;
static alpha_fn set_alpha;
static set_fn set_direct;
static find_fn find_instance;
static user_fn event_user;
static destroy_fn destroy;
static free_fn release;
static copy_fn copy_owned;
static push_fn push_context;
static pop_fn pop_context;
static script_fn original_scaled,original_font,glow_script;
static event_fn original_draw,original_user;
static event_fn volatile original_step __attribute__((used));
static const void *builtin_variables;
static const int *builtin_count;
static uintptr_t getters[5],setters[5];
static getter_fn room_height;
static int installed;
unsigned letter_native_hits[5],letter_native_fallbacks[5],letter_native_draws,letter_native_rng_calls;
unsigned letter_native_reject_line[64],letter_native_reject_count[64],letter_native_reject_overflow;
enum { DRAW_HOOK,STEP_HOOK,USER_HOOK,SCALED_HOOK,FONT_HOOK };
static uint32_t word(const void *p,size_t o) { uint32_t v; memcpy(&v,(const uint8_t *)p+o,4); return v; }
static double coordinate(void *p,size_t o) { float v; memcpy(&v,(const uint8_t *)p+o,4); return (double)v; }
static RV real(double n) { RV v={n,0,0}; return v; }
static int reject(unsigned line) {
 for(int i=0;i<64;++i) if(!letter_native_reject_line[i]||letter_native_reject_line[i]==line) {
  letter_native_reject_line[i]=line; ++letter_native_reject_count[i]; return 0;
 }
 ++letter_native_reject_overflow; return 0;
}
#define REQUIRE(c) do { if(!(c)) return reject(__LINE__); } while(0)
typedef struct { const uint8_t *entries; uint32_t mask; } Table;
static int table(void *owner,Table *t) {
 REQUIRE(owner);
 const void *vt=(const void *)(uintptr_t)word(owner,0);
 REQUIRE(vt);
 REQUIRE(word(vt,8)==(uintptr_t)standard_get);
 REQUIRE(word(vt,12)==(uintptr_t)standard_ref);
 const void *ht=(const void *)(uintptr_t)word(owner,0x24); REQUIRE(ht);
 t->mask=word(ht,8); t->entries=(const uint8_t *)(uintptr_t)word(ht,16);
 REQUIRE(t->entries);
 REQUIRE(t->mask<=65535);
 REQUIRE(!(t->mask&(t->mask+1))); return 1;
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
/* The IEEE high-word test is exactly finite abs(value)<2^30, including zeros
 * and subnormals; it also excludes infinities and every NaN representation. */
static unsigned kind(const RV *v) { return v->kind&0x00ffffff; }
static int real_kind(const RV *v) { return kind(v)==0||kind(v)==13; }
static double number(const RV *v) {
 if(real_kind(v)) return v->value;
 if(kind(v)==10) { int64_t n; memcpy(&n,v,8); return (double)n; }
 return (double)(int32_t)word(v,0);
}
static int numeric(const RV *v) {
 if(!v) return 0;
 if(real_kind(v)) return (word(v,4)&0x7fffffff)<0x41d00000;
 if(kind(v)==7||kind(v)==10) return fabs(number(v))<1073741824.0;
 return 0;
}
static int font_number(const RV *v) {
 return numeric(v)||(v&&kind(v)==15&&word(v,4)==0x01000006&&word(v,0)<1048576);
}
static int truth(const RV *v) { return number(v)>0.5; }
static int string_value(const RV *v) { return v&&kind(v)==1&&word(v,0)&&word((void *)(uintptr_t)word(v,0),0); }
static int common(void) {
 REQUIRE(!*profile);
 REQUIRE(*functions);
 REQUIRE(isfinite(*epsilon));
 REQUIRE(*epsilon>=0);
 REQUIRE(*epsilon<=0.00001);
 return 1;
}
static int registry(unsigned mask) {
 /* Loader: instance_exists is wrapped in the registry by the loader's clothesline code; the wrapper only
  * differs during clothesline Draw, so it is equivalent here. */
 if(mask&(1u<<EXISTS)) {
  REQUIRE(descriptors[EXISTS][1]<16384);
  REQUIRE((*functions)[descriptors[EXISTS][1]].call);
 }
 mask&=~(1u<<EXISTS);
 for(int i=0;i<NFUNCS;++i) if(mask&(1u<<i)) {
  REQUIRE(descriptors[i][1]<16384);
  REQUIRE((*functions)[descriptors[i][1]].call==builtins[i]);
 }
 return 1;
}
static int builtins_ordinary(void) {
 REQUIRE(*builtin_count>0);
 REQUIRE(*builtin_count<=500);
 for(int i=0;i<5;++i) {
  REQUIRE(vars[X+i][1]<(uint32_t)*builtin_count);
  REQUIRE(word(builtin_variables,16*vars[X+i][1]+4)==getters[i]);
  REQUIRE(word(builtin_variables,16*vars[X+i][1]+8)==setters[i]);
 }
 return 1;
}
static void builtin(int n,RV *out,void *self,void *other,int argc,RV *args) {
 Function *saved=*current_function; uint32_t z=0;
 memcpy(&out->value,&z,4); out->flags=0; out->kind=5;
 *current_function=*functions+descriptors[n][1];
 /* EXISTS must honor the loader's registered wrapper, as legacy dispatch does. */
 builtin_fn call=n==EXISTS?(*current_function)->call:builtins[n];
 call(out,self,other,argc,args); *current_function=saved;
}
/* Exact finite REAL/BOOL path at e3ee1c/e3f31c: subtract, abs <= epsilon,
 * otherwise sign. Admission excludes NaN/infinity and mixed reference types. */
static int cmp(double a,double b) { double d=a-b; return fabs(d)<=*epsilon?0:(d<0?-1:1); }
static void scalar_write(RV *v,double n) { v->value=n; v->kind=0; }
static void bool_write(RV *v,int n) { *v=real(n); v->kind=13; }
static void plus(RV *v,double n) { RV b=real(n); add(v,&b); }
static void minus(RV *v,double n) { RV b=real(n); subtract(v,&b); }
static void owned_write(void *owner,RV *dst,const RV *src) {
 if(dst==src) return;
 RV saved=*src; push_context(owner);
 if(0x46u&(1u<<(dst->kind&31))) release(dst);
 *dst=saved;
 if(0x46u&(1u<<(saved.kind&31))) copy_owned(dst,&saved);
 pop_context(1);
}
static void set_builtin(void *self,int n,double x) { RV v=real(x); set_direct(self,(int)vars[n][1],(int)0x80000000,&v); }
static RV *slot(void *self,int n) { return standard_get(self,(int)vars[n][1]); }
static double value(void *self,int n) { return number(slot(self,n)); }
static double height(void *self) { RV v=real(0); room_height(self,0,&v); return v.value; }
static void scaled_values(void *self,void *other,const RV *x,const RV *y,const RV *text,
                          const RV *sx,const RV *sy,const RV *angle) {
 Trace tr={*trace_start,"gml_Script_draw_text_scaled_transformed",2}; *trace_start=&tr;
 const RV *fs=slot(*global,FONT_SCALE),*off=slot(*global,FONT_YOFFSET);
 RV args[6]={*x,*y,*text,*sx,*sy,*angle},out=real(0);
 if(real_kind(y)&&real_kind(sx)&&real_kind(sy)&&real_kind(fs)&&real_kind(off)) {
  args[1]=real(number(y)+(number(off)*number(fs))*number(sy));
  args[3]=real(number(sx)*number(fs)); args[4]=real(number(sy)*number(fs));
 } else {
  /* Integer RV operations retain integer result kinds and the runner's promotions. */
  RV offset=*off; multiply(&offset,fs); multiply(&offset,sy); add(args+1,&offset);
  multiply(args+3,fs); multiply(args+4,fs);
 }
 builtin(TEXT,&out,self,other,6,args); ++letter_native_draws;
 *trace_start=tr.previous;
}
static void scaled_call(void *self,void *other,double x,double y,const RV *text,const RV *scale,const RV *angle) {
 RV xx=real(x),yy=real(y); scaled_values(self,other,&xx,&yy,text,scale,scale,angle);
}
static int scaled_admit(void *self,RV *out,int argc,RV **args) {
 (void)self; Table g;
 if(!common()) return 0;
 if(!registry(1u<<TEXT)) return 0;
 if(!table(*global,&g)) return 0;
 REQUIRE(out);
 REQUIRE(argc>=6);
 REQUIRE(args);
 REQUIRE(numeric(peek(&g,FONT_SCALE)));
 REQUIRE(numeric(peek(&g,FONT_YOFFSET)));
 REQUIRE(out!=peek(&g,FONT_SCALE));
 REQUIRE(out!=peek(&g,FONT_YOFFSET));
 for(int i=0;i<6;++i) {
  REQUIRE(args[i]);
  REQUIRE(args[i]!=out);
  REQUIRE((i==2?string_value(args[i]):numeric(args[i])));
 }
 return 1;
}
RV *ABI letter_native_scaled(void *self,void *other,RV *out,int argc,RV **args) {
 if(!scaled_admit(self,out,argc,args)) { ++letter_native_fallbacks[SCALED_HOOK]; return original_scaled(self,other,out,argc,args); }
 ++letter_native_hits[SCALED_HOOK]; uint32_t z=0; memcpy(&out->value,&z,4); out->kind=5;
 scaled_values(self,other,args[0],args[1],args[2],args[3],args[4],args[5]); return out;
}
/* Language/font switch lifted from 01952860; preserve first-match case order. */
static int font_cmp(const RV *a,const RV *b) {
 if(kind(a)==15&&kind(b)==15) {
  /* Both admitted references are fonts. YYCompareVal orders their signed IDs. */
  int32_t x=(int32_t)word(a,0),y=(int32_t)word(b,0); return (x>y)-(x<y);
 }
 return real_kind(a)&&real_kind(b)?cmp(number(a),number(b)):compare(a,b,*epsilon,false);
}
static int font_fixed(const RV *a,int64_t n) {
 double id=number(a);
 /* Every fixed case is a small integer. For integral admitted IDs, all runner
  * numeric/reference paths have the same equality result; fractional IDs still
  * need the runner's INT64 conversion, including its rounding behavior. */
 if(id==(int32_t)id) return cmp(id,(double)n);
 RV b=real(0); memcpy(&b,&n,8); b.kind=10; return font_cmp(a,&b);
}
static void font_call(const RV *input_rv,const Table *g) { double input=number(input_rv),fs=1,off=0,out=input;
  int lang=(int)number(peek(g,GAME_LANGUAGE));
 if(font_cmp(input_rv,peek(g,FONT_CYRILLIC))!=0&&font_cmp(input_rv,peek(g,FONT_CASINO))!=0&&lang<=13) {
  if(lang==1||lang==2||lang==8||lang==9) {
   out=lang==1?3:lang==2?4:number(peek(g,FONT_CHINESE));
   if(font_cmp(input_rv,peek(g,FONT_TEXTBOX))==0) { off=lang==1?1:0; }
   else if(font_fixed(input_rv,10)==0) { fs=2; off=lang==1?0:-2; }
   else if(font_cmp(input_rv,peek(g,FONT_BIG))==0) { fs=4; off=lang==1?1:lang==2?-1:0; }
   else if(font_cmp(input_rv,peek(g,FONT_VHS))==0) { fs=lang==1?.8:.9; off=lang==1?3:1; }
   else if(font_fixed(input_rv,1)==0) { fs=lang>=8?1.3:1.2; off=lang>=8?-2:0; }
   else if(font_fixed(input_rv,2)==0) { off=0; }
   else if(font_fixed(input_rv,5)==0) { fs=2.5; off=lang==1?2:lang==2?-1:-2; }
   else if(font_fixed(input_rv,6)==0) { fs=.64; off=lang>=8?-2:0; }
   else if(font_fixed(input_rv,7)==0) { fs=2; off=lang==1?2:0; }
   else if(font_fixed(input_rv,8)==0) { fs=2.5; off=lang==1?2:lang==2?1:0; }
   else if(font_fixed(input_rv,9)==0) { fs=3.2; off=lang==1?2:lang==2?0:-1; }
   else if(lang>=8) off=-1;
  } else {
   if(lang==0&&font_fixed(input_rv,9)==0) off=-2;
   else if(lang!=0&&font_cmp(input_rv,peek(g,FONT_TEXTBOX))==0) { /* unchanged */ }
   else if(lang!=0&&font_fixed(input_rv,10)==0) { if(lang==7) { out=number(peek(g,FONT_XIROD)); off=-1; } else off=1; }
   else if(lang!=0&&font_cmp(input_rv,peek(g,FONT_BIG))==0) off=-3;
   else if(font_cmp(input_rv,peek(g,FONT_VHS))==0) off=2;
   else if(lang==0&&font_fixed(input_rv,10)==0) off=1;
   else if(font_cmp(input_rv,peek(g,FONT_BIG))==0) off=-3;
   else if(lang!=0&&font_fixed(input_rv,1)==0) { out=number(peek(g,FONT_TEXTBOX)); fs=lang==7?1.4:1.2; }
   else if(lang!=0&&font_fixed(input_rv,2)==0) { out=number(peek(g,FONT_TEXTBOX)); fs=lang==7?1:lang==11?.8:.9; }
   else if(lang!=0&&font_fixed(input_rv,6)==0) { out=number(peek(g,FONT_TEXTBOX)); fs=.64; }
   else if(lang==7&&font_fixed(input_rv,5)==0) { out=number(peek(g,FONT_VCR_SMALL)); fs=1.4; }
   else if(lang==7&&font_fixed(input_rv,7)==0) out=number(peek(g,FONT_VCR_SMALL));
   else if(lang!=0&&font_fixed(input_rv,8)==0) { out=number(peek(g,FONT_VCR_SMALL)); fs=lang==7?1.5:1; }
   else if(font_fixed(input_rv,9)==0) { if(lang!=0) out=number(peek(g,FONT_VCR)); off=-2; }
   else if(font_fixed(input_rv,10)==0) { if(lang==7) { out=number(peek(g,FONT_XIROD)); off=-1; } else off=1; }
  }
 }
 scalar_write(peek(g,FONT_SCALE),fs); scalar_write(peek(g,FONT_YOFFSET),off);
 RV selected=real(out); set_font(integer_value(&selected,0));
}
static int font_admit(RV *out,int argc,RV **args,Table *cached) {
 Table g; if(!common()) return 0;
 if(!table(*global,&g)) return 0;
 REQUIRE(out);
 REQUIRE(argc>=1);
 REQUIRE(args);
 REQUIRE(font_number(args[0]));
 REQUIRE(args[0]!=out);
 RV *fs=peek(&g,FONT_SCALE),*yo=peek(&g,FONT_YOFFSET),*language=peek(&g,GAME_LANGUAGE);
 REQUIRE(numeric(fs));
 REQUIRE(numeric(yo));
 REQUIRE(numeric(language));
 double lang=number(language); REQUIRE(lang>=0);
 REQUIRE(lang<=15);
 REQUIRE(lang==(int)lang);
 REQUIRE(fs!=yo);
 REQUIRE(out!=fs);
 REQUIRE(out!=yo);
 REQUIRE(args[0]!=fs);
 REQUIRE(args[0]!=yo);
 for(int i=FONT_CYRILLIC;i<=GAME_LANGUAGE;++i) {
  RV *v=i==GAME_LANGUAGE?language:peek(&g,i);
  REQUIRE(i==GAME_LANGUAGE||font_number(v));
  REQUIRE(v!=fs);
  REQUIRE(v!=yo);
  REQUIRE(v!=out);
 }
 *cached=g; return 1;
}
RV *ABI letter_native_font(void *self,void *other,RV *out,int argc,RV **args) {
 Table g; if(!font_admit(out,argc,args,&g)) { ++letter_native_fallbacks[FONT_HOOK]; return original_font(self,other,out,argc,args); }
 ++letter_native_hits[FONT_HOOK]; Trace tr={*trace_start,"gml_Script_draw_set_font_localized",453}; *trace_start=&tr;
 RV input=*args[0]; uint32_t z=0; memcpy(&out->value,&z,4); out->kind=5;
 font_call(&input,&g); *trace_start=tr.previous; return out;
}
typedef struct { RV *v[NVARS]; void *parent; } State;
static int field_admit(int n,const RV *v) {
 switch(n) {
 case DIE: REQUIRE(numeric(v)); break;
 case TYPE: REQUIRE(numeric(v)); break;
 case RAINBOW: REQUIRE(numeric(v)); break;
 case SET_OSC: REQUIRE(numeric(v)); break;
 case OSC_DELAY: REQUIRE(numeric(v)); break;
 case RAINBOW_VAL: REQUIRE(numeric(v)); break;
 case OSC_Y: REQUIRE(numeric(v)); break;
 case VSP: REQUIRE(numeric(v)); break;
 case ENTRANCE: REQUIRE(numeric(v)); break;
 case ALPHA: REQUIRE(numeric(v)); break;
 case H_OFFSET: REQUIRE(numeric(v)); break;
 case V_OFFSET: REQUIRE(numeric(v)); break;
 case FX_TIMEOUT: REQUIRE(numeric(v)); break;
 case FX_ON: REQUIRE(numeric(v)); break;
 case FX_SCALE: REQUIRE(numeric(v)); break;
 case SPD: REQUIRE(numeric(v)); break;
 case FONT: REQUIRE(font_number(v)); break;
 case AX: REQUIRE(numeric(v)); break;
 case AY: REQUIRE(numeric(v)); break;
 case GARBLE: REQUIRE(numeric(v)); break;
 case GARBLE_INIT: REQUIRE(numeric(v)); break;
 case GARBLE_TIMER: REQUIRE(numeric(v)); break;
 case COL: REQUIRE(numeric(v)); break;
 case SCALE: REQUIRE(numeric(v)); break;
 case ANGLE: REQUIRE(numeric(v)); break;
 case DIR: REQUIRE(numeric(v)); break;
 case GLOW: REQUIRE(numeric(v)); break;
 default: REQUIRE(0);
 }
 return 1;
}
static int collect(void *self,State *s,const int *list,size_t count) {
 Table t; if(!table(self,&t)) return 0;
 uint32_t seen[2]={0,0};
 for(size_t i=0;i<count;++i) {
  int n=list[i]; s->v[n]=peek(&t,n); if(!field_admit(n,s->v[n])) return 0;
  unsigned h=((uintptr_t)s->v[n]>>4)&63; uint32_t bit=1u<<(h&31);
  if(seen[h>>5]&bit) for(size_t j=0;j<i;++j) REQUIRE(s->v[n]!=s->v[list[j]]);
  seen[h>>5]|=bit;
 }
 return 1;
}
static int parent_admit(void *self,State *s) {
 Table t,p; if(!table(self,&t)) return 0; RV *v=peek(&t,PARENT); REQUIRE(v);
 REQUIRE(kind(v)==15);
 REQUIRE(word(v,4)==0x04000001);
 REQUIRE(word(v,0)>=100000);
 s->parent=find_instance((int)word(v,0)); REQUIRE(s->parent);
 if(!table(s->parent,&p)) return 0;
 REQUIRE(isfinite(coordinate(self,0xb8)));
 REQUIRE(isfinite(coordinate(self,0xbc)));
 REQUIRE(isfinite(coordinate(s->parent,0xb8)));
 REQUIRE(isfinite(coordinate(s->parent,0xbc)));
 return 1;
}
static int user_admit(void *self,State *s) {
 static const int list[]={TYPE,GLOW,H_OFFSET,V_OFFSET,FX_SCALE,SCALE,OSC_Y};
 if(!common()) return 0;
 if(!registry((1u<<TEXT)|(1u<<GET_ALPHA))) return 0;
 if(!builtins_ordinary()) return 0;
 if(!collect(self,s,list,sizeof(list)/sizeof(*list))) return 0;
 if(!parent_admit(self,s)) return 0;
 Table t,p; if(!table(self,&t)) return 0;
 if(!table(s->parent,&p)) return 0;
 REQUIRE(string_value(peek(&t,LETTER)));
 REQUIRE(numeric(peek(&p,TXT_GLOW_ALPHA)));
 Table g; if(!table(*global,&g)) return 0;
 REQUIRE(numeric(peek(&g,FONT_SCALE)));
 REQUIRE(numeric(peek(&g,FONT_YOFFSET)));
 return 1;
}
static void user_call(void *self,void *other,State *s) {
 int type=cmp(number(s->v[TYPE]),0)==0?0:cmp(number(s->v[TYPE]),1)==0?1:cmp(number(s->v[TYPE]),2)==0?2:-1;
 if(type<0) return;
 for(int pass=truth(s->v[GLOW])?0:1;pass<2;++pass) {
  Trace *tr=*trace_start; tr->line=type==0?(pass?8:6):type==1?(pass?17:15):(pass?26:24);
  double x=coordinate(s->parent,0xb8)+coordinate(self,0xb8); x+=number(s->v[H_OFFSET]);
  double y=coordinate(s->parent,0xbc)+coordinate(self,0xbc); y+=number(s->v[V_OFFSET]);
  if(type==1) {
   double a=random_value(1*number(s->v[FX_SCALE])),b=random_value(1*number(s->v[FX_SCALE]));
   x=(x+a)-b;
   a=random_value(1*number(s->v[FX_SCALE])); b=random_value(1*number(s->v[FX_SCALE])); y=(y+a)-b;
   letter_native_rng_calls+=4;
  } else if(type==2) y+=(number(s->v[OSC_Y])*3)*number(s->v[FX_SCALE]);
  if(pass) { RV angle=real(0); scaled_call(self,other,x,y,slot(self,LETTER),s->v[SCALE],&angle); }
  else {
   RV alpha=real(0); builtin(GET_ALPHA,&alpha,self,other,0,NULL);
   RV args[7]={real(x),real(y),*slot(self,LETTER),real(2),real(2),real(8388736),real(((value(s->parent,TXT_GLOW_ALPHA)*alpha.value)*.75)*number(s->v[FX_SCALE]))},out=real(0);
   RV *argv[7]; for(int i=0;i<7;++i) argv[i]=args+i;
   glow_script(self,other,&out,7,argv);
  }
 }
}
void ABI letter_native_user(void *self,void *other) {
 State s; if(!user_admit(self,&s)) { ++letter_native_fallbacks[USER_HOOK]; original_user(self,other); return; }
 ++letter_native_hits[USER_HOOK]; Trace tr={*trace_start,"gml_Object_obj_letter_Other_10",2}; *trace_start=&tr;
 user_call(self,other,&s); *trace_start=tr.previous;
}
static int step_admit(void *self,State *s) {
 static const int list[]={DIE,TYPE,RAINBOW,SET_OSC,OSC_DELAY,RAINBOW_VAL,OSC_Y,VSP,ENTRANCE,ALPHA,H_OFFSET,V_OFFSET,
 FX_TIMEOUT,FX_ON,FX_SCALE};
 Table g; if(!common()) return 0;
 if(!collect(self,s,list,sizeof(list)/sizeof(*list))) return 0;
 if(!table(*global,&g)) return 0;
 REQUIRE(numeric(peek(&g,FR)));
 REQUIRE(numeric(peek(&g,TEXT_SPEED)));
 Table t; if(!table(self,&t)) return 0;
 RV *dst=peek(&t,SPD_FR);
 if(dst) {
  REQUIRE(numeric(dst)||kind(dst)==5);
  for(size_t i=0;i<sizeof(list)/sizeof(*list);++i) REQUIRE(dst!=s->v[list[i]]);
 }
 /* A nonpositive fr can make the original setup loop infinite. Delegate. */
 REQUIRE(number(peek(&g,FR))>0);
 REQUIRE(number(s->v[OSC_DELAY])<=4096);
 return 1;
}
/* YYC leaves its spd_fr expression's flags word uninitialized at entry SP-104.
 * Capture it before C allocates a different frame, retaining even stale bits. */
void ABI __attribute__((naked,target("arm"))) letter_native_step(void *self __attribute__((unused)),void *other __attribute__((unused))) {
 __asm__ volatile("ldr r2, [sp, #-104]\n push {r0, r1, r2, lr}\n bl letter_native_step_impl\n cmp r0, #0\n pop {r0, r1, r2, lr}\n str r2, [sp, #-104]\n bxne lr\n ldr r12, =original_step\n ldr r12, [r12]\n bx r12\n .ltorg");
}
int ABI __attribute__((used,noinline)) letter_native_step_impl(void *self,void *other,uint32_t expression_flags) {
 (void)other;
 State s; if(!step_admit(self,&s)) { ++letter_native_fallbacks[STEP_HOOK]; return 0; }
 ++letter_native_hits[STEP_HOOK]; Trace tr={*trace_start,"gml_Object_obj_letter_Step_0",1}; *trace_start=&tr;
 double fr=value(*global,FR),text_speed=value(*global,TEXT_SPEED);
 if(cmp(number(s.v[DIE]),0)==0) {
  if((cmp(number(s.v[TYPE]),2)==0||truth(s.v[RAINBOW]))&&!truth(s.v[SET_OSC])) {
   while(cmp(number(s.v[OSC_DELAY]),0)>0) {
    plus(s.v[RAINBOW_VAL],1*fr);
    if(cmp(number(s.v[OSC_Y]),0)>0) minus(s.v[VSP],.01*fr);
    if(cmp(number(s.v[OSC_Y]),0)<0) plus(s.v[VSP],.01*fr);
    plus(s.v[OSC_Y],number(s.v[VSP])*fr); minus(s.v[OSC_DELAY],1*fr);
   }
   bool_write(s.v[SET_OSC],1); s.v[SET_OSC]->flags=expression_flags;
  }
  if(cmp(number(s.v[TYPE]),2)==0) {
   if(cmp(number(s.v[OSC_Y]),0)>0) minus(s.v[VSP],.01*fr);
   if(cmp(number(s.v[OSC_Y]),0)<0) plus(s.v[VSP],.01*fr);
   plus(s.v[OSC_Y],number(s.v[VSP])*fr);
  }
  if(truth(s.v[RAINBOW])) plus(s.v[RAINBOW_VAL],2*fr);
  if(cmp(number(s.v[ENTRANCE]),1)==0) {
   s.v[SPD_FR]=standard_ref(self,(int)vars[SPD_FR][1]);
   scalar_write(s.v[SPD_FR],1+text_speed); s.v[SPD_FR]->flags=expression_flags;
   if(cmp(number(s.v[ALPHA]),1)<0) plus(s.v[ALPHA],(.075*fr)*number(s.v[SPD_FR]));
   else scalar_write(s.v[ALPHA],1);
   if(cmp(number(s.v[H_OFFSET]),0)>0) minus(s.v[H_OFFSET],(fabs(number(s.v[H_OFFSET]))*.125*fr)*number(s.v[SPD_FR]));
   else scalar_write(s.v[H_OFFSET],0);
   if(cmp(number(s.v[V_OFFSET]),0)>0) minus(s.v[V_OFFSET],(fabs(number(s.v[V_OFFSET]))*.125*fr)*number(s.v[SPD_FR]));
   else scalar_write(s.v[V_OFFSET],0);
  } else { scalar_write(s.v[ALPHA],1); scalar_write(s.v[H_OFFSET],0); scalar_write(s.v[V_OFFSET],0); }
  if(cmp(number(s.v[FX_TIMEOUT]),0)>0) {
   minus(s.v[FX_TIMEOUT],1*fr);
   if(cmp(number(s.v[FX_TIMEOUT]),0)==0) { bool_write(s.v[FX_ON],0); s.v[FX_ON]->flags=expression_flags; }
  }
  if(!truth(s.v[FX_ON])) {
   minus(s.v[FX_SCALE],.02*fr);
   if(cmp(number(s.v[FX_SCALE]),0)<0) scalar_write(s.v[FX_SCALE],0);
  }
 }
 *trace_start=tr.previous; return 1;
}
static void garble_character(void *self,void *other,RV *letter) {
 RV choices[2]={real(random_value(10)+48),real(0)},chosen=real(0),floored=real(0),text=real(0);
 choices[1]=real(random_value(26)+97); RV *argv[2]={choices,choices+1}; choose(&chosen,2,argv);
 builtin(FLOOR,&floored,self,other,1,&chosen); builtin(CHR,&text,self,other,1,&floored);
 owned_write(self,letter,&text); release(&text); letter_native_rng_calls+=3;
}
static void garble_timer(void *self,void *other,RV *timer) {
 RV arg=real(random_value(30)),rounded=real(0); builtin(ROUND,&rounded,self,other,1,&arg);
 RV result=real(20); add(&result,&rounded); owned_write(self,timer,&result); ++letter_native_rng_calls;
}
static int draw_admit(void *self,State *s,Table *fonts) {
 static const int list[]={DIE,SPD,FONT,AX,AY,GARBLE,GARBLE_INIT,GARBLE_TIMER,FX_SCALE,ALPHA,COL,RAINBOW,RAINBOW_VAL,
 TYPE,GLOW,H_OFFSET,V_OFFSET,SCALE,OSC_Y,ANGLE,DIR};
 if(!common()) return 0;
 if(!registry((1u<<NFUNCS)-1)) return 0;
 if(!builtins_ordinary()) return 0;
 if(!collect(self,s,list,sizeof(list)/sizeof(*list))) return 0;
 if(!parent_admit(self,s)) return 0;
 Table t,p,g; if(!table(self,&t)) return 0;
 if(!table(s->parent,&p)) return 0;
 if(!table(*global,&g)) return 0;
 REQUIRE(string_value(peek(&t,LETTER)));
 REQUIRE(string_value(peek(&t,GARBLE_LETTER)));
 REQUIRE(numeric(peek(&g,FR)));
 REQUIRE(numeric(peek(&p,STATE)));
 REQUIRE(numeric(peek(&p,GOTO_ID)));
 REQUIRE(numeric(peek(&p,HAS_CHOICE)));
 REQUIRE(isfinite(coordinate(self,0xdc)));
 REQUIRE(isfinite(coordinate(self,0xd4)));
 REQUIRE(isfinite(coordinate(self,0xd0)));
 REQUIRE(numeric(peek(&p,TXT_GLOW_ALPHA)));
 REQUIRE(vars[ROOM_HEIGHT][1]<500);
 REQUIRE(word(builtin_variables,16*vars[ROOM_HEIGHT][1]+4)==(uintptr_t)room_height);
 RV font=*s->v[FONT],out=real(0); RV *argv[1]={&font}; if(!font_admit(&out,1,argv,fonts)) return 0;
 return 1;
}
void ABI letter_native_draw(void *self,void *other) {
 State s; Table fonts; if(!draw_admit(self,&s,&fonts)) { ++letter_native_fallbacks[DRAW_HOOK]; original_draw(self,other); return; }
 ++letter_native_hits[DRAW_HOOK]; Trace tr={*trace_start,"gml_Object_obj_letter_Draw_0",1}; *trace_start=&tr;
 uint32_t scratch_flags=s.v[SPD]->flags;
 set_builtin(self,SPEED,number(s.v[SPD])*value(*global,FR));
 tr.line=2; Trace ftr={*trace_start,"gml_Script_draw_set_font_localized",453}; *trace_start=&ftr;
 ++letter_native_hits[FONT_HOOK]; font_call(s.v[FONT],&fonts); *trace_start=ftr.previous;
 tr.line=3; set_valign(0); tr.line=4; set_halign(0);
 /* The original exists query remains in the observable builtin stream. */
 tr.line=5; RV parent=*slot(self,PARENT),exists=real(0); builtin(EXISTS,&exists,self,other,1,&parent);
 if(cmp(value(s.parent,STATE),3)!=0) {
  tr.line=9; RV ax=real(coordinate(s.parent,0xb8)); ax.value+=coordinate(self,0xb8); owned_write(self,s.v[AX],&ax);
  tr.line=10; RV ay=real(coordinate(s.parent,0xbc)); ay.value+=coordinate(self,0xbc); owned_write(self,s.v[AY],&ay);
  scratch_flags=0;
 }
 if(truth(s.v[GARBLE])) {
  if(!truth(s.v[GARBLE_INIT])) {
   tr.line=18; bool_write(s.v[GARBLE_INIT],1);
   tr.line=19; owned_write(self,slot(self,GARBLE_LETTER),slot(self,LETTER));
   tr.line=20; garble_character(self,other,slot(self,LETTER));
   scratch_flags=0;
   tr.line=21; garble_timer(self,other,s.v[GARBLE_TIMER]);
  }
  tr.line=23; minus(s.v[GARBLE_TIMER],1);
  if(cmp(number(s.v[GARBLE_TIMER]),0)<=0) {
   tr.line=26; garble_timer(self,other,s.v[GARBLE_TIMER]);
   scratch_flags=0;
   tr.line=27; garble_character(self,other,slot(self,LETTER));
   if(cmp(number(s.v[FX_SCALE]),0)<=0) {
    tr.line=30; owned_write(self,slot(self,LETTER),slot(self,GARBLE_LETTER)); tr.line=31; bool_write(s.v[GARBLE],0);
   }
  }
 }
 if(cmp(value(s.parent,STATE),3)!=0) {
  tr.line=38; set_colour(integer_value(s.v[COL],0)); tr.line=39; set_alpha((float)number(s.v[ALPHA]));
  tr.line=40; event_user(self,other,0);
  if(truth(s.v[RAINBOW])) {
   tr.line=45; set_alpha((float)(number(s.v[ALPHA])*number(s.v[FX_SCALE])));
   tr.line=46; RV colour=real(0),args[3]={real(fmod(number(s.v[RAINBOW_VAL]),255)),real(255),real(255)};
   builtin(HSV,&colour,self,other,3,args); set_colour((int)number(&colour)); tr.line=47; event_user(self,other,0);
  }
  tr.line=49; set_alpha(1); tr.line=50; set_colour(0xffffff);
 } else if(cmp(number(s.v[DIE]),0)==0) {
  tr.line=56; set_builtin(self,X,number(s.v[AX])); tr.line=57; set_builtin(self,Y,number(s.v[AY]));
  tr.line=58; scalar_write(s.v[DIE],1);
  if(cmp(value(s.parent,HAS_CHOICE),0)==0) { tr.line=61; scalar_write(s.v[DIE],2); }
  else if(cmp(value(s.parent,GOTO_ID),0)<0) {
   tr.line=67; scalar_write(s.v[SPD],random_value(5));
   tr.line=68; set_builtin(self,DIRECTION,random_value(360)); letter_native_rng_calls+=2;
  }
 }
 if(cmp(number(s.v[DIE]),1)==0) {
  tr.line=83; parent=*slot(self,PARENT); builtin(EXISTS,&exists,self,other,1,&parent);
  if(truth(&exists)&&cmp(value(s.parent,GOTO_ID),0)>0) {
   tr.line=86; destroy(self,other,0,NULL); tr.line=88; bool_write(s.v[DIE],0);
   s.v[DIE]->flags=scratch_flags;
  }
  if(cmp(number(s.v[DIE]),1)==0) {
   tr.line=94; set_valign(1); tr.line=95; set_halign(1);
   tr.line=96; set_direct(self,(int)vars[SPEED][1],(int)0x80000000,s.v[SPD]);
   tr.line=97; set_builtin(self,VSPEED,coordinate(self,0xdc)+.3*value(*global,FR));
   tr.line=98; RV speed=real(coordinate(self,0xd4)); owned_write(self,s.v[SPD],&speed);
   tr.line=99; set_builtin(self,SPEED,number(s.v[SPD])*value(*global,FR));
   tr.line=100; if(cmp(coordinate(self,0xbc),height(self))>0) { tr.line=102; destroy(self,other,0,NULL); }
   tr.line=104; set_colour(integer_value(s.v[COL],0));
   tr.line=105; scaled_call(self,other,coordinate(self,0xb8),coordinate(self,0xbc),slot(self,LETTER),s.v[SCALE],s.v[ANGLE]);
   tr.line=106; set_colour(0xffffff); tr.line=107; plus(s.v[ANGLE],number(s.v[DIR])*value(*global,FR));
  }
 }
 if(cmp(number(s.v[DIE]),2)==0) {
  tr.line=113; minus(s.v[ALPHA],.1);
  if(cmp(number(s.v[ALPHA]),0)<=0) { tr.line=116; destroy(self,other,0,NULL); }
  tr.line=118; set_colour(integer_value(s.v[COL],0)); tr.line=119; set_alpha((float)number(s.v[ALPHA]));
  tr.line=120; RV angle=real(0); scaled_call(self,other,coordinate(self,0xb8),coordinate(self,0xbc),slot(self,LETTER),s.v[SCALE],&angle);
  tr.line=121; set_alpha(1); tr.line=122; set_colour(0xffffff);
 }
 tr.line=125; set_alpha(1); *trace_start=tr.previous;
}
int letter_native_install(void *(*rx_alloc)(size_t)) {
 if(installed||!rx_alloc) return 0;
#define RESOLVE(v,t,n) do { v=(t)so_symbol(&yoyoloader_mod,n); if(!v) return 0; } while(0)
 static const char *const hooks[]={"_Z28gml_Object_obj_letter_Draw_0P9CInstanceS0_","_Z28gml_Object_obj_letter_Step_0P9CInstanceS0_",
 "_Z30gml_Object_obj_letter_Other_10P9CInstanceS0_","_Z39gml_Script_draw_text_scaled_transformedP9CInstanceS0_R8YYRValueiPPS1_",
 "_Z34gml_Script_draw_set_font_localizedP9CInstanceS0_R8YYRValueiPPS1_"};
 static const uint32_t offsets[]={0x42b6628,0x42b0dfc,0x42b2834,0x198bc50,0x1952860};
 uintptr_t entry[5]; for(int i=0;i<5;++i) RESOLVE(entry[i],uintptr_t,hooks[i]);
 if(entry[0]<offsets[0]) return 0;
 uintptr_t bias=entry[0]-offsets[0]; if(bias>UINTPTR_MAX-0x04f00000) return 0;
 for(int i=0;i<5;++i) if((entry[i]&3)||entry[i]!=bias+offsets[i]||word((void *)entry[i],0)!=0xe92d4ff0||word((void *)entry[i],4)!=0xe28db01c) return 0;
 RESOLVE(global,void **,"g_pGlobal"); RESOLVE(profile,const uint8_t *,"g_bProfile"); RESOLVE(epsilon,const double *,"g_GMLMathEpsilon");
 RESOLVE(functions,Function **,"the_functions"); RESOLVE(current_function,Function **,"g_pFunction");
 RESOLVE(trace_start,Trace **,"_ZN13SYYStackTrace8s_pStartE");
 RESOLVE(standard_get,ref_fn,"_ZN12YYObjectBase19InternalGetYYVarRefEi"); RESOLVE(standard_ref,ref_fn,"_ZN12YYObjectBase20InternalGetYYVarRefLEi");
 RESOLVE(add,op_fn,"_ZN8YYRValuepLERKS_"); RESOLVE(subtract,op_fn,"_ZN8YYRValuemIERKS_");
 RESOLVE(multiply,op_fn,"_ZN8YYRValuemLERKS_");
 RESOLVE(choose,choose_fn,"_Z12YYGML_chooseR8YYRValueiPPS_"); RESOLVE(compare,compare_fn,"_Z12YYCompareValRK6RValueS1_db");
 RESOLVE(random_value,random_fn,"_Z12YYGML_randomd"); RESOLVE(find_instance,find_fn,"_Z18YYGML_FindInstancei");
 RESOLVE(set_direct,set_fn,"_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue");
 RESOLVE(event_user,user_fn,"_Z16YYGML_event_userP9CInstanceS0_i"); RESOLVE(destroy,destroy_fn,"_Z22YYGML_instance_destroyP9CInstanceS0_iPP8YYRValue");
 RESOLVE(set_colour,int_fn,"_Z21YYGML_draw_set_colouri"); RESOLVE(set_alpha,alpha_fn,"_Z20YYGML_draw_set_alphaf");
 RESOLVE(integer_value,convert_int_fn,"_Z10YYGetInt32PK6RValuei");
 RESOLVE(set_font,int_fn,"_Z19YYGML_draw_set_fonti"); RESOLVE(set_halign,int_fn,"_Z21YYGML_draw_set_haligni"); RESOLVE(set_valign,int_fn,"_Z21YYGML_draw_set_valigni");
 RESOLVE(release,free_fn,"_Z16FREE_RValue__PreP6RValue"); copy_owned=(copy_fn)(bias+0x42ba26c);
 RESOLVE(push_context,push_fn,"_Z16PushContextStackP12YYObjectBase"); RESOLVE(pop_context,pop_fn,"_Z15PopContextStacki");
 RESOLVE(glow_script,script_fn,"_Z36gml_Script_draw_text_scaled_glow_extP9CInstanceS0_R8YYRValueiPPS1_");
 RESOLVE(builtin_variables,const void *,"builtin_variables"); RESOLVE(builtin_count,const int *,"g_nStartGlobalVariables");
 RESOLVE(room_height,getter_fn,"_Z13GV_RoomHeightP9CInstanceiP6RValue");
 static const char *const getter_names[]={"_Z4GV_XP9CInstanceiP6RValue","_Z4GV_YP9CInstanceiP6RValue","_Z8GV_SpeedP9CInstanceiP6RValue","_Z9GV_VspeedP9CInstanceiP6RValue","_Z12GV_DirectionP9CInstanceiP6RValue"};
 static const char *const setter_names[]={"_Z4SV_XP9CInstanceiP6RValue","_Z4SV_YP9CInstanceiP6RValue","_Z8SV_SpeedP9CInstanceiP6RValue","_Z9SV_VSpeedP9CInstanceiP6RValue","_Z12SV_DirectionP9CInstanceiP6RValue"};
 for(int i=0;i<5;++i) { RESOLVE(getters[i],uintptr_t,getter_names[i]); RESOLVE(setters[i],uintptr_t,setter_names[i]); }
 char name[64]; for(int i=0;i<NVARS;++i) { strcpy(name,"g_VAR_"); strcat(name,var_names[i]); RESOLVE(vars[i],const uint32_t *,name); }
 for(int i=0;i<NFUNCS;++i) { strcpy(name,"g_FUNC_"); strcat(name,fn_names[i]); RESOLVE(descriptors[i],const uint32_t *,name); RESOLVE(builtins[i],builtin_fn,fn_symbols[i]); }
#undef RESOLVE
 void *rx=rx_alloc(80);
 if(!rx||((uintptr_t)rx&3)||(uintptr_t)rx>UINTPTR_MAX-80||((uintptr_t)rx<bias+0x04f00000&&(uintptr_t)rx+80>bias)) return 0;
 uint32_t code[20]; for(int i=0;i<5;++i) { code[4*i]=0xe92d4ff0; code[4*i+1]=0xe28db01c; code[4*i+2]=0xe51ff004; code[4*i+3]=(uint32_t)(entry[i]+8); }
 kuKernelCpuUnrestrictedMemcpy(rx,code,sizeof(code)); kuKernelFlushCaches(rx,sizeof(code));
 original_draw=(event_fn)rx; original_step=(event_fn)((uint8_t *)rx+16); original_user=(event_fn)((uint8_t *)rx+32);
 original_scaled=(script_fn)((uint8_t *)rx+48); original_font=(script_fn)((uint8_t *)rx+64);
 uintptr_t replacements[]={(uintptr_t)letter_native_draw,(uintptr_t)letter_native_step,(uintptr_t)letter_native_user,(uintptr_t)letter_native_scaled,(uintptr_t)letter_native_font};
 for(int i=0;i<5;++i) { hook_addr(entry[i],replacements[i]); kuKernelFlushCaches((void *)entry[i],8); }
 installed=1; return 1;
}

