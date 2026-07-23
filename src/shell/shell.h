#ifndef SHELL_H
#define SHELL_H

#include "esp_err.h"

esp_err_t shell_init(void);
void      shell_start(void);

const char *shell_get_cwd(void);
void        shell_set_cwd(const char *path);
void        shell_resolve_path(const char *input, char *output, size_t output_size);

// Run a console command line programmatically (built-in first, then the app_run
// fallback). Lets a GUI app do anything the console can. Returns the command's
// exit code, or negative on a run error.
int         shell_exec(const char *line);

#endif
