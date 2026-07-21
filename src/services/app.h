#ifndef APP_H
#define APP_H

// Foreground application runtime.
//
// Apps are downloadable ELF binaries in /sdcard/bin/<name>.elf (the same PATH
// `which` searches). Unlike drivers (which register via module_register and
// run in the background through modstart), an app's app_main() runs the app
// to completion on the caller's task and returns an exit code — the shell
// blocks until it returns, then resumes. This keeps the launcher model simple:
// type an app name in the console and it runs.

#define APP_NOT_FOUND  (-1000)   // no such binary in the search path
#define APP_NAME_MAX   32        // max app-name length (incl. NUL)

// Run app `name` (bare name → /sdcard/bin/<name>.elf, or an explicit path with
// a '/'). Loads the ELF, calls its entry with argc/argv, unloads, and returns
// the app's exit code — or APP_NOT_FOUND if the binary does not exist.
int app_run(const char *name, int argc, char **argv);

// List app names (basenames of *.elf in /sdcard/bin) into `names`, up to `max`.
// Returns the count found. Used by a launcher to browse installed apps.
int app_list(char (*names)[APP_NAME_MAX], int max);

// File helpers exported to apps (modules can't reach libc fopen/fread). Read
// up to `max` bytes of `path` into `buf` (returns bytes read, or -1); write
// `len` bytes of `buf` to `path`, truncating (returns 0 on success, -1 on err).
int app_read_file (const char *path, char *buf, int max);
int app_write_file(const char *path, const char *buf, int len);

#endif
