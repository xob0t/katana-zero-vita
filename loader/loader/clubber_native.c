/* libyoyo.so SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701.
 * ARMv7 AAPCS softfp, -O2, -ffp-contract=off, no fast-math.
 * Install stopped after import resolution; see clubber_native.md. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#ifdef CLUBBER_TEST
#include "collision_stub.h"
#else
#include <kubridge.h>
#include "main.h"
#include "so_util.h"
#endif
#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags,kind; } RV;
typedef void ABI (*event_fn)(void *,void *);
typedef RV *ABI (*script_fn)(void *,void *,RV *,int,RV **);
typedef RV *ABI (*ref_fn)(void *,int);
typedef int ABI (*bool_fn)(const RV *);
typedef int ABI (*int_fn)(const RV *);
typedef int64_t ABI (*int64_fn)(const RV *);
typedef void ABI (*builtin_fn)(RV *,void *,void *,int,RV *);
typedef float ABI (*direction_fn)(float,float,float,float);
typedef void ABI (*draw_fn)(void *,int,float,float,float,float,float,float,int,float);
typedef void ABI (*set_fn)(void *,int,int,RV *);
typedef void ABI (*push_fn)(void *);
typedef void ABI (*pop_fn)(int);
typedef struct { void *saved_self,*saved_other,*begin,*end; } Iterator;
typedef int ABI (*with_fn)(Iterator *,void **,void **,const RV *);
typedef int ABI (*next_fn)(Iterator *,void **,void **);
typedef void ABI (*delete_fn)(Iterator *,void **,void **);
typedef struct { const char *name; builtin_fn call; int argc; } Function;
typedef struct Trace { struct Trace *previous; const char *name; int line; } Trace;
enum { LIGHT_DRAWME,RIM_COLOR,RIM_DIR,RIM_ALPHA,DIR_ALPHA,CLOSE_LIGHTS,CLOSE_ARRAY,
 BOTTOM,TOP,LEFT,RIGHT,ANIMATION,DIR,WIDTH,HEIGHT,ALPHA,COLOR,CHARACTER,
 XVIEW,YVIEW,WVIEW,HVIEW,X,Y,IMAGE_ANGLE,IMAGE_ALPHA,NVARS };
static const char *const var_names[NVARS]={"light_drawme","rim_color","rim_dir","rim_alpha","dir_alpha",
 "close_lights","close_light_array","bottom_sprite","top_sprite","left_sprite","right_sprite",
 "animation_index","dir","width","height","alpha","color","character_lighting",
 "xview","yview","wview","hview","x","y","image_angle","image_alpha"};
static const uint32_t *vars[NVARS];
enum { OTHER,DISTANCE,NFUNCS };
static const char *const fn_names[NFUNCS]={"Other","point_distance"};
static const char *const fn_symbols[NFUNCS]={"_Z9F_JSOtherR6RValueP9CInstanceS2_iPS_",
 "_Z15F_PointDistanceR6RValueP9CInstanceS2_iPS_"};
static const uint32_t *descriptors[NFUNCS];
static builtin_fn builtins[NFUNCS];
static Function **functions,**current_function;
static Trace **trace_start;
static void **global;
static const uint8_t *profile;
static const double *epsilon;
static const uint32_t *instance_hash;
static const int *builtin_count;
static const void *builtin_variables;
static uintptr_t getters[4],setters[4];
static ref_fn standard_get,standard_ref;
static script_fn inside_screen;
static bool_fn boolean;
static int_fn integer;
static int64_fn integer64;
static direction_fn direction;
static draw_fn draw_sprite;
static set_fn set_direct;
static push_fn push_context;
static pop_fn pop_context;
static with_fn new_with;
static next_fn next_with;
static delete_fn delete_with;
static event_fn original;
static int installed;
unsigned clubber_native_hits,clubber_native_fallbacks,clubber_native_draws;
/* Reasons are stable API indices, see the enum and report. */
enum { PROFILE,HELPER_HOOK,REGISTRY,VARIABLE_IDS,BUILTIN_OVERRIDE,TABLE,
 MISSING,VALUE_KIND,ARRAY_SHAPE,ARRAY_ENTRY,INSTANCE_MAP,INSTANCE_STATE,ALIAS,NREASONS };
unsigned clubber_native_reject_count[NREASONS];
static int reject(unsigned reason) { ++clubber_native_reject_count[reason]; return 0; }
#define REQUIRE(c,r) do { if(!(c)) return reject(r); } while(0)
static uint32_t word(const void *p,size_t o) { uint32_t v; memcpy(&v,(const uint8_t *)p+o,4); return v; }
static float scalar(const void *p,size_t o) { float v; memcpy(&v,(const uint8_t *)p+o,4); return v; }
static RV real(double x) { RV v={x,0,0}; return v; }
/* Exact full kinds only. Tagged numeric metadata and owned values decline. */
static int numeric(const RV *v) { return v&&v->kind==0&&isfinite(v->value)&&fabs(v->value)<1073741824.0; }
static int plain(const RV *v) { return v&&(v->kind==0||v->kind==7||v->kind==10||v->kind==13); }
static int sprite(const RV *v) {
 return numeric(v)||(v&&v->kind==15&&word(v,4)==0x01000001&&word(v,0)<1048576);
}
typedef struct { const uint8_t *entries; uint32_t mask; } Table;
static int table(void *owner,Table *t) {
 REQUIRE(owner,TABLE);
 const void *vt=(const void *)(uintptr_t)word(owner,0);
 REQUIRE(vt&&word(vt,8)==(uintptr_t)standard_get&&word(vt,12)==(uintptr_t)standard_ref,TABLE);
 /* Fixed-index storage makes direct access take a different path. */
 REQUIRE(!word(owner,4),TABLE);
 const void *ht=(const void *)(uintptr_t)word(owner,0x24); REQUIRE(ht,TABLE);
 t->mask=word(ht,8); t->entries=(const uint8_t *)(uintptr_t)word(ht,16);
 REQUIRE(t->entries&&t->mask<=65535&&!(t->mask&(t->mask+1)),TABLE); return 1;
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
/* Finite REAL versus finite REAL uses this subtract/abs/sign path in
 * YYCompareVal e3ee1c/e3f31c. Bounds in admission preclude overflow. */
static int cmp(double a,double b) { double d=a-b; return fabs(d)<=*epsilon?0:(d<0?-1:1); }
typedef struct { void *owner; RV *alpha,*color; } Light;
enum { MAX_LIGHTS=32 };
typedef struct { RV *v[HEIGHT+1]; Light light[MAX_LIGHTS]; RV *items,*character; int count; } State;
/* Helper entry fingerprints are filled from the audited ELF below. */
typedef struct { const char *name; uint32_t offset,a,b; uintptr_t address; uintptr_t *got; } Helper;
static Helper helpers[] = {
 {"_ZN12YYObjectBase19InternalGetYYVarRefEi",0xf21708,0xe92d4df0,0xe28db018,0,NULL},
 {"_ZN12YYObjectBase20InternalGetYYVarRefLEi",0xf219b0,0xe92d4bf0,0xe28db018,0,NULL},
 {"_Z24gml_Script_inside_screenP9CInstanceS0_R8YYRValueiPPS1_",0x314677c,0xe92d4ff0,0xe28db01c,0,NULL},
 {"_Z11BOOL_RValuePK6RValue",0xec9c5c,0xe92d4c10,0xe28db008,0,NULL},
 {"_Z12INT32_RValuePK6RValue",0xec7bdc,0xe92d4c10,0xe28db008,0,NULL},
 {"_Z12INT64_RValuePK6RValue",0xec9a58,0xe92d4c10,0xe28db008,0,NULL},
 {"_Z21YYGML_point_directionffff",0xdfa184,0xee010a10,0xee022a10,0,NULL},
 {"_Z21YYGML_draw_sprite_extP9CInstanceiffffffif",0xddec54,0xe92d4ff0,0xe28db01c,0,NULL},
 {"_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue",0xf8936c,0xe92d4df0,0xe28db018,0,NULL},
 {"_Z16PushContextStackP12YYObjectBase",0xecc284,0xe92d48f0,0xe28db010,0,NULL},
 {"_Z15PopContextStacki",0xecc33c,0xe59f1014,0xe79f1001,0,NULL},
 {"_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue",0xee0188,0xe92d4bf0,0xe28db018,0,NULL},
 {"_Z22YYGML_WithIteratorNextP13SWithIteratorPP12YYObjectBaseS3_",0xee04b8,0xe5903008,0xe1a02000,0,NULL},
 {"_Z24YYGML_DeleteWithIteratorP13SWithIteratorPP12YYObjectBaseS3_",0xee04ec,0xe92d48f0,0xe28db010,0,NULL},
 {"_Z9F_JSOtherR6RValueP9CInstanceS2_iPS_",0xf3afb4,0xe3a01006,0xe5802000,0,NULL},
 {"_Z15F_PointDistanceR6RValueP9CInstanceS2_iPS_",0xdf9cac,0xe92d48f0,0xe28db010,0,NULL},
 {"_Z12YYCompareValRK6RValueS1_db",0xe3e8f8,0xe92d4970,0xe28db010,0,NULL},
 {"_Z4GV_XP9CInstanceiP6RValue",0xf844d4,0xed900a2e,0xe3a00000,0,NULL},
 {"_Z4GV_YP9CInstanceiP6RValue",0xf844f0,0xed900a2f,0xe3a00000,0,NULL},
 {"_Z13GV_ImageAngleP9CInstanceiP6RValue",0xf84b2c,0xed900a2b,0xe3a00000,0,NULL},
 {"_Z13GV_ImageAlphaP9CInstanceiP6RValue",0xf84b48,0xed900a2c,0xe3a00000,0,NULL},
 {"_Z4SV_XP9CInstanceiP6RValue",0xf84e6c,0xe92d4c10,0xe28db008,0,NULL},
 {"_Z4SV_YP9CInstanceiP6RValue",0xf84ebc,0xe92d4c10,0xe28db008,0,NULL},
 {"_Z13SV_ImageAngleP9CInstanceiP6RValue",0xf85678,0xe92d4c10,0xe28db008,0,NULL},
 {"_Z13SV_ImageAlphaP9CInstanceiP6RValue",0xf856c4,0xe92d4c10,0xe28db008,0,NULL},
 {"_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_",0xede028,0xe92d4ff0,0xe28db01c,0,NULL},
 {"_ZN8YYRValuepLEi",0xbae490,0xe92d4c10,0xe28db008,0,NULL},
 {"_ZN8YYRValuepLERKS_",0xbaed0c,0xe92d4830,0xe28db008,0,NULL},
 {"_ZN8YYRValuemLERKS_",0xbae6ac,0xe92d48f0,0xe28db010,0,NULL},
 {"_ZN8YYRValuemIERKS_",0xbaeb4c,0xe92d4830,0xe28db008,0,NULL},
 {"_ZmlRK8YYRValuei",0xbae3ac,0xe92d4830,0xe28db008,0,NULL},
 {"_ZdvRK8YYRValued",0xbdf85c,0xe92d4830,0xe28db008,0,NULL},
 {"_ZmiRK8YYRValuei",0xbdf9a4,0xe92d4830,0xe28db008,0,NULL},
 {"_Z9YYGML_absd",0xdf2fcc,0xe7df1f9f,0xe12fff1e,0,NULL},
 {"_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb",0xf89eb0,0xe92d4ef0,0xe28db018,0,NULL},
 {"_Z32YYGML_ErrCheck_Variable_GetValueRK8YYRValueiiP6RValueP9CInstanceS5_",0xf8a7fc,0xe92d4df0,0xe28db018,0,NULL},
 {"_Z23YYGML_Variable_SetValueRK8YYRValueiiP6RValue",0xf8a058,0xe92d4c70,0xe28db010,0,NULL},
};
static int ordinary(void) {
 REQUIRE(!*profile,PROFILE);
 for(unsigned i=0;i<sizeof(helpers)/sizeof(*helpers);++i) {
  REQUIRE(word((void *)helpers[i].address,0)==helpers[i].a&&word((void *)helpers[i].address,4)==helpers[i].b,HELPER_HOOK);
  REQUIRE(!helpers[i].got||*helpers[i].got==helpers[i].address,HELPER_HOOK);
 }
 REQUIRE(*functions,REGISTRY);
 for(int i=0;i<NFUNCS;++i)
  REQUIRE(descriptors[i][1]<16384&&(*functions)[descriptors[i][1]].call==builtins[i],REGISTRY);
 REQUIRE(isfinite(*epsilon)&&*epsilon>=0&&*epsilon<=0.00001,VALUE_KIND);
 REQUIRE(*builtin_count>0&&*builtin_count<=500,VARIABLE_IDS);
 for(int i=0;i<X;++i) REQUIRE(vars[i][1]>=(uint32_t)*builtin_count&&vars[i][1]<0x7ffffffe,VARIABLE_IDS);
 for(int i=0;i<4;++i) {
  REQUIRE(vars[X+i][1]<(uint32_t)*builtin_count,VARIABLE_IDS);
  REQUIRE(word(builtin_variables,16*vars[X+i][1]+4)==getters[i]&&
          word(builtin_variables,16*vars[X+i][1]+8)==setters[i]&&
          (word(builtin_variables,16*vars[X+i][1]+12)&255)==1,BUILTIN_OVERRIDE);
 }
 return 1;
}
static int lookup(uint32_t id,void **out) {
 const void *buckets=(const void *)(uintptr_t)instance_hash[0]; uint32_t mask=instance_hash[1];
 REQUIRE(buckets&&mask<=1048575&&!(mask&(mask+1)),INSTANCE_MAP);
 const void *node=(const void *)(uintptr_t)word(buckets,8*(id&mask));
 for(unsigned chain=0;node;++chain,node=(const void *)(uintptr_t)word(node,4)) {
  REQUIRE(chain<4096,INSTANCE_MAP);
  if(word(node,8)==id) { *out=(void *)(uintptr_t)word(node,12); return 1; }
 }
 *out=NULL; return 1;
}
static int admit(void *self,State *s) {
 Table a,g;
 if(!ordinary()||!table(self,&a)||!table(*global,&g)) return 0;
 REQUIRE(word(self,0x50)==1&&(word(self,0x7c)&0x100007)==4,INSTANCE_STATE);
 for(int i=0;i<=HEIGHT;++i) {
  s->v[i]=peek(&a,i); REQUIRE(s->v[i],MISSING);
  /* Only writes can make a slot alias change subsequent reads. */
  for(int j=0;j<i&&j<=DIR_ALPHA;++j) REQUIRE(s->v[i]!=s->v[j],ALIAS);
 }
 for(int i=LIGHT_DRAWME;i<=DIR_ALPHA;++i) REQUIRE(plain(s->v[i]),VALUE_KIND);
 REQUIRE(numeric(s->v[CLOSE_LIGHTS])&&s->v[CLOSE_LIGHTS]->value>=0&&s->v[CLOSE_LIGHTS]->value<=MAX_LIGHTS,VALUE_KIND);
 /* Fractional counts are ordinary REAL comparisons; preflight the exact loop. */
 s->count=0; while(cmp(s->count,s->v[CLOSE_LIGHTS]->value)<0) ++s->count;
 for(int i=BOTTOM;i<=RIGHT;++i) REQUIRE(sprite(s->v[i]),VALUE_KIND);
 for(int i=ANIMATION;i<=HEIGHT;++i) REQUIRE(numeric(s->v[i]),VALUE_KIND);
 for(int i=0;i<4;++i) REQUIRE(isfinite(scalar(self,(size_t[]){0xb8,0xbc,0xac,0xb0}[i])),VALUE_KIND);
 s->character=peek(&g,CHARACTER); REQUIRE(s->character,MISSING);
 REQUIRE(numeric(s->character)||(s->character->kind==13&&isfinite(s->character->value)),VALUE_KIND);
 for(int i=XVIEW;i<=HVIEW;++i) REQUIRE(numeric(peek(&g,i)),VALUE_KIND);
 /* Even an empty list may have a scalar instead of an array in normal GML.
  * The event never indexes it, so no array check is needed in that case. */
 s->items=NULL; if(!s->count) return 1;
 RV *arr=s->v[CLOSE_ARRAY]; REQUIRE(arr->kind==2&&word(arr,0),ARRAY_SHAPE);
 const void *header=(const void *)(uintptr_t)word(arr,0);
 REQUIRE(word(header,0x1c)>=(uint32_t)s->count&&word(header,4),ARRAY_SHAPE);
 s->items=(RV *)(uintptr_t)word(header,4);
 for(int i=0;i<s->count;++i) {
  const RV *entry=s->items+i; uint32_t id;
  if(entry->kind==15) { REQUIRE(word(entry,4)==0x04000001,ARRAY_ENTRY); id=word(entry,0); }
  else { REQUIRE(numeric(entry)&&entry->value>=100000&&entry->value<=2147483647&&trunc(entry->value)==entry->value,ARRAY_ENTRY); id=(uint32_t)entry->value; }
  REQUIRE(id>=100000&&id<=2147483647,ARRAY_ENTRY);
  void *light; if(!lookup(id,&light)) return 0;
  s->light[i]=(Light){light,NULL,NULL};
  if(!light||(word(light,0x7c)&0x100007)!=4) continue;
  REQUIRE(light!=self,ALIAS); REQUIRE(word(light,0x50)==1,INSTANCE_STATE);
  Table l; if(!table(light,&l)) return 0;
  s->light[i].alpha=peek(&l,ALPHA); s->light[i].color=peek(&l,COLOR);
  REQUIRE(s->light[i].alpha&&s->light[i].color,MISSING);
  REQUIRE(numeric(s->light[i].alpha)&&numeric(s->light[i].color),VALUE_KIND);
  REQUIRE(isfinite(scalar(light,0xb8))&&isfinite(scalar(light,0xbc)),VALUE_KIND);
  for(int j=0;j<=HEIGHT;++j) REQUIRE(s->light[i].alpha!=s->v[j]&&s->light[i].color!=s->v[j],ALIAS);
 }
 return 1;
}
static void builtin(int n,RV *out,void *self,void *other,int argc,RV *args) {
 Function *saved=*current_function; *out=real(0); out->kind=5;
 *current_function=*functions+descriptors[n][1]; builtins[n](out,self,other,argc,args); *current_function=saved;
}
static void set(void *self,int n,RV *v) { set_direct(self,(int)vars[n][1],(int)0x80000000,v); }
static void directional(void *actor,State *s,int sprite_index,double center,Trace *tr,int line) {
 tr->line=line;
 push_context(actor);
 s->v[DIR_ALPHA]->value=fabs(s->v[RIM_DIR]->value-center)/-90.0+1.0; s->v[DIR_ALPHA]->kind=0;
 pop_context(1);
 for(int repeat=0;repeat<2;++repeat) {
  tr->line=line+1+repeat;
  /* INT64_RValue supplies the colour low word, including its exact rounding. */
  int col=(int)integer64(s->v[RIM_COLOR]);
  double opacity=((double)scalar(actor,0xb0)*s->v[RIM_ALPHA]->value)*s->v[DIR_ALPHA]->value;
  draw_sprite(actor,integer(s->v[sprite_index]),(float)s->v[ANIMATION]->value,
   scalar(actor,0xb8),scalar(actor,0xbc),(float)(s->v[DIR]->value*s->v[WIDTH]->value),
   (float)s->v[HEIGHT]->value,scalar(actor,0xac),col,(float)opacity);
  ++clubber_native_draws;
 }
}
void ABI clubber_native_event(void *self,void *other) {
 State s;
 if(!admit(self,&s)) { ++clubber_native_fallbacks; original(self,other); return; }
 ++clubber_native_hits;
 Trace tr={*trace_start,"gml_Object_obj_clubber_Other_16",0}; *trace_start=&tr;
 /* Original obtains both variable references before inside_screen. */
 RV *character=standard_get(*global,(int)vars[CHARACTER][1]);
 RV *drawme=standard_ref(self,(int)vars[LIGHT_DRAWME][1]);
 RV margin=real(40),visible=real(0); RV *args[]={&margin};
 int yes=boolean(inside_screen(self,other,&visible,1,args));
 if(yes) yes=boolean(character);
 push_context(self); drawme->value=yes?1:0; drawme->kind=13; pop_context(1);
 tr.line=3;
 for(int i=0;cmp(i,s.v[CLOSE_LIGHTS]->value)<0;++i) {
  Iterator outer;
  tr.line=5;
  if(new_with(&outer,&self,&other,s.items+i)>=1) do {
   RV actor=real(0); tr.line=7; builtin(OTHER,&actor,self,other,0,NULL);
   void *dancer=(void *)(uintptr_t)word(&actor,0);
   double ax=scalar(dancer,0xb8),ay=scalar(dancer,0xbc);
   tr.line=10;
   if(cmp(s.light[i].alpha->value,0)>0) {
    RV distance,args4[]={real(scalar(self,0xb8)),real(scalar(self,0xbc)),real(ax),real(ay-32)};
    tr.line=12; builtin(DISTANCE,&distance,self,other,4,args4);
    tr.line=13;
    if(cmp(distance.value,200)<=0) {
     tr.line=15; set(dancer,RIM_COLOR,s.light[i].color);
     tr.line=16; RV angle=real((double)direction(scalar(self,0xb8),scalar(self,0xbc),(float)ax,(float)ay)); set(dancer,RIM_DIR,&angle);
     tr.line=17; RV alpha=real((((1.0-distance.value/200.0)*s.light[i].alpha->value)*2.0)*2.0); set(dancer,RIM_ALPHA,&alpha);
     Iterator inner; tr.line=18;
     if(new_with(&inner,&self,&other,&actor)>=1) do {
      tr.line=20;
      if(boolean(s.v[LIGHT_DRAWME])) {
       tr.line=23;
       if(cmp(s.v[RIM_DIR]->value,0)>0&&cmp(s.v[RIM_DIR]->value,180)<0) directional(self,&s,BOTTOM,90,&tr,25);
       tr.line=30;
       if(cmp(s.v[RIM_DIR]->value,180)>0&&cmp(s.v[RIM_DIR]->value,360)<0) directional(self,&s,TOP,270,&tr,32);
       tr.line=37;
       if(cmp(s.v[RIM_DIR]->value,90)>0&&cmp(s.v[RIM_DIR]->value,270)<0) directional(self,&s,LEFT,180,&tr,39);
       tr.line=44;
       if(cmp(s.v[RIM_DIR]->value,180)<0) { tr.line=46; s.v[RIM_DIR]->value+=360; s.v[RIM_DIR]->kind=0; }
       tr.line=48;
       if(cmp(s.v[RIM_DIR]->value,270)>0&&cmp(s.v[RIM_DIR]->value,450)<0) directional(self,&s,RIGHT,360,&tr,50);
      }
     } while(next_with(&inner,&self,&other));
     delete_with(&inner,&self,&other);
    }
   }
  } while(next_with(&outer,&self,&other));
  delete_with(&outer,&self,&other); tr.line=3;
 }
 *trace_start=tr.previous;
}
int clubber_native_install(void *(*rx_alloc)(size_t)) {
 if(installed||!rx_alloc) return 0;
#define RESOLVE(v,t,n) do { v=(t)so_symbol(&yoyoloader_mod,n); if(!v) return 0; } while(0)
 uintptr_t entry; RESOLVE(entry,uintptr_t,"_Z31gml_Object_obj_clubber_Other_16P9CInstanceS0_");
 if((entry&3)||entry<0x373d9f8||word((void *)entry,0)!=0xe92d4ff0||word((void *)entry,4)!=0xe28db01c) return 0;
 uintptr_t bias=entry-0x373d9f8; if(bias>UINTPTR_MAX-0x06000000) return 0;
 for(unsigned i=0;i<sizeof(helpers)/sizeof(*helpers);++i) {
  RESOLVE(helpers[i].address,uintptr_t,helpers[i].name);
  if(helpers[i].address!=bias+helpers[i].offset||word((void *)helpers[i].address,0)!=helpers[i].a||word((void *)helpers[i].address,4)!=helpers[i].b) return 0;
  helpers[i].got=so_got_slot(&yoyoloader_mod,helpers[i].name);
 }
 RESOLVE(global,void **,"g_pGlobal"); RESOLVE(profile,const uint8_t *,"g_bProfile"); RESOLVE(epsilon,const double *,"g_GMLMathEpsilon");
 RESOLVE(functions,Function **,"the_functions"); RESOLVE(current_function,Function **,"g_pFunction");
 RESOLVE(trace_start,Trace **,"_ZN13SYYStackTrace8s_pStartE");
 RESOLVE(instance_hash,const uint32_t *,"_ZN9CInstance14ms_ID2InstanceE");
 RESOLVE(standard_get,ref_fn,"_ZN12YYObjectBase19InternalGetYYVarRefEi"); RESOLVE(standard_ref,ref_fn,"_ZN12YYObjectBase20InternalGetYYVarRefLEi");
 RESOLVE(inside_screen,script_fn,"_Z24gml_Script_inside_screenP9CInstanceS0_R8YYRValueiPPS1_");
 RESOLVE(boolean,bool_fn,"_Z11BOOL_RValuePK6RValue"); RESOLVE(integer,int_fn,"_Z12INT32_RValuePK6RValue"); RESOLVE(integer64,int64_fn,"_Z12INT64_RValuePK6RValue");
 RESOLVE(direction,direction_fn,"_Z21YYGML_point_directionffff"); RESOLVE(draw_sprite,draw_fn,"_Z21YYGML_draw_sprite_extP9CInstanceiffffffif");
 RESOLVE(set_direct,set_fn,"_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue");
 RESOLVE(push_context,push_fn,"_Z16PushContextStackP12YYObjectBase"); RESOLVE(pop_context,pop_fn,"_Z15PopContextStacki");
 RESOLVE(new_with,with_fn,"_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue");
 RESOLVE(next_with,next_fn,"_Z22YYGML_WithIteratorNextP13SWithIteratorPP12YYObjectBaseS3_"); RESOLVE(delete_with,delete_fn,"_Z24YYGML_DeleteWithIteratorP13SWithIteratorPP12YYObjectBaseS3_");
 RESOLVE(builtin_count,const int *,"g_nStartGlobalVariables"); RESOLVE(builtin_variables,const void *,"builtin_variables");
 const char *const get_names[]={"_Z4GV_XP9CInstanceiP6RValue","_Z4GV_YP9CInstanceiP6RValue","_Z13GV_ImageAngleP9CInstanceiP6RValue","_Z13GV_ImageAlphaP9CInstanceiP6RValue"};
 const char *const set_names[]={"_Z4SV_XP9CInstanceiP6RValue","_Z4SV_YP9CInstanceiP6RValue","_Z13SV_ImageAngleP9CInstanceiP6RValue","_Z13SV_ImageAlphaP9CInstanceiP6RValue"};
 for(int i=0;i<4;++i) { RESOLVE(getters[i],uintptr_t,get_names[i]); RESOLVE(setters[i],uintptr_t,set_names[i]); }
 char name[64]; for(int i=0;i<NVARS;++i) { strcpy(name,"g_VAR_"); strcat(name,var_names[i]); RESOLVE(vars[i],const uint32_t *,name); }
 for(int i=0;i<NFUNCS;++i) { strcpy(name,"g_FUNC_"); strcat(name,fn_names[i]); RESOLVE(descriptors[i],const uint32_t *,name); RESOLVE(builtins[i],builtin_fn,fn_symbols[i]); }
#undef RESOLVE
 void *rx=rx_alloc(16);
 if(!rx||((uintptr_t)rx&3)||(uintptr_t)rx>UINTPTR_MAX-16||((uintptr_t)rx<bias+0x06000000&&(uintptr_t)rx+16>bias)) return 0;
 uint32_t code[]={0xe92d4ff0,0xe28db01c,0xe51ff004,(uint32_t)(entry+8)};
 kuKernelCpuUnrestrictedMemcpy(rx,code,sizeof(code)); kuKernelFlushCaches(rx,sizeof(code));
 original=(event_fn)rx; hook_addr(entry,(uintptr_t)clubber_native_event); kuKernelFlushCaches((void *)entry,8); installed=1; return 1;
}
