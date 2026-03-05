/**
 * @file hostabi_posix_io.h
 * @brief POSIX I/O and terminal control API for xv6 host environment
 *
 * This header provides POSIX I/O operations beyond basic file operations,
 * including terminal (TTY) control, file descriptor flags, and ioctl support.
 * Maintains metadata for each file descriptor including TTY state.
 */
#ifndef XV6_HOSTABI_POSIX_IO_H
#define XV6_HOSTABI_POSIX_IO_H

#include <termios.h>

/**
 * @brief Initialize the I/O subsystem
 *
 * Initializes the file descriptor metadata system and sets up default
 * metadata for stdin, stdout, and stderr (fd 0, 1, 2).
 * Must be called before any other I/O operations.
 */
void hostabi_posix_io_init(void);

/**
 * @brief Called when a file is opened
 * @param fd File descriptor
 * @param flags Open flags
 * @param path File path (for TTY detection)
 *
 * Initializes metadata for a newly opened file descriptor. Detects if
 * the file is a TTY based on the path and sets appropriate flags.
 */
void hostabi_posix_io_on_open(int fd, int flags, const char *path);

/**
 * @brief Called when a file is closed
 * @param fd File descriptor to close
 * @param keep_stdio_defaults If true, preserve defaults for stdio fds
 *
 * Clears metadata for a closed file descriptor. Optionally preserves
 * default metadata for standard I/O streams (stdin/stdout/stderr).
 */
void hostabi_posix_io_on_close(int fd, int keep_stdio_defaults);

/**
 * @brief Called when a file descriptor is duplicated
 * @param oldfd Original file descriptor
 * @param newfd New file descriptor
 *
 * Copies metadata from oldfd to newfd after duplication.
 */
void hostabi_posix_io_on_dup(int oldfd, int newfd);

/**
 * @brief Check if file descriptor is a terminal
 * @param fd File descriptor to check
 * @return 1 if TTY, 0 otherwise
 *
 * Determines whether the file descriptor refers to a terminal device.
 * Sets errno to ENOTTY if not a TTY.
 */
int hostabi_posix_isatty(int fd);

/**
 * @brief Manipulate file descriptor flags
 * @param fd File descriptor
 * @param cmd Fcntl command (F_GETFL, F_SETFL, F_GETFD, F_SETFD, F_DUPFD, etc.)
 * @param ... Optional argument for the command
 * @return Depends on cmd, -1 on failure
 *
 * Performs various operations on file descriptors including getting/setting
 * flags, duplicating with minimum fd value, etc.
 */
int hostabi_posix_fcntl(int fd, int cmd, ...);

/**
 * @brief Device control
 * @param fd File descriptor
 * @param request Ioctl request code
 * @param ... Optional argument
 * @return 0 on success, -1 on failure
 *
 * Performs device-specific operations. Supports FIONBIO (non-blocking),
 * TIOCGWINSZ/TIOCSWINSZ (window size), and TCSGETS/TCSETS (terminal settings).
 */
int hostabi_posix_ioctl(int fd, unsigned long request, ...);

/**
 * @brief Get terminal attributes
 * @param fd File descriptor
 * @param tio Buffer to store terminal attributes
 * @return 0 on success, -1 on failure
 *
 * Retrieves the current terminal parameters for the given fd.
 * @pre tio != NULL
 */
int hostabi_posix_tcgetattr(int fd, struct termios *tio);

/**
 * @brief Set terminal attributes
 * @param fd File descriptor
 * @param optional_actions When to apply changes (TCSANOW, TCSADRAIN, TCSAFLUSH)
 * @param tio New terminal attributes
 * @return 0 on success, -1 on failure
 *
 * Sets terminal parameters. The optional_actions determines when the
 * changes take effect.
 * @pre tio != NULL
 * @pre optional_actions is TCSANOW, TCSADRAIN, or TCSAFLUSH
 */
int hostabi_posix_tcsetattr(int fd, int optional_actions, const struct termios *tio);

/**
 * @brief Make terminal raw (unbuffered, no echo, etc.)
 * @param tio Terminal attributes structure
 *
 * Configures the terminal into raw mode by disabling echo, canonical mode,
 * signals, and other processing.
 */
void hostabi_posix_cfmakeraw(struct termios *tio);

/** Callback used to deliver TTY-generated signals to foreground process group. */
typedef int (*hostabi_posix_tty_signal_handler_t)(int sig);

/** Callback used to query whether the current task owns the foreground TTY. */
typedef int (*hostabi_posix_tty_foreground_query_t)(int fd);

/**
 * @brief Register TTY signal delivery callback
 * @param handler Callback invoked for line-discipline signals
 */
void hostabi_posix_set_tty_signal_handler(hostabi_posix_tty_signal_handler_t handler);

/**
 * @brief Register foreground/background ownership query for controlling TTY
 * @param query Callback returning 1 for foreground, 0 for background, <0 on error
 */
void hostabi_posix_set_tty_foreground_query(hostabi_posix_tty_foreground_query_t query);

/**
 * @brief Map an input byte to a TTY-generated signal according to line discipline
 * @param c Input byte (0..255)
 * @return Signal number (e.g. SIGINT/SIGTSTP) or 0 if byte is not a signal char
 *
 * Uses current controlling TTY settings (ISIG, VINTR, VSUSP).
 */
int hostabi_posix_tty_signal_for_char(int c);

/**
 * @brief Deliver a line-discipline signal via registered callback
 * @param sig Signal number to deliver
 * @return 0 on success, -1 if callback is missing or delivery failed
 */
int hostabi_posix_tty_dispatch_signal(int sig);

/**
 * @brief Poll console and return TTY-generated signal from line discipline
 * @return Signal number (e.g. SIGINT/SIGTSTP) or 0 if no signal char was received
 *
 * Polls bytes configured by line discipline control chars without consuming
 * unrelated input bytes.
 */
int hostabi_posix_tty_poll_signal(void);

/**
 * @brief Enforce foreground/background rules before reading from a TTY
 * @param fd File descriptor to read from
 * @return 0 if access is allowed, -1 on failure
 */
int hostabi_posix_tty_before_read(int fd);

/**
 * @brief Enforce foreground/background rules before writing to a TTY
 * @param fd File descriptor to write to
 * @return 0 if access is allowed, -1 on failure
 */
int hostabi_posix_tty_before_write(int fd);

/**
 * @brief Enforce foreground/background rules before mutating TTY attributes
 * @param fd File descriptor whose terminal settings are being changed
 * @return 0 if access is allowed, -1 on failure
 */
int hostabi_posix_tty_before_attr_change(int fd);

#endif
