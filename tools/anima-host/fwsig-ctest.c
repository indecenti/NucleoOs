// fwsig-check.mjs's driver: streams a signed file through the REAL fwsig.c in chunks of the given size and
// checks its trailing block.   fwsig-ctest <file> <domain> <version> <chunk>   ->  prints the verdict.
#include "fwsig.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    size_t chunk = (size_t)atoi(argv[4]);
    static uint8_t buf[70000];
    fwsig_stream_t s;
    fwsig_stream_init(&s);
    size_t n;
    while ((n = fread(buf, 1, chunk, f)) > 0) fwsig_stream_update(&s, buf, n);
    fclose(f);
    char sha[65]; uint32_t size = 0;
    bool whole = fwsig_stream_final(&s, sha, &size);
    fwsig_stream_free(&s);
    printf("%s %s %u\n", whole ? fwsig_rc_name(fwsig_verify(s.tail, argv[2], argv[3], sha, size)) : "short", sha, (unsigned)size);
    return 0;
}
