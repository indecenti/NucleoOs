/* heap_model.h — force-included (gcc -include) into nucleo_gb.c by the cache gate.
 *
 * The device heap is not "N bytes free": in the emulator's Solo boot it is ~90 KB in pieces, the
 * largest of them 32 KB (/gbemu_trace.txt). A harness that answers every malloc from a PC heap can
 * never fail the way the device fails — a 32 KB allocation always succeeds on a PC. This routes the
 * firmware module's malloc/calloc/free through a best-fit model of a list of free blocks (see
 * gb_cache_test.c), and heap_caps_* read the same model (NUCLEO_HOST_HEAP_MODEL in the shim).
 *
 * <stdlib.h> is included FIRST so its declarations keep the real names; only the module's own calls
 * are renamed afterwards.
 */
#pragma once
#include <stdlib.h>
#include <string.h>

void *nucleo_hm_malloc(size_t n);
void *nucleo_hm_calloc(size_t n, size_t m);
void  nucleo_hm_free(void *p);

#define malloc(n)    nucleo_hm_malloc(n)
#define calloc(n, m) nucleo_hm_calloc(n, m)
#define free(p)      nucleo_hm_free(p)
