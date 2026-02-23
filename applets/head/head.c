#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int parse_num(const char *s)
{
    int v = 0;
    if (s == NULL || *s == '\0')
        return -1;
    while (*s >= '0' && *s <= '9') {
        v = (v * 10) + (*s - '0');
        s++;
    }
    if (*s != '\0')
        return -1;
    return v;
}

static int copy_head_fd(int fd, int lines)
{
    char ch;
    int rc;

    while (lines > 0) {
        rc = read(fd, &ch, 1);
        if (rc == 0)
            return 0;
        if (rc < 0)
            return -1;
        if (write(1, &ch, 1) != 1)
            return -1;
        if (ch == '\n')
            lines--;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int lines = 10;
    int argi = 1;
    int printed_any = 0;
    int rc = 0;

    while (argi < argc && argv[argi][0] == '-') {
        int n = parse_num(argv[argi] + 1);
        if (n < 0) {
            fprintf(stderr, "Badly formed number\n");
            return 1;
        }
        lines = n;
        argi++;
    }

    if (argi >= argc) {
        if (copy_head_fd(0, lines) != 0) {
            perror("stdin");
            return 1;
        }
        return 0;
    }

    while (argi < argc) {
        int fd = open(argv[argi], O_RDONLY);
        if (fd < 0) {
            perror(argv[argi]);
            rc = 1;
            argi++;
            continue;
        }
        if (printed_any)
            (void)write(1, "\n", 1);
        printed_any = 1;
        if ((argc - argi) > 1) {
            printf("==> %s <==\n", argv[argi]);
        }
        if (copy_head_fd(fd, lines) != 0) {
            perror(argv[argi]);
            rc = 1;
        }
        (void)close(fd);
        argi++;
    }

    return rc;
}
