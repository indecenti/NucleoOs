// probe.c: run JavaScript files in Nucleo Code's runtime (code_rt.c, the device's engine) with the current folder as the workspace;
// the agent bench runs its checkers with it. Usage: probe file.js...
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "code_rt.h"
static int64_t now_ms(void){ return (int64_t)GetTickCount64(); }
static void out(void *op, const char *s, size_t n){ (void)op; fwrite(s,1,n,stdout); }
int main(int argc, char **argv){
  for (int i = 1; i < argc; i++) {
    FILE *f = fopen(argv[i], "rb"); if (!f) continue; static char buf[65536]; size_t n = fread(buf,1,sizeof buf-1,f); fclose(f); buf[n]=0;
    code_env_t e = {0}; e.root="."; e.out=out; e.now_ms=now_ms; e.timeout_ms=2000;
    // PROBE_SCREEN=WxH: an app on a virtual screen; its last frame goes to probe-shot.png, an app loop ending at the limit is fine
    const char *ps = getenv("PROBE_SCREEN"); int sw = 0, sh = 0;
    if (ps && sscanf(ps, "%dx%d", &sw, &sh) == 2) { e.scr_w = sw; e.scr_h = sh; e.shot = "probe-shot.png"; }
    static char arena[96*1024]; code_result_t r;
    printf("%-3d| ", i); code_run(arena, sizeof arena, buf, n, "t.js", &e, &r);
    if (r.drew && r.frames && (r.ok || r.timeout)) printf("DREW %dx%d frames=%u\n", r.scr_w, r.scr_h, (unsigned)r.frames);
    else if (!r.ok) printf("ERR %s %s\n", r.error, r.where);
  }
  return 0;
}
