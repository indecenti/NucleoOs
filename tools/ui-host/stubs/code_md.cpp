// ui-host: Code's markdown pass as a plain copy (the real one is in nucleo_code/code_rt.c, with the engine)
#include <stddef.h>
#include <string.h>
extern "C" size_t code_md_plain(const char *in, char *out, size_t cap)
{
    size_t n = strlen(in);
    if (n >= cap) n = cap - 1;
    memcpy(out, in, n); out[n] = 0;
    return n;
}
