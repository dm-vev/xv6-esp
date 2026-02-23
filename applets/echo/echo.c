#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    int i, nflg;

    nflg = 0;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'n' && !argv[1][2]) {
        nflg++;
        argc--;
        argv++;
    }
    for (i = 1; i < argc; i++) {
        const char *s = argv[i];
        size_t n = strlen(s);
        if (n > 0)
            (void)write(1, s, n);
        if (i < argc - 1)
            (void)write(1, " ", 1);
    }
    if (nflg == 0)
        (void)write(1, "\n", 1);
    exit(0);
}
