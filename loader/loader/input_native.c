/* APK 1.0.10, ARM AAPCS, install after runner builtin registration. */
#include <string.h>
#include "main.h"
#include <stdint.h>
#include <math.h>
#include <kubridge.h>

#define ABI __attribute__((pcs("aapcs")))
typedef struct CInstance CInstance;
/* YYC also passes stack RValues at addresses congruent to 4 modulo 8. */
typedef struct __attribute__((packed, aligned(4))) { union { double d; uint64_t bits; uint32_t w[2]; } v; uint32_t flags, kind; } RV;
typedef RV *ABI (*Script)(CInstance *, CInstance *, RV *, int, RV **);
typedef RV *ABI (*Read)(void *, int);
typedef int ABI (*Compare)(const RV *, const RV *, double, int);
typedef int ABI (*Bool)(const RV *);
typedef RV *ABI (*Legacy)(CInstance *, CInstance *, RV *, int, int, RV **);
typedef int ABI (*Get)(const RV *, int, int, RV *, int, int);
typedef void ABI (*Free)(RV *);
typedef float ABI (*Direction)(float, float, float, float);
typedef double ABI (*Real)(const RV *);
typedef struct Trace { struct Trace *previous; const char *name; int line; } Trace;
typedef struct { const char *name; void *call; int argc; } Function;
typedef struct { uint32_t line, count; } Rejection;
unsigned input_native_calls, input_native_hits, input_native_fallbacks;
unsigned input_native_bindings, input_native_key_visits;
Rejection input_native_rejections[512];
unsigned input_native_rejection_used, input_native_rejection_overflow;

static Script original;
static Read read_var;
static Compare compare;
static Bool truth;
static Legacy legacy;
static Get get;
static Free free_value;
static Direction direction;
static Real real_value;
static void **global, **object_hash;
static uint32_t *id_hash, *changes, *activation;
static Trace **trace_start;
static double *epsilon;
static Function **functions;
static int installed;
enum { THRESHOLD, PAUSE, LIST, PRESSED, ASSIGNED, POSX, POSY, BACK, CONTROL, SLOT,
       LL, LR, LU, LD, RL, RR, RU, RD, NV };
static const char *const variable_names[NV] = {
 "controller_threshold", "key_pause", "vKeyList", "isPressed", "assignedKey",
 "posValueX", "posValueY", "backButtonPressed", "control", "controller_slot",
 "analogue_left_left", "analogue_left_right", "analogue_left_up", "analogue_left_down",
 "analogue_right_left", "analogue_right_right", "analogue_right_up", "analogue_right_down" };
static const uint32_t *variables[NV];
enum { EXISTS, ANGLE, KEYBOARD, MOUSE, GAMEPAD, NF };
static const char *const function_names[NF] = {
 "instance_exists", "angle_difference", "keyboard_check_pressed",
 "mouse_check_button_pressed", "gamepad_button_check_pressed" };
static const char *const builtin_symbols[NF] = {
 "_Z16F_InstanceExistsR6RValueP9CInstanceS2_iPS_",
 "_Z21F_MathAngleDifferenceR6RValueP9CInstanceS2_iPS_",
 "_Z17F_CheckKeyPressedR6RValueP9CInstanceS2_iPS_",
 "_Z25F_CheckMouseButtonPressedR6RValueP9CInstanceS2_iPS_",
 "_Z27F_GamepadButtonCheckPressedR6RValueP9CInstanceS2_iPS_" };
static const uint32_t *descriptors[NF];
static void *builtins[NF];
static const char *script_name;

static uint32_t word(const void *p, unsigned offset) {
 uint32_t v; memcpy(&v, (const char *)p + offset, 4); return v;
}
static void reject(unsigned line) {
 unsigned i; ++input_native_fallbacks;
 for (i=0; i<input_native_rejection_used; ++i)
  if (input_native_rejections[i].line == line) { ++input_native_rejections[i].count; return; }
 if (i == 512) { ++input_native_rejection_overflow; return; }
 input_native_rejections[i].line=line; input_native_rejections[i].count=1;
 ++input_native_rejection_used;
}
static RV number(double d) { RV v = {{0},0,0}; v.v.d=d; return v; }
static RV reference(unsigned id) { RV v = {{0},0,15}; v.v.bits=UINT64_C(0x0100000000000000)|id; return v; }
static int scalar(const RV *v) {
 return v && (v->kind==0 || v->kind==7 || v->kind==13) &&
  (v->kind==7 || isfinite(v->v.d));
}
static int disjoint(const void *a, size_t n, const RV *b) {
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
 return x>=y+16 || y>=x+n;
}
static RV *slot(void *owner, int n) { return read_var(owner, (int)variables[n][1]); }
static int eq(const RV *a, double b) { RV v=number(b); return compare(a,&v,*epsilon,0)==0; }
static void *hash_find(const void *h, int id) {
 uintptr_t node; unsigned trips=0;
 if (!h || !word(h,0)) return NULL;
 node=word((void *)(uintptr_t)word(h,0), ((unsigned)id & word(h,4))*8);
 while (node && trips++<4096) {
  if ((int)word((void *)node,8)==id) return (void *)(uintptr_t)word((void *)node,12);
  node=word((void *)node,4);
 }
 return NULL;
}
static int ordinary(void *p) {
 if (!p || word(p,0x50)!=1 || (word(p,0x7c)&0x100003)) return 0;
 return 1;
}
static void *object_first(int id) {
 void *obj=hash_find(*object_hash,id); uintptr_t node; unsigned trips=0;
 /* Original OBJTYPE lookup tests instance +0x7c, never CObjectGM +0x50. */
 if (!obj) return NULL;
 node=word(obj,0x44);
 while (node && trips++<4096) {
  void *p=(void *)(uintptr_t)word((void *)node,8);
  if (!p) return NULL;
  if (!(word(p,0x7c)&0x100003)) return ordinary(p)?p:NULL;
  node=word((void *)node,0);
 }
 return NULL;
}
static RV *array(const RV *v, unsigned *length) {
 void *h; int n;
 if (!v || v->kind!=2 || !v->v.w[0]) return NULL;
 h=(void *)(uintptr_t)v->v.w[0]; n=(int)word(h,28);
 if (n<0 || n>64 || (n && !word(h,4))) return NULL;
 *length=(unsigned)n; return (RV *)(uintptr_t)word(h,4);
}
static RV *call(CInstance *s, CInstance *o, RV *out, int f, int n, RV *a, RV *b) {
 RV *argv[2]={a,b}; out->v.w[0]=0; out->flags=0; out->kind=5;
 return legacy(s,o,out,n,(int)descriptors[f][1],argv);
}

RV *ABI input_native_pressed(CInstance *self, CInstance *other, RV *result, int argc, RV **argv) {
 unsigned n=UINT32_MAX, m=UINT32_MAX, pause_length=UINT32_MAX, i,j, line=0;
 RV *bindings, *list, *pause, *threshold, *control, *padslot, *back;
 RV pressed[64], assigned[64], analog[8], px={{0},0,0},py={{0},0,0};
 RV out={{0},0,5}, kb=reference(703), tmp={{0},0,0};
 void *keyboard, *controller, *joystick=NULL;
 Trace trace;
 int yes=0, direct_result=0;
 ++input_native_calls;
#define CHECK(c) do { if (!(c)) { line=__LINE__; goto fallback; } } while (0)
 CHECK(self && result && !((uintptr_t)result&3));
 CHECK(argc==1 && argv && argv[0]);
 CHECK(disjoint(argv[0],16,result));
 CHECK(result->kind==0 || result->kind==7 || result->kind==13 || result->kind==5 || result->kind==0xffffff);
 CHECK(*global && *functions && isfinite(*epsilon) && *epsilon>=0 && *epsilon<0.01);
 /* Resolve builtins through the current legacy registry, including late loader
  * wrappers. No pointer identity is used to select input behavior. */
 threshold=slot(*global,THRESHOLD); pause=slot(*global,PAUSE);
 CHECK(scalar(threshold) && pause && pause->kind==2);
 CHECK(disjoint(threshold,16,result) && disjoint(pause,16,result));
 { RV *p=array(pause,&pause_length);
   CHECK((p || pause_length==0) && pause->v.w[0] && disjoint((void *)(uintptr_t)pause->v.w[0],32,result));
   CHECK(!pause_length || disjoint(p,16*pause_length,result)); }
 bindings=array(argv[0],&n); CHECK(bindings || (argv[0]->kind==2 && n==0));
 CHECK(argv[0]->v.w[0] && disjoint((void *)(uintptr_t)argv[0]->v.w[0],32,result));
 for(i=0;i<n;++i) CHECK(scalar(&bindings[i]) && disjoint(&bindings[i],16,result));
 /* Pending changes can alter object-index resolution, including inheritance. */
 CHECK(changes[2]==0 && activation[2]==0);
 if (!n) { ++input_native_hits; result->v.d=0; result->kind=13; return result; }
 keyboard=object_first(703); controller=object_first(501);
 CHECK(keyboard && controller);
 CHECK(disjoint(slot(keyboard,LIST),16,result));
 list=array(slot(keyboard,LIST),&m); CHECK(list && m>=13 && disjoint(list,16*m,result));
 CHECK(disjoint((void *)(uintptr_t)slot(keyboard,LIST)->v.w[0],32,result));
 back=slot(keyboard,BACK); control=slot(controller,CONTROL); padslot=slot(controller,SLOT);
 CHECK(scalar(back) && scalar(control) && scalar(padslot));
 CHECK(disjoint(back,16,result) && disjoint(control,16,result) && disjoint(padslot,16,result));
 for (i=0;i<8;++i) { RV *v=slot(controller,LL+(int)i); CHECK(scalar(v) && disjoint(v,16,result)); analog[i]=*v; }
 for(j=0;j<m;++j) {
  int id; void *key; RV *p,*a;
  CHECK(list[j].kind==15 || list[j].kind==7 || list[j].kind==0);
  if (list[j].kind==0) { CHECK(isfinite(list[j].v.d) && list[j].v.d>=100000 && list[j].v.d<=2147483647); id=(int)list[j].v.d; }
  else id=(int)list[j].v.w[0];
  CHECK(id>=100000); key=hash_find(id_hash,id); CHECK(ordinary(key));
  p=slot(key,PRESSED); a=slot(key,ASSIGNED); CHECK(scalar(p) && scalar(a) && disjoint(p,16,result) && disjoint(a,16,result));
  pressed[j]=*p; assigned[j]=*a;
 }
 for(i=0;i<n;++i) if(eq(&bindings[i],32776) && truth(&pressed[0])) {
  joystick=object_first(880); CHECK(joystick);
  CHECK(scalar(slot(joystick,POSX)) && scalar(slot(joystick,POSY)));
  CHECK(disjoint(slot(joystick,POSX),16,result) && disjoint(slot(joystick,POSY),16,result));
  px=*slot(joystick,POSX); py=*slot(joystick,POSY); break;
 }
 /* Execute the first array GET at its original point. It can assign the
  * copy-on-write owner even though this script never writes an element. */
 trace.previous=*trace_start; trace.name=script_name; trace.line=8; *trace_start=&trace;
 ++input_native_hits;
 for(i=0;i<n;++i) {
  RV *key=&bindings[i]; ++input_native_bindings;
  trace.line=8; call(self,other,&out,EXISTS,1,&kb,NULL);
  if(truth(&out)) {
   if(eq(key,32776)) {
    trace.line=13; get(&kb,(int)variables[LIST][1],0,&tmp,0,0);
    if(truth(&pressed[0])) {
     RV delta=number(fabs(real_value(&py))-real_value(threshold)), eps=number(*epsilon);
     trace.line=17;
     double dir=(double)direction(0,0,(float)real_value(&px),(float)real_value(&py));
     RV direction_arg=number(dir);
     trace.line=18;
     if(compare(&delta,&eps,*epsilon,1)<1) { yes=1; break; }
     trace.line=18; tmp=number(270);
     call(self,other,&out,ANGLE,2,&tmp,&direction_arg);
     if(fabs(out.v.d)-100<=*epsilon) { yes=1; break; }
    }
   }
   trace.line=29;
   if(compare(argv[0],pause,*epsilon,0)==0) {
    if(truth(back)) { yes=1; break; }
    trace.line=32; get(&kb,(int)variables[LIST][1],12,&tmp,0,0);
    if(truth(&pressed[12])) {
     tmp=reference(474); call(self,other,&out,EXISTS,1,&tmp,NULL);
     if(!truth(&out)) { yes=1; break; }
    }
   }
   trace.line=40;
   /* Use the runner for ownership/refcount effects once per binding. */
   get(&kb,(int)variables[LIST][1],(int)0x80000000u,&tmp,0,0);
   free_value(&tmp);
   tmp.kind=5; tmp.flags=0; /* FREE_RValue__Pre does not clear an array tag. */
   for(j=0;j<m;++j) {
    ++input_native_key_visits;
    if(compare(&assigned[j],key,*epsilon,0)==0 && truth(&pressed[j])) { yes=1; break; }
   }
   if(yes) break;
  }
  if(eq(control,0)) {
   trace.line=51;
   if(eq(key,1)||eq(key,2)||eq(key,3)) {
    trace.line=53; call(self,other,&out,MOUSE,1,key,NULL); yes=truth(&out);
   } else if(eq(key,-100000)) {
    trace.line=60; tmp=number(1); call(self,other,&out,KEYBOARD,1,&tmp,NULL);
    direct_result=1; break;
   } else { trace.line=62; call(self,other,&out,KEYBOARD,1,key,NULL); yes=truth(&out); }
  } else {
   trace.line=95;
   for(j=0;j<8;++j) if(eq(key,j==7?99970:9990+(double)j)) break;
   if(j<8) { trace.line=97; yes=eq(&analog[j],1); }
   else { trace.line=107; call(self,other,&out,GAMEPAD,2,padslot,key); yes=truth(&out); }
  }
  if(yes) break;
 }
 if(direct_result) *result=out;
 else { result->v.d=yes?1:0; result->kind=13; }
 *trace_start=trace.previous;
 return result;
fallback:
 reject(line); return original(self,other,result,argc,argv);
#undef CHECK
}

int input_native_install(void *(*rx_alloc)(size_t)) {
 uintptr_t entry; uint32_t code[4]; void *rx; unsigned i; char name[96];
 if(installed || !rx_alloc) return 0;
#define RESOLVE(t,n) ((t)so_symbol(&yoyoloader_mod,n))
 entry=RESOLVE(uintptr_t,"_Z39gml_Script_controller_check_pressed_extP9CInstanceS0_R8YYRValueiPPS1_");
 if(!entry || (entry&3) || word((void *)entry,0)!=0xe92d4ff0 || word((void *)entry,4)!=0xe28db01c) return 0;
 read_var=RESOLVE(Read,"_ZN12YYObjectBase17InternalReadYYVarEi");
 compare=RESOLVE(Compare,"_Z12YYCompareValRK6RValueS1_db");
 truth=RESOLVE(Bool,"_Z11BOOL_RValuePK6RValue");
 legacy=RESOLVE(Legacy,"_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_");
 get=RESOLVE(Get,"_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
 free_value=RESOLVE(Free,"_Z16FREE_RValue__PreP6RValue");
 direction=RESOLVE(Direction,"_Z21YYGML_point_directionffff");
 real_value=RESOLVE(Real,"_Z14REAL_RValue_ExPK6RValue");
 global=RESOLVE(void **,"g_pGlobal"); object_hash=RESOLVE(void **,"g_ObjectHash");
 id_hash=RESOLVE(uint32_t *,"_ZN9CInstance14ms_ID2InstanceE");
 changes=RESOLVE(uint32_t *,"g_InstanceChangeArray"); activation=RESOLVE(uint32_t *,"g_InstanceActivateDeactive");
 trace_start=RESOLVE(Trace **,"_ZN13SYYStackTrace8s_pStartE");
 epsilon=RESOLVE(double *,"g_GMLMathEpsilon"); functions=RESOLVE(Function **,"the_functions");
 for(i=0;i<NV;++i) { strcpy(name,"g_VAR_"); strcat(name,variable_names[i]); variables[i]=RESOLVE(const uint32_t *,name); if(!variables[i]) return 0; }
 for(i=0;i<NF;++i) {
  strcpy(name,"g_FUNC_"); strcat(name,function_names[i]); descriptors[i]=RESOLVE(const uint32_t *,name);
  builtins[i]=RESOLVE(void *,builtin_symbols[i]);
  if(!descriptors[i] || !builtins[i]) return 0;
 }
#undef RESOLVE
 if(!read_var || !compare || !truth || !legacy || !get || !free_value || !direction || !real_value ||
    !global || !object_hash || !id_hash || !changes || !activation || !trace_start || !epsilon || !functions || !*functions) return 0;
 /* Accept loader overrides at install. builtins[] is resolution bookkeeping,
  * not a dispatch table; call() always uses YYGML_CallLegacyFunction. */
 for(i=0;i<NF;++i) {
  builtins[i]=(*functions)[descriptors[i][1]].call;
  if(!builtins[i]) return 0;
 }
 /* Use the exact stack-trace literal obtained from the entry's PC-relative ADD. */
 script_name=(const char *)(entry+0x40+(int32_t)word((void *)entry,0x1010));
 if(strcmp(script_name,"gml_Script_controller_check_pressed_ext")) return 0;
 rx=rx_alloc(16); if(!rx || ((uintptr_t)rx&3)) return 0;
 code[0]=0xe92d4ff0; code[1]=0xe28db01c; code[2]=0xe51ff004; code[3]=(uint32_t)(entry+8);
 kuKernelCpuUnrestrictedMemcpy(rx,code,sizeof code); kuKernelFlushCaches(rx,sizeof code);
 original=(Script)rx; hook_addr(entry,(uintptr_t)input_native_pressed); kuKernelFlushCaches((void *)entry,8);
 installed=1; return 1;
}

