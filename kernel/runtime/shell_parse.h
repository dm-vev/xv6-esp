#ifndef XV6_SHELL_PARSE_H
#define XV6_SHELL_PARSE_H

#include "core/types.h"

/*
 * parse_line() uses in-band marker bytes for escaped/protected metacharacters.
 * Interactive shell input is printable ASCII, so low control-byte markers are safe.
 */
#define KSH_MARK_ESC_DOLLAR ((char)0x01)
#define KSH_MARK_ESC_STAR ((char)0x02)
#define KSH_MARK_ESC_QMARK ((char)0x03)
#define KSH_MARK_ESC_LBRACK ((char)0x04)
#define KSH_MARK_ESC_RBRACK ((char)0x05)

int parse_line(char *line, char **argv, int max_args);
int parse_u32_dec(const char *s, uint32 *out);
int parse_i32_dec(const char *s, int *out);
int is_fd_token(const char *s, int *out_fd);
int is_redir_token(const char *s);
int is_control_token(const char *s);
int find_pipe_pos(int argc, char **argv);
int build_pipeline(int argc, char **argv, int *starts, int *lens, int max_stages);

#endif
