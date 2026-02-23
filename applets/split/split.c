#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned count = 1000;
int fnumber;
char fname[100];
char *ifil;
char *ofil;
FILE *is;
FILE *os;

static int parse_positive_count(const char *s, unsigned *out);

int main(int argc, char *argv[])
{
    int i, c, f;
    int iflg = 0;

    count = 1000;
    fnumber = 0;
    ifil = 0;
    ofil = 0;
    is = 0;
    os = 0;

    for (i = 1; i < argc; i++)
        if (argv[i][0] == '-')
            switch (argv[i][1]) {
            case '\0':
                iflg = 1;
                continue;

            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                if (parse_positive_count(argv[i] + 1, &count) != 0) {
                    fprintf(stderr, "split: bad line count %s\n", argv[i]);
                    exit(1);
                }
                continue;
            }
        else if (iflg)
            ofil = argv[i];
        else {
            ifil = argv[i];
            iflg = 2;
        }
    if (iflg != 2)
        is = stdin;
    else if ((is = fopen(ifil, "r")) == NULL) {
        perror(ifil);
        exit(1);
    }
    if (ofil == 0)
        ofil = "x";
    if (strlen(ofil) > sizeof(fname) - 3) {
        fprintf(stderr, "split: output prefix too long\n");
        exit(1);
    }

loop:
    f = 1;
    for (i = 0; i < count; i++)
        do {
            c = getc(is);
            if (c == EOF) {
                if (f == 0 && os != NULL)
                    fclose(os);
                exit(0);
            }
            if (f) {
                if (fnumber >= 26 * 26) {
                    fprintf(stderr, "split: too many output files\n");
                    exit(1);
                }
                for (f = 0; ofil[f] != '\0'; f++)
                    fname[f] = ofil[f];
                fname[f++] = fnumber / 26 + 'a';
                fname[f++] = fnumber % 26 + 'a';
                fname[f] = '\0';
                fnumber++;
                if ((os = fopen(fname, "w")) == NULL) {
                    fprintf(stderr, "Cannot create output\n");
                    exit(1);
                }
                f = 0;
            }
            putc(c, os);
        } while (c != '\n');
    if (os != NULL) {
        fclose(os);
        os = NULL;
    }
    goto loop;
}

int parse_positive_count(const char *s, unsigned *out)
{
    char *end = NULL;
    unsigned long v;

    if (s == NULL || *s == '\0')
        return -1;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v == 0 || v > UINT_MAX)
        return -1;
    *out = (unsigned)v;
    return 0;
}
