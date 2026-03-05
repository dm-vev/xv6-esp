#ifndef XV6_TERMIOS_COMPAT_H
#define XV6_TERMIOS_COMPAT_H

#include <stdint.h>
#include <sys/types.h>

typedef uint8_t cc_t;
typedef uint32_t speed_t;
typedef uint16_t tcflag_t;

#define VEOF 0
#define VEOL 1
#define VERASE 2
#define VINTR 3
#define VKILL 4
#define VMIN 5
#define VQUIT 6
#define VSTART 7
#define VSTOP 8
#define VSUSP 9
#define VTIME 10
#define NCCS (VTIME + 1)

#define BRKINT (1u << 0)
#define ICRNL (1u << 1)
#define INPCK (1u << 6)
#define ISTRIP (1u << 7)
#define IXON (1u << 11)

#define OPOST (1u << 0)
#define ONLCR (1u << 2)

#define CS8 (3u << 0)
#define CREAD (1u << 3)

#define ECHO (1u << 0)
#define ICANON (1u << 4)
#define IEXTEN (1u << 5)
#define ISIG (1u << 6)
#define TOSTOP (1u << 8)

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413UL
#endif
#ifndef TIOCSWINSZ
#define TIOCSWINSZ 0x5414UL
#endif

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#endif
