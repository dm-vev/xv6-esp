/*
 * Copyright (c) 1983 Regents of the University of California.
 * All rights reserved.  The Berkeley software License Agreement
 * specifies the terms and conditions for redistribution.
 */

/*
 * cp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/dir.h>
#include <sys/time.h>

#ifndef MAXBSIZE
#define MAXBSIZE 4096
#endif
#ifndef CP_RECURSION_MAX
#define CP_RECURSION_MAX 64
#endif

int iflag;
int rflag;
int pflag;
static int g_cp_recursion_depth;

static void Perror(char *s);
static int copy(char *from, char *to);
static int rcopy(char *from, char *to);
static int setimes(char *path, struct stat *statp);

int main(
    int argc,
    char **argv)
{
    struct stat stb;
    int rc, i;

    iflag = 0;
    rflag = 0;
    pflag = 0;
    g_cp_recursion_depth = 0;

    argc--, argv++;
    while (argc > 0 && **argv == '-') {
        (*argv)++;
        while (**argv) switch (*(*argv)++) {

        case 'i':
            iflag++; break;

        case 'R':
        case 'r':
            rflag++; break;

        case 'p':   /* preserve mtimes, atimes, and modes */
            pflag++;
            (void) umask(0);
            break;

        default:
            goto usage;
        }
        argc--; argv++;
    }
    if (argc < 2)
        goto usage;
    if (argc > 2) {
        if (stat(argv[argc-1], &stb) < 0)
            goto usage;
        if ((stb.st_mode&S_IFMT) != S_IFDIR)
            goto usage;
    }
    rc = 0;
    for (i = 0; i < argc-1; i++)
        rc |= copy(argv[i], argv[argc-1]);
    exit(rc);
usage:
    fprintf(stderr,
        "Usage: cp [-ip] f1 f2; or: cp [-irp] f1 ... fn d2\n");
    exit(1);
}

int copy(
    char *from, char *to)
{
    int fold, fnew, n, exists;
    char *last, destname[MAXPATHLEN + 1];
    char *buf = 0;
    struct stat stfrom, stto;

    if (stat(from, &stfrom) < 0) {
        Perror(from);
        return (1);
    }

    if (stat(to, &stto) >= 0 &&
       (stto.st_mode&S_IFMT) == S_IFDIR) {
        last = rindex(from, '/');
        if (last) last++; else last = from;
        if (strlen(to) + strlen(last) >= sizeof destname - 1) {
            fprintf(stderr, "cp: %s/%s: Name too long", to, last);
            return(1);
        }
        if (snprintf(destname, sizeof(destname), "%s/%s", to, last) >= (int)sizeof(destname)) {
            fprintf(stderr, "cp: %s/%s: Name too long\n", to, last);
            return (1);
        }
        to = destname;
    }

    if (rflag && (stfrom.st_mode&S_IFMT) == S_IFDIR) {
        int fixmode = 0;    /* cleanup mode after rcopy */
        int rc;

        if (g_cp_recursion_depth >= CP_RECURSION_MAX) {
            fprintf(stderr, "cp: %s: recursion limit reached\n", from);
            return (1);
        }

        if (stat(to, &stto) < 0) {
            if (mkdir(to, (stfrom.st_mode & 07777) | 0700) < 0) {
                Perror(to);
                return (1);
            }
            fixmode = 1;
        } else if ((stto.st_mode&S_IFMT) != S_IFDIR) {
            fprintf(stderr, "cp: %s: Not a directory.\n", to);
            return (1);
        } else if (pflag)
            fixmode = 1;
        g_cp_recursion_depth++;
        rc = rcopy(from, to);
        g_cp_recursion_depth--;
        if (fixmode)
            (void) chmod(to, stfrom.st_mode & 07777);
        return (rc);
    }

    fold = open(from, 0);
    if (fold < 0) {
        Perror(from);
        return (1);
    }

    if ((stfrom.st_mode&S_IFMT) == S_IFDIR)
        fprintf(stderr,
            "cp: %s: Is a directory (copying as plain file).\n",
                from);

    exists = stat(to, &stto) == 0;
    if (exists) {
        if (stfrom.st_dev == stto.st_dev &&
           stfrom.st_ino == stto.st_ino) {
            fprintf(stderr,
                "cp: %s and %s are identical (not copied).\n",
                    from, to);
            (void) close(fold);
            return (1);
        }
        if (iflag && isatty(fileno(stdin))) {
            int i, c;

            fprintf (stderr, "overwrite %s? ", to);
            i = c = getchar();
            while (c != '\n' && c != EOF)
                c = getchar();
            if (i != 'y') {
                (void) close(fold);
                return(1);
            }
        }
    }
    fnew = creat(to, stfrom.st_mode & 07777);
    if (fnew < 0) {
        Perror(to);
        (void) close(fold); return(1);
    }
    buf = (char *)malloc(MAXBSIZE);
    if (buf == 0) {
        fprintf(stderr, "cp: out of memory\n");
        (void) close(fold); (void) close(fnew); return (1);
    }
    if (exists && pflag)
        (void) fchmod(fnew, stfrom.st_mode & 07777);

    for (;;) {
        n = read(fold, buf, MAXBSIZE);
        if (n == 0)
            break;
        if (n < 0) {
            Perror(from);
            free(buf);
            (void) close(fold); (void) close(fnew); return (1);
        }
        if (write(fnew, buf, n) != n) {
            Perror(to);
            free(buf);
            (void) close(fold); (void) close(fnew); return (1);
        }
    }
    free(buf);
    (void) close(fold); (void) close(fnew);
    if (pflag)
        return (setimes(to, &stfrom));
    return (0);
}

int rcopy(
    char *from, char *to)
{
    DIR *fold = opendir(from);
    struct dirent *dp;
    struct stat statb;
    int errs = 0;
    long max_iters = 4096;
    long iter = 0;
    char fromname[MAXPATHLEN + 1];

    if (stat(from, &statb) == 0 && statb.st_size > 0) {
        long est = (long)(statb.st_size / 4) + 64;
        if (est > max_iters)
            max_iters = est;
        if (max_iters > 200000)
            max_iters = 200000;
    }

    if (fold == 0 || (pflag && fstat(dirfd(fold), &statb) < 0)) {
        Perror(from);
        return (1);
    }
    for (;;) {
        if (++iter > max_iters) {
            fprintf(stderr, "cp: %s: directory iteration limit exceeded\n", from);
            (void)closedir(fold);
            return (errs + 1);
        }
        dp = readdir(fold);
        if (dp == 0) {
            closedir(fold);
            if (pflag)
                return (setimes(to, &statb) + errs);
            return (errs);
        }
        if (dp->d_ino == 0)
            continue;
        if (dp->d_name[0] == '\0') {
            errs++;
            continue;
        }
        if (strchr(dp->d_name, '/') != 0) {
            fprintf(stderr, "cp: %s/%s: Invalid directory entry name.\n", from, dp->d_name);
            errs++;
            continue;
        }
        if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, ".."))
            continue;
        if (strlen(from)+1+strlen(dp->d_name) >= sizeof fromname - 1) {
            fprintf(stderr, "cp: %s/%s: Name too long.\n",
                from, dp->d_name);
            errs++;
            continue;
        }
        if (snprintf(fromname, sizeof(fromname), "%s/%s", from, dp->d_name) >= (int)sizeof(fromname)) {
            fprintf(stderr, "cp: %s/%s: Name too long.\n", from, dp->d_name);
            errs++;
            continue;
        }
        errs += copy(fromname, to);
    }
}

int
setimes(
    char *path,
    struct stat *statp)
{
    struct timeval tv[2];

    tv[0].tv_sec = statp->st_atime;
    tv[1].tv_sec = statp->st_mtime;
    tv[0].tv_usec = tv[1].tv_usec = 0;
    if (utimes(path, tv) < 0) {
        Perror(path);
        return (1);
    }
    return (0);
}

void Perror(
    char *s)
{
    fprintf(stderr, "cp: ");
    perror(s);
}
