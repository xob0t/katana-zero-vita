/* APK 1.10 libyoyo SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701.
 * ARMv7 AAPCS softfp, -marm -O2 -ffp-contract=off, no fast-math.
 * Hook spawner Draw_0. Retain original setters, nearest and iterator. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#ifdef DUSTSPAWNER_TEST
#include "collision_stub.h"
#else
#include <kubridge.h>
#include "main.h"
#include "so_util.h"
#endif
#define ABI __attribute__((pcs("aapcs")))
typedef struct { double value; uint32_t flags,kind; } RV;
typedef void ABI (*Event)(void *,void *);
typedef void ABI (*Setter)(void *,int,int,RV *);
typedef void ABI (*Context)(void *);
typedef void ABI (*Pop)(int);
typedef void ABI (*Builtin)(RV *,void *,void *,int,RV *);
typedef struct { const char *name; Builtin call; int argc; } Function;
typedef struct Trace { struct Trace *previous; const char *name; int line; } Trace;
enum { CLOSE,VX,VY,DRAW,DIST,RAND1,RAND2,ALPHA,ALPHA_SCALE,SCALE,FR,
       X,Y,IMAGE_ALPHA,XSCALE,YSCALE,LEADER,LEFT,RIGHT,XVIEW,WVIEW,NVARS };
static const char *const names[NVARS]={"closewindow","vx","vy","draw","dist",
 "rand1","rand2","alpha","alpha_scale","scale","fr","x","y",
 "image_alpha","image_xscale","image_yscale","leader","leftside","rightside","xview","wview"};
static const uint32_t *vars[NVARS];
Event dustspawner_loop_native_original;
static void **globals;
static const void *builtin_variables,*instance_hash;
static const int *builtin_start,*function_count,*managed_guard;
static const uint8_t *profile,*managed;
static const double *epsilon;
static Function **functions,**current_function;
static const uint32_t *distance_descriptor;
static Trace **trace_start;
static bool installed;
unsigned dustspawner_loop_native_hits,dustspawner_loop_native_fallbacks;
unsigned dustspawner_loop_native_refreshes,dustspawner_loop_native_paused,dustspawner_loop_native_draws;
enum { PROFILE,HELPER,TABLE,IDS,MISSING,KIND,BUILTIN,INSTANCE,CHILD,REGISTRY,
       ALIAS,MANAGED,RANGE,PAUSED,EVENT_STATE,ROOM,DEPTH,TAIL,NREASONS };
unsigned dustspawner_loop_native_reject_count[NREASONS];
static int reject(unsigned reason) { ++dustspawner_loop_native_reject_count[reason]; return 0; }
#define REQUIRE(c,r) do { if(!(c)) return reject(r); } while(0)
static uint32_t word(const void *p,size_t off) {
 uint32_t v; memcpy(&v,(const uint8_t *)p+off,4); return v;
}
static void *ptr(const void *p,size_t off) { return (void *)(uintptr_t)word(p,off); }
static float field(const void *p,size_t off) {
 float v; memcpy(&v,(const uint8_t *)p+off,4); return v;
}
static int id(int n) { return (int)vars[n][1]; }
enum { H_GET,H_REF,H_BOOL,H_COMPARE,H_SET,H_VALUE,H_GET_VALUE,H_ERR_VALUE,H_FREE,
 H_PUSH,H_POP,H_XGET,H_YGET,H_XSET,H_YSET,H_AGET,H_ASET,H_XSGET,H_XSSET,
 H_YSGET,H_YSSET,H_DISTANCE,H_DRAW,H_ADD,H_SUB,H_MUL,H_DIV,H_NEAREST,H_ITER,H_ITEREX,H_NEXT,H_DELETE,H_EVENT,H_PERFORM,H_ASYNC,H_EFFECTIVE,H_EXEC,H_CODE,H_EXECIT,H_PARTICLE,H_ASSIGN,H_POSITION,H_SKELETON,H_DIRTY,H_SCALEX,H_SCALEY,H_SPRITE_EXISTS,H_SPRITE_DATA,H_IMAGE_INDEX,H_RENDER,H_COMMAND_NEAREST,NHELPERS };
typedef struct { const char *name; uint32_t offset,a,b; uintptr_t address; uintptr_t *got; } Helper;
/* The verifier checks these against the APK, not a Python GML model. */
static Helper helpers[NHELPERS]={
 {"_ZN12YYObjectBase19InternalGetYYVarRefEi",0xf21708,0xe92d4df0,0xe28db018,0,NULL},
 {"_ZN12YYObjectBase20InternalGetYYVarRefLEi",0xf219b0,0xe92d4bf0,0xe28db018,0,NULL},
 {"_Z11BOOL_RValuePK6RValue",0xec9c5c,0xe92d4c10,0xe28db008,0,NULL},
 {"_Z12YYCompareValRK6RValueS1_db",0xe3e8f8,0xe92d4970,0xe28db010,0,NULL},
 {"_Z24Variable_SetValue_DirectP12YYObjectBaseiiP6RValue",0xf8936c,0xe92d4df0,0xe28db018,0,NULL},
 {"_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb",0xf89eb0,0xe92d4ef0,0xe28db018,0,NULL},
 {"_Z10GET_RValueP6RValueS0_P12YYObjectBaseibb",0xec8590,0xe92d4ff0,0xe28db01c,0,NULL},
 {"_Z32YYGML_ErrCheck_Variable_GetValueRK8YYRValueiiP6RValueP9CInstanceS5_",0xf8a7fc,0xe92d4df0,0xe28db018,0,NULL},
 {"_Z16FREE_RValue__PreP6RValue",0xba7758,0xe590100c,0xe3c114ff,0,NULL},
 {"_Z16PushContextStackP12YYObjectBase",0xecc284,0xe92d48f0,0xe28db010,0,NULL},
 {"_Z15PopContextStacki",0xecc33c,0xe59f1014,0xe79f1001,0,NULL},
 {"_Z4GV_XP9CInstanceiP6RValue",0xf844d4,0xed900a2e,0xe3a00000,0,NULL},
 {"_Z4GV_YP9CInstanceiP6RValue",0xf844f0,0xed900a2f,0xe3a00000,0,NULL},
 {"_Z4SV_XP9CInstanceiP6RValue",0xf84e6c,0xe92d4c10,0xe28db008,0,NULL},
 {"_Z4SV_YP9CInstanceiP6RValue",0xf84ebc,0xe92d4c10,0xe28db008,0,NULL},
 {"_Z13GV_ImageAlphaP9CInstanceiP6RValue",0xf84b48,0xed900a2c,0xe3a00000,0,NULL},
 {"_Z13SV_ImageAlphaP9CInstanceiP6RValue",0xf856c4,0xe92d4c10,0xe28db008,0,NULL},
 {"_Z14GV_ImageXScaleP9CInstanceiP6RValue",0xf84af4,0xed900a29,0xe3a00000,0,NULL},
 {"_Z14SV_ImageXScaleP9CInstanceiP6RValue",0xf855e0,0xe92d4c10,0xe28db008,0,NULL},
 {"_Z14GV_ImageYScaleP9CInstanceiP6RValue",0xf84b10,0xed900a2a,0xe3a00000,0,NULL},
 {"_Z14SV_ImageYScaleP9CInstanceiP6RValue",0xf8562c,0xe92d4c10,0xe28db008,0,NULL},
 {"_Z15F_PointDistanceR6RValueP9CInstanceS2_iPS_",0xdf9cac,0xe92d48f0,0xe28db010,0,NULL},
 {"_Z15YYGML_draw_selfP9CInstance",0xdde728,0xe92d4ff0,0xe28db01c,0,NULL},
 {"_ZN8YYRValuepLERKS_",0xbaed0c,0xe92d4830,0xe28db008,0,NULL},
 {"_ZN8YYRValuemIERKS_",0xbaeb4c,0xe92d4830,0xe28db008,0,NULL},
 {"_ZN8YYRValuemLERKS_",0xbae6ac,0xe92d48f0,0xe28db010,0,NULL},
 {"_ZN8YYRValuedVERKS_",0xbdff50,0xe92d4830,0xe28db008,0,NULL},
 {"_Z17F_InstanceNearestR6RValueP9CInstanceS2_iPS_",0xdd1fc4,0xe92d4df0,0xe28db018,0,NULL},
 {"_Z21YYGML_NewWithIteratorP13SWithIteratorPP12YYObjectBaseS3_RK8YYRValue",0xee0188,0xe92d4bf0,0xe28db018,0,NULL},
 {"_Z23YYGML_NewWithIteratorExP13SWithIteratorPP12YYObjectBaseS3_i",0xedfcb8,0xe92d4ff0,0xe28db01c,0,NULL},
 {"_Z22YYGML_WithIteratorNextP13SWithIteratorPP12YYObjectBaseS3_",0xee04b8,0xe5903008,0xe1a02000,0,NULL},
 {"_Z24YYGML_DeleteWithIteratorP13SWithIteratorPP12YYObjectBaseS3_",0xee04ec,0xe92d48f0,0xe28db010,0,NULL},
 {"_Z16YYGML_event_userP9CInstanceS0_i",0xdfe960,0xe282300a,0xe3a02007,0,NULL},
 {"_Z13Perform_EventP9CInstanceS0_ii",0xf363f8,0xe92d4df0,0xe28db018,0,NULL},
 {"_Z26Perform_Event_Object_ASyncP9CInstanceS0_iii",0xf361c8,0xe92d4ff0,0xe28db01c,0,NULL},
 {"_ZN9CObjectGM17GetEventRecursiveEii",0xd447e8,0xe92d4ff0,0xe28db01c,0,NULL},
 {"_ZN6CEvent7ExecuteEP9CInstanceS1_",0xf35f44,0xe92d4830,0xe28db008,0,NULL},
 {"_Z12Code_ExecuteP9CInstanceS0_P5CCodeP6RValuei",0xeca178,0xe92d4800,0xe1a0b00d,0,NULL},
 {"_Z9ExecuteItP9CInstanceS0_P5CCodeP6RValuei",0xec9fcc,0xe92d4bf0,0xe28db018,0,NULL},
 {"_Z36gml_Object_obj_dustparticle_Other_10P9CInstanceS0_",0x3d0fb28,0xe92d4ff0,0xe28db01c,0,NULL},
 {"_ZN8YYRValueaSERKS_",0xbdf4c8,0xe92d4c70,0xe28db010,0,NULL},
 {"_ZN9CInstance11SetPositionEff",0xf297d4,0xe92d4c10,0xe28db008,0,NULL},
 {"_ZN9CInstance17SkeletonAnimationEv",0xf2673c,0xe92d4830,0xe28db008,0,NULL},
 {"_Z18CollisionMarkDirtyP9CInstance",0xf6bf40,0xe5901068,0xe3510000,0,NULL},
 {"_ZN9CInstance14SetImageScaleXEf",0xf27150,0xee001a10,0xed901a29,0,NULL},
 {"_ZN9CInstance14SetImageScaleYEf",0xf271d8,0xee001a10,0xed901a2a,0,NULL},
 {"_Z13Sprite_Existsi",0xd6ec70,0xe3500000,0x4a00000c,0,NULL},
 {"_Z11Sprite_Datai",0xd6ef90,0xe1a01000,0xe3a00000,0,NULL},
 {"_ZN9CInstance13GetImageIndexEv",0xf26f34,0xe92d4830,0xe28db008,0,NULL},
 {"_ZN7CSprite4DrawEffffffjf",0xd615b0,0xe92d4ff0,0xe28db01c,0,NULL},
 {"_Z23Command_InstanceNearestffi",0xf65818,0xe92d4830,0xe28db008,0,NULL}
};
static int helpers_ok(void) {
 for(int i=0;i<NHELPERS;++i) {
  Helper *h=helpers+i;
  REQUIRE(word((void *)h->address,0)==h->a&&word((void *)h->address,4)==h->b,HELPER);
  REQUIRE(!h->got||*h->got==h->address,HELPER);
 }
 return 1;
}
typedef struct { const uint8_t *entries; uint32_t mask; } Table;
static inline __attribute__((always_inline)) int table_check(void *owner,Table *t,bool writable) {
 if(!owner||word(owner,4)) return 0;
 const void *vt=ptr(owner,0);
 if(!vt||word(vt,8)!=helpers[H_GET].address||
    (writable&&word(vt,12)!=helpers[H_REF].address)) return 0;
 const void *ht=ptr(owner,0x24);if(!ht) return 0;
 t->mask=word(ht,8); t->entries=ptr(ht,16);
 return t->entries&&t->mask<=65535&&word(ht,0)==t->mask+1&&!(t->mask&(t->mask+1));
}
static inline __attribute__((always_inline)) int table(void *owner,Table *t,bool writable) {
 REQUIRE(table_check(owner,t,writable),TABLE);
 return 1;
}
static inline __attribute__((always_inline)) RV *peek(const Table *t,int n) {
 uint32_t key=(vars[n][1]+1)&0x7fffffff,i=key&t->mask;
 if(!key) return NULL;
 for(uint32_t d=0;d<=t->mask;++d,i=(i+1)&t->mask) {
  const void *e=t->entries+12*i; uint32_t k=word(e,8);
  if(k==key) return ptr(e,0);
  if(!k||d>((i-(k&t->mask))&t->mask)) break;
 }
 return NULL;
}
static int local(int n) { return id(n)>=0&&(id(n)>9999||id(n)>=*builtin_start); }
static int numeric(const RV *v,bool boolean) {
 return v&&(v->kind==0||(boolean&&v->kind==13))&&isfinite(v->value)&&fabs(v->value)<=1048576;
}
static inline __attribute__((always_inline)) int builtin_ok(int n,int getter,int setter) {
 REQUIRE(id(n)>=0&&id(n)<=9999&&id(n)<*builtin_start,IDS);
 const uint8_t *b=(const uint8_t *)builtin_variables+16*id(n);
 REQUIRE(word(b,4)==helpers[getter].address&&word(b,8)==helpers[setter].address&&b[12],BUILTIN);
 return 1;
}
static int live(void *p,uint32_t ident) {
 return p&&word(p,0x50)==1&&word(p,0x80)==ident&&(word(p,0x7c)&0x100007)==4;
}
static void *instance(const RV *v) {
 uint32_t ident;
 if(v->kind==15&&word(v,4)==0x04000001) ident=word(v,0);
 else {
  if(!numeric(v,false)||v->value<100000||v->value>1048576||v->value!=(int)v->value) return NULL;
  ident=(uint32_t)v->value;
 }
 if(ident<100000||ident>2147483647) return NULL;
 const void *buckets=ptr(instance_hash,0); uint32_t mask=word(instance_hash,4);
 if(!buckets||mask>1048575||(mask&(mask+1))) return NULL;
 const void *node=ptr(buckets,8*(ident&mask));
 for(unsigned i=0;node&&i<4096;node=ptr(node,4),++i)
  if(word(node,8)==ident) { void *p=ptr(node,12); return live(p,ident)?p:NULL; }
 return NULL;
}

typedef int ABI (*NewIterator)(void *,void **,void **,const RV *);
typedef int ABI (*NextIterator)(void *,void **,void **);
typedef void ABI (*DeleteIterator)(void *,void **,void **);
typedef RV * ABI (*GetRef)(void *,int);
typedef RV * ABI (*Assign)(RV *,const RV *);
typedef void ABI (*ValueGet)(const RV *,int,int,RV *,bool,bool);
typedef void ABI (*UserEvent)(void *,void *,int);
typedef void * ABI (*Effective)(void *,int,int);
typedef struct { void *old_self,*old_other,*array,*cursor; } Iterator;
static const uint32_t *nearest_descriptor;
static void **object_hash,**run_room;
static int *new_room,*event_object,*event_type,*event_number,*event_depth;
static uint8_t *argument_relative,*code_error;
static const char *spawner_name,*particle_name;
unsigned dustspawner_loop_native_calls,dustspawner_loop_native_loops;
unsigned dustspawner_loop_native_passes,dustspawner_loop_native_particle_fallbacks;
unsigned dustspawner_loop_native_whole_fallbacks;
typedef struct { RV *close,*leader,*left,*right,*fr,*xview,*wview; void *dust; } State;

/* Look up the canonical object. Actual iteration remains the runner's work. */
static void *object(int ident) {
 const void *hash=*object_hash;
 if(!hash) return NULL;
 const void *buckets=ptr(hash,0); uint32_t mask=word(hash,4);
 if(!buckets||mask>1048575||(mask&(mask+1))) return NULL;
 const void *node=ptr(buckets,8*((uint32_t)ident&mask));
 for(unsigned i=0;node&&i<4096;node=ptr(node,4),++i)
  if(word(node,8)==(uint32_t)ident) return ptr(node,12);
 return NULL;
}
static int plain(const RV *v) { return v&&(v->kind==0||v->kind==13||v->kind==15); }
static int coordinates(void *p) {
 return isfinite(field(p,0xb8))&&isfinite(field(p,0xbc))&&
        fabs(field(p,0xb8))<=1048576&&fabs(field(p,0xbc))<=1048576;
}
static int global_admit(void *self,State *s) {
 Table g,t;
 REQUIRE(!*profile,PROFILE);
 if(!helpers_ok()) return 0;
 REQUIRE(*builtin_start>0&&*builtin_start<=10000,IDS);
 REQUIRE(isfinite(*epsilon)&&*epsilon>=0&&*epsilon<=1,RANGE);
 for(int i=0;i<NVARS;++i) {
  bool is_local=i<=FR||i>=LEADER;
  if(is_local) REQUIRE(local(i),IDS);
  for(int j=0;j<i;++j) REQUIRE(id(i)!=id(j),ALIAS);
 }
 if(!builtin_ok(X,H_XGET,H_XSET)||!builtin_ok(Y,H_YGET,H_YSET)||
    !builtin_ok(IMAGE_ALPHA,H_AGET,H_ASET)||!builtin_ok(XSCALE,H_XSGET,H_XSSET)||
    !builtin_ok(YSCALE,H_YSGET,H_YSSET)) return 0;
 REQUIRE(self&&word(self,0x50)==1&&(word(self,0x7c)&0x100007)==4,INSTANCE);
 if(!table(self,&t,true)||!table(*globals,&g,false)) return 0;
 s->close=peek(&t,CLOSE);s->leader=peek(&t,LEADER);s->left=peek(&t,LEFT);s->right=peek(&t,RIGHT);
 s->fr=peek(&g,FR);s->xview=peek(&g,XVIEW);s->wview=peek(&g,WVIEW);
 RV *slots[]={s->close,s->leader,s->left,s->right,s->fr,s->xview,s->wview};
 for(unsigned i=0;i<7;++i) {
  REQUIRE(slots[i],MISSING);
  for(unsigned j=0;j<i;++j) REQUIRE(slots[i]!=slots[j],ALIAS);
 }
 REQUIRE(plain(s->close)&&numeric(s->leader,true)&&numeric(s->left,false)&&numeric(s->right,false)&&
         numeric(s->fr,false)&&numeric(s->xview,false)&&numeric(s->wview,false),KIND);
 if(fabs(s->fr->value)<=*epsilon) { ++dustspawner_loop_native_paused;return reject(PAUSED); }
 REQUIRE(coordinates(self)&&isfinite(field(self,0xa4))&&fabs(field(self,0xa4))<=1048576&&
         fabs((double)field(self,0xb8)+(double)field(self,0xa4)*32)<=1048576&&
         fabs(s->xview->value+s->wview->value)<=2097152,RANGE);
 REQUIRE(*functions&&*function_count>0&&*function_count<=16384&&
         distance_descriptor[1]<(uint32_t)*function_count&&nearest_descriptor[1]<(uint32_t)*function_count&&
         (*functions)[distance_descriptor[1]].call==(Builtin)helpers[H_DISTANCE].address&&
         (*functions)[nearest_descriptor[1]].call==(Builtin)helpers[H_NEAREST].address,REGISTRY);
 REQUIRE(*run_room&&*new_room==-1,ROOM);
 REQUIRE(*event_depth>=0&&*event_depth<32,DEPTH);
 s->dust=object(0x170);REQUIRE(s->dust&&word(s->dust,0x64)==0x170,EVENT_STATE);
 void *e=((Effective)helpers[H_EFFECTIVE].address)(s->dust,7,10);
 REQUIRE(e&&word(e,4)==0x170,EVENT_STATE);
 const void *code=ptr(e,0);REQUIRE(code&&(word(code,8)==1||word(code,8)==2),EVENT_STATE);
 const void *fn=ptr(code,0x64);
 REQUIRE(fn&&word(fn,4)==helpers[H_PARTICLE].address,EVENT_STATE);
 REQUIRE(!(*managed&&*managed_guard>=1&&(word(s->dust,0x50)&0x80)),MANAGED);
 return 1;
}
/* Only particle-local properties are checked here. No getter or write occurs. */
static int particle_admit(void *p,void *spawner,const State *s,RV **v,void *window,bool window_table) {
 Table t;
 REQUIRE(p&&p!=spawner&&p!=*globals&&word(p,0x50)==1&&(word(p,0x7c)&0x100007)==4,INSTANCE);
 REQUIRE(ptr(p,0x68)==s->dust&&word(p,0x84)==0x170,CHILD);
 if(!table(p,&t,true)) return 0;
 for(int i=0;i<FR;++i) {
  v[i]=peek(&t,i);REQUIRE(v[i],MISSING);
  REQUIRE(i<=VY?plain(v[i]):numeric(v[i],i==DRAW),KIND);
  for(int j=0;j<i;++j) REQUIRE(v[i]!=v[j],ALIAS);
  REQUIRE(v[i]!=s->close&&v[i]!=s->leader&&v[i]!=s->left&&v[i]!=s->right&&
          v[i]!=s->fr&&v[i]!=s->xview&&v[i]!=s->wview,ALIAS);
 }
 v[FR]=s->fr;
 REQUIRE(window&&live(window,word(window,0x80)),INSTANCE);
 REQUIRE(window_table,TABLE);
 REQUIRE(coordinates(p)&&coordinates(window),RANGE);
 REQUIRE(isfinite((float)((0.6+v[RAND2]->value*0.4)*v[SCALE]->value))&&
         isfinite((float)(((0.8+v[RAND1]->value*0.2)*v[ALPHA]->value)*v[ALPHA_SCALE]->value)),RANGE);
 return 1;
}

static RV real(double d) { RV v={d,0,0}; return v; }
static void set(void *self,int n,RV *v) {
 ((Setter)helpers[H_SET].address)(self,id(n),(int)0x80000000,v);
}
static int particle(void *self,void *other,RV **v,void *window) {
 Trace trace={*trace_start,particle_name,3}; *trace_start=&trace;
 RV x=real((double)field(self,0xb8)),y=real((double)field(self,0xbc));
 /* op_addeq retains the left operand's flags. SV_X/Y round to float. */
 RV moved=*v[VX]; moved.value+=x.value; trace.line=6; set(self,X,&moved);
 moved=*v[VY]; moved.value+=y.value; trace.line=7; set(self,Y,&moved);
 {
  ++dustspawner_loop_native_refreshes;
  trace.line=10;
  RV draw={0,v[VY]->flags,13}; *v[DRAW]=draw;
  trace.line=11;
  RV args[4]={real((double)field(self,0xb8)),real((double)field(self,0xbc)),
              real((double)field(window,0xb8)),real((double)field(window,0xbc))};
  RV distance={0,0,5}; Function *saved=*current_function;
  *current_function=*functions+distance_descriptor[1];
  ((Builtin)helpers[H_DISTANCE].address)(&distance,self,other,4,args);
  *current_function=saved;
  ((Context)helpers[H_PUSH].address)(self); *v[DIST]=distance;
  ((Pop)helpers[H_POP].address)(1);
  /* Bounded finite REAL operands use YYCompareVal's subtraction/epsilon arm.
   * Equality includes abs(delta) == epsilon, and is outside the light radius. */
  double delta=v[DIST]->value-250;
  RV a=real(0);
  if(delta < 0&&fabs(delta)>*epsilon) {
   trace.line=14; draw.value=1; *v[DRAW]=draw;
   trace.line=15;
   a.value=(((0.8+v[RAND1]->value*0.2)*(1-v[DIST]->value/250))*v[ALPHA]->value)*v[ALPHA_SCALE]->value;
  } else trace.line=19;
  set(self,IMAGE_ALPHA,&a);
  trace.line=22; a.value=(0.6+v[RAND2]->value*0.4)*v[SCALE]->value; set(self,XSCALE,&a);
  trace.line=23; a.value=(double)field(self,0xa4); set(self,YSCALE,&a);
 }
 trace.line=26;
 if(v[DRAW]->value>0.5) {
  trace.line=28; ((Context)helpers[H_DRAW].address)(self); ++dustspawner_loop_native_draws;
 }
 trace.line=32; set(self,X,&x); trace.line=33; set(self,Y,&y);
 *trace_start=trace.previous; return 1;
}

static void copy_context(void *self,RV *dst,const RV *src) {
 ((Context)helpers[H_PUSH].address)(self);
 if(dst!=src) *dst=*src;
 ((Pop)helpers[H_POP].address)(1);
}
static uintptr_t target(int h) { return helpers[h].got?*helpers[h].got:helpers[h].address; }
static RV *get_local(void *self,int n,bool write) {
 const void *vt=ptr(self,0);
 return ((GetRef)(uintptr_t)word(vt,write?12:8))(self,id(n));
}
static void generic_copy(void *self,RV *dst,const RV *src) {
 ((Context)target(H_PUSH))(self);
 ((Assign)target(H_ASSIGN))(dst,src);
 ((Pop)target(H_POP))(1);
}
/* Preserve missing-variable creation, refcount handling and dynamic vtables on
 * the slow tail. Once arbitrary GML runs, do not reuse cached particle slots. */
static void generic_assignments(void *p,void *spawner,Trace *trace,uint32_t xflags,uint32_t yflags) {
 (void)get_local(p,CLOSE,false);
 trace->line=12;
 RV *dst=get_local(p,CLOSE,true),*src=get_local(spawner,CLOSE,false);
 generic_copy(p,dst,src);
 RV owner={0,0,6};memcpy(&owner.value,&spawner,4);
 RV coordinate={0,xflags,0xffffff};
 trace->line=13;dst=get_local(p,VX,true);
 ((ValueGet)target(H_VALUE))(&owner,id(X),(int)0x80000000,&coordinate,false,false);
 generic_copy(p,dst,&coordinate);
 trace->line=14;dst=get_local(p,VY,true);coordinate.flags=yflags;
 ((ValueGet)target(H_VALUE))(&owner,id(Y),(int)0x80000000,&coordinate,false,false);
 generic_copy(p,dst,&coordinate);
}
static void native_event(void *p,void *other,RV **v,void *window) {
 int saved_object=*event_object,saved_type=*event_type,saved_number=*event_number;
 uint8_t relative=*argument_relative;
 ++*event_depth;
 *event_type=7;*event_object=0x170;*event_number=10;
 *argument_relative=0;*code_error=0;
 particle(p,other,v,window);
 *argument_relative=relative;
 *event_type=saved_type;*event_object=saved_object;*event_number=saved_number;
 --*event_depth;
}
static int greater(double a,double b) { double d=a-b;return d>0&&fabs(d)>*epsilon; }
static int less(double a,double b) { double d=a-b;return d<0&&fabs(d)>*epsilon; }
int ABI __attribute__((noinline,used)) dustspawner_loop_native_try(void *self,void *other,const uint8_t *scratch) {
 ++dustspawner_loop_native_calls;
 State state;
 if(!global_admit(self,&state)) {
  ++dustspawner_loop_native_fallbacks;++dustspawner_loop_native_whole_fallbacks;return 0;
 }
 ++dustspawner_loop_native_hits;
 Trace trace={*trace_start,spawner_name,2};*trace_start=&trace;
 if(state.leader->value>0.5) { *trace_start=trace.previous;return 1; }
 trace.line=4;
 RV args[3]={real((double)field(self,0xb8)),real((double)field(self,0xbc)),{0,0,15}};
 uint32_t asset[2]={0x420,0x01000000};memcpy(&args[2].value,asset,8);
 RV closest={0,0,5};Function *saved=*current_function;
 *current_function=*functions+nearest_descriptor[1];
 ((Builtin)helpers[H_NEAREST].address)(&closest,self,other,3,args);
 *current_function=saved;
 copy_context(self,state.close,&closest);
 /* GV_X/Y leave the original spawner temporaries' flags untouched. Those
  * stale flags are copied into persistent locals and must be retained. */
 uint32_t xflags=word(scratch-0x98,0),yflags=word(scratch-0xa8,0);
 trace.line=5;RV left=real((double)field(self,0xb8));left.flags=xflags;copy_context(self,state.left,&left);
 trace.line=6;RV right=left;right.value+=(double)field(self,0xa4)*32;copy_context(self,state.right,&right);
 trace.line=8;
 double begin=state.xview->value,end=begin+state.wview->value;
 if(!((greater(left.value,begin)&&less(left.value,end))||
      (greater(right.value,begin)&&less(right.value,end)))) {
  *trace_start=trace.previous;return 1;
 }
 ++dustspawner_loop_native_loops;
 trace.line=10;Iterator it;void *p=self,*with_other=other;
 RV object_ref={0,0,15};asset[0]=0x170;memcpy(&object_ref.value,asset,8);
 int present=((NewIterator)helpers[H_ITER].address)(&it,&p,&with_other,&object_ref);
 void *window=instance(&closest);Table window_t;
 bool window_table=table_check(window,&window_t,false),slow=false;
 while(present>0) {
  ++dustspawner_loop_native_passes;
  RV *v[FR+1];
  if(!slow&&particle_admit(p,self,&state,v,window,window_table)) {
   trace.line=12;copy_context(p,v[CLOSE],state.close);
   trace.line=13;RV x=real((double)field(self,0xb8));x.flags=xflags;copy_context(p,v[VX],&x);
   trace.line=14;RV y=real((double)field(self,0xbc));y.flags=yflags;copy_context(p,v[VY],&y);
   trace.line=15;native_event(p,with_other,v,window);
  } else {
   if(slow) ++dustspawner_loop_native_reject_count[TAIL];
   slow=true;
   generic_assignments(p,self,&trace,xflags,yflags);
   trace.line=15;((UserEvent)target(H_EVENT))(p,with_other,0);
   ++dustspawner_loop_native_fallbacks;++dustspawner_loop_native_particle_fallbacks;
  }
  present=((NextIterator)target(H_NEXT))(&it,&p,&with_other);
 }
 ((DeleteIterator)target(H_DELETE))(&it,&p,&with_other);
 *trace_start=trace.previous;return 1;
}
/* The original frame is 0x128 bytes. Admission and its callees stay below it. */
void ABI __attribute__((naked,target("thumb"))) dustspawner_loop_native_event(void *self __attribute__((unused)),void *other __attribute__((unused))) {
 __asm__ volatile("mov r2,sp\n" "sub.w sp,sp,#0x160\n" "push {r0,r1,r4,lr}\n"
  "bl dustspawner_loop_native_try\n" "mov ip,r0\n" "pop {r0,r1,r4,lr}\n"
  "add.w sp,sp,#0x160\n" "cmp ip,#0\n" "bne 1f\n"
  "movw ip,:lower16:dustspawner_loop_native_original\n"
  "movt ip,:upper16:dustspawner_loop_native_original\n" "ldr ip,[ip]\n" "bx ip\n" "1: bx lr\n");
}
int dustspawner_loop_native_install(void *(*rx_alloc)(size_t)) {
 if(installed||!rx_alloc) return 0;
#define RESOLVE(v,t,n) do { v=(t)so_symbol(&yoyoloader_mod,n);if(!v) return 0; } while(0)
 uintptr_t entry;
 RESOLVE(entry,uintptr_t,"_Z43gml_Object_obj_cathedral_dustspawner_Draw_0P9CInstanceS0_");
 if((entry&3)||entry<0x36810f4||word((void *)entry,0)!=0xe92d4ff0||word((void *)entry,4)!=0xe28db01c) return 0;
 uintptr_t bias=entry-0x36810f4;if(bias>UINTPTR_MAX-0x6000000) return 0;
 for(int i=0;i<NHELPERS;++i) {
  Helper *h=helpers+i;RESOLVE(h->address,uintptr_t,h->name);
  if(h->address!=bias+h->offset||word((void *)h->address,0)!=h->a||word((void *)h->address,4)!=h->b) return 0;
  h->got=so_got_slot(&yoyoloader_mod,h->name);if(h->got&&*h->got!=h->address) return 0;
 }
 RESOLVE(globals,void **,"g_pGlobal");RESOLVE(trace_start,Trace **,"_ZN13SYYStackTrace8s_pStartE");
 RESOLVE(profile,const uint8_t *,"g_bProfile");RESOLVE(epsilon,const double *,"g_GMLMathEpsilon");
 RESOLVE(managed,const uint8_t *,"g_enableManagedObjects");RESOLVE(managed_guard,const int *,"g_nManagedObjectGuard");
 RESOLVE(builtin_start,const int *,"g_nStartGlobalVariables");RESOLVE(builtin_variables,const void *,"builtin_variables");
 RESOLVE(instance_hash,const void *,"_ZN9CInstance14ms_ID2InstanceE");
 RESOLVE(functions,Function **,"the_functions");RESOLVE(current_function,Function **,"g_pFunction");
 RESOLVE(function_count,const int *,"the_numb");RESOLVE(distance_descriptor,const uint32_t *,"g_FUNC_point_distance");
 RESOLVE(nearest_descriptor,const uint32_t *,"g_FUNC_instance_nearest");
 RESOLVE(object_hash,void **,"g_ObjectHash");RESOLVE(run_room,void **,"Run_Room");RESOLVE(new_room,int *,"New_Room");
 RESOLVE(event_object,int *,"Current_Object");RESOLVE(event_type,int *,"Current_Event_Type");
 RESOLVE(event_number,int *,"Current_Event_Number");RESOLVE(argument_relative,uint8_t *,"Argument_Relative");
 RESOLVE(code_error,uint8_t *,"Code_Error_Occured");
 event_depth=(int *)(bias+0x4e5f520);
 spawner_name=(const char *)(bias+0x8eacdd);particle_name=(const char *)(bias+0x89db17);
 char symbol[64];
 for(int i=0;i<NVARS;++i) { strcpy(symbol,"g_VAR_");strcat(symbol,names[i]);RESOLVE(vars[i],const uint32_t *,symbol); }
#undef RESOLVE
 uint8_t *rx=rx_alloc(16);
 if(!rx||((uintptr_t)rx&3)||(uintptr_t)rx>UINTPTR_MAX-16||
    ((uintptr_t)rx<bias+0x6000000&&(uintptr_t)rx+16>bias)) return 0;
 uint32_t code[]={0xe92d4ff0,0xe28db01c,0xe51ff004,(uint32_t)(entry+8)};
 kuKernelCpuUnrestrictedMemcpy(rx,code,sizeof(code));kuKernelFlushCaches(rx,16);
 dustspawner_loop_native_original=(Event)rx;
 hook_addr(entry,(uintptr_t)dustspawner_loop_native_event);kuKernelFlushCaches((void *)entry,8);
 installed=true;return 1;
}
