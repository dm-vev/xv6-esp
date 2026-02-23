/*
 * Deal with duplicated lines in a file
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

int fields;
int letters;
int linec;
char mode;
int uniq;
static FILE *g_in = NULL;
static FILE *g_out = NULL;

#define LINEBUFSZ 1000

static void printe(const char *p, const char *s);
static int parse_nonneg_int(const char *s, int *out);
static int gline(char buf[], size_t buflen);
static void pline(char buf[]);
static int equal(char b1[], char b2[]);
static char *skip(char *s);

int main(int argc, char *argv[])
{
    static char b1[LINEBUFSZ], b2[LINEBUFSZ];
    FILE *in_file = NULL;
    FILE *out_file = NULL;

    fields = 0;
    letters = 0;
    linec = 0;
    mode = 0;
    uniq = 0;
    g_in = stdin;
    g_out = stdout;

    while (argc > 1) {
        if (*argv[1] == '-') {
            if (isdigit((unsigned char)argv[1][1])) {
                if (parse_nonneg_int(&argv[1][1], &fields) != 0)
                    printe("bad field count %s\n", argv[1]);
            } else {
                mode = argv[1][1];
            }
            argc--;
            argv++;
            continue;
        }
        if (*argv[1] == '+') {
            if (parse_nonneg_int(&argv[1][1], &letters) != 0)
                printe("bad letter count %s\n", argv[1]);
            argc--;
            argv++;
            continue;
        }
        in_file = fopen(argv[1], "r");
        if (in_file == NULL)
            printe("cannot open %s\n", argv[1]);
        g_in = in_file;
        break;
    }
    if (argc > 2) {
        out_file = fopen(argv[2], "w");
        if (out_file == NULL)
            printe("cannot create %s\n", argv[2]);
        g_out = out_file;
    }

    if (gline(b1, sizeof(b1)))
        goto done;
    for (;;) {
        linec++;
        if (gline(b2, sizeof(b2))) {
            pline(b1);
            goto done;
        }
        if (!equal(b1, b2)) {
            pline(b1);
            linec = 0;
            do {
                linec++;
                if (gline(b1, sizeof(b1))) {
                    pline(b2);
                    goto done;
                }
            } while (equal(b1, b2));
            pline(b2);
            linec = 0;
        }
    }

done:
    if (out_file)
        fclose(out_file);
    if (in_file)
        fclose(in_file);
    exit(0);
}

int parse_nonneg_int(const char *s, int *out)
{
    char *end = NULL;
    long v;

    if (s == NULL || *s == '\0')
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > INT_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

int gline(char buf[], size_t buflen)
{
    size_t pos = 0;
    int c;

    if (buflen == 0)
        return (1);
    for (;;) {
        c = fgetc(g_in);
        if (c == EOF) {
            if (pos == 0)
                return (1);
            break;
        }
        if (c == '\n')
            break;
        if (pos + 1 < buflen)
            buf[pos++] = (char)c;
    }
    buf[pos] = '\0';
    return (0);
}

void pline(char buf[])
{
    switch (mode) {
    case 'u':
        if (uniq) {
            uniq = 0;
            return;
        }
        break;

    case 'd':
        if (uniq)
            break;
        return;

    case 'c':
        fprintf(g_out, "%4d ", linec);
    }
    uniq = 0;
    fputs(buf, g_out);
    fputc('\n', g_out);
}

int equal(char b1[], char b2[])
{
    char c;

    b1 = skip(b1);
    b2 = skip(b2);
    while ((c = *b1++) != 0)
        if (c != *b2++)
            return (0);
    if (*b2 != 0)
        return (0);
    uniq++;
    return (1);
}

char *skip(char *s)
{
    int nf, nl;

    nf = nl = 0;
    while (nf++ < fields) {
        while (*s == ' ' || *s == '\t')
            s++;
        while (!(*s == ' ' || *s == '\t' || *s == 0))
            s++;
    }
    while (nl++ < letters && *s != 0)
        s++;
    return (s);
}

void printe(const char *p, const char *s)
{
    fprintf(stderr, p, s);
    exit(1);
}
