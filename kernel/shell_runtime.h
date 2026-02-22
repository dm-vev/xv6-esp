#ifndef XV6_SHELL_RUNTIME_H
#define XV6_SHELL_RUNTIME_H

int shell_runtime_init(void);
int shell_runtime_bootstrap(const char *shell_path);
int shell_runtime_eval_line(const char *line, int *exit_code);
int shell_runtime_run_interactive(void);
void shell_runtime_reboot(void);

#endif
