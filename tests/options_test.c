/* Every option parameter must round-trip: a host tap at option k must read back as option k,
 * and the display text must match. The framework host test only exercises parameter 0, which
 * is why a later option list could break unnoticed (device report 2026-10-31: SYNC's 1/16 and
 * 1/8, REVERB's 201 jumped back). */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "params.h"
typedef struct AEffect AEffect;
typedef intptr_t (*cb)(AEffect*,int32_t,int32_t,intptr_t,void*,float);
struct AEffect { int32_t magic; intptr_t (*d)(AEffect*,int32_t,int32_t,intptr_t,void*,float);
 void*p; void (*setP)(AEffect*,int32_t,float); float (*getP)(AEffect*,int32_t);
 int32_t np,npar,ni,no,flags; intptr_t r1,r2; int32_t a,b,c; float io; void*obj,*user; int32_t uid,ver;
 void (*pr)(AEffect*,float**,float**,int32_t); void*pdr; char f[56]; };
extern AEffect* VSTPluginMain(cb);
static intptr_t host(AEffect*e,int32_t op,int32_t i,intptr_t v,void*p,float o){ (void)e;(void)op;(void)i;(void)v;(void)p;(void)o; return 0; }

int main(void) {
  AEffect *a = VSTPluginMain(host);
  if (!a) { printf("FAIL: no plugin\n"); return 1; }
  int fails = 0, checked = 0;
  for (int i = 0; i < NPARAMS; i++) {
    const int n = PARAMS[i].nopts;
    if (n < 2) continue;
    for (int k = 0; k < n; k++) {
      const float want = (float)k / (float)(n - 1);
      a->setP(a, i, want);
      const float got = a->getP(a, i);
      char disp[64] = {0};
      a->d(a, 7 /*effGetParamDisplay*/, i, 0, disp, 0);
      const int ok = fabsf(got - want) < 0.02f && strcmp(disp, PARAMS[i].opts[k]) == 0;
      if (!ok) {
        printf("FAIL %-10s option %d/%d: set %.3f -> get %.3f, display '%s' (want '%s')\n",
               PARAMS[i].key, k, n - 1, want, got, disp, PARAMS[i].opts[k]);
        ++fails;
      }
      ++checked;
    }
  }
  printf("%s: %d option selections checked (%d failed)\n", fails ? "FAILED" : "PASSED", checked, fails);
  return fails ? 1 : 0;
}
