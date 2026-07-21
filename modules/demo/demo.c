// demo — a sample foreground app.
//
// Apps are ELF binaries in /sdcard/bin/<name>.elf. Unlike a driver, an app
// does NOT call module_register: its main() (renamed app_main by the build)
// runs the program to completion and returns an exit code. The shell blocks
// while it runs, then resumes. This proves the launcher model: type "demo" in
// the console and this runs — using the kernel input service, then exits.

#include <stdint.h>

extern int vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int input_get_key(uint32_t timeout_ms);   // -1 on timeout

int main(int argc, char *argv[])
{
    vconsole_printf("\n=== demo app ===\n");
    vconsole_printf("argc=%d", argc);
    for (int i = 0; i < argc; i++)
        vconsole_printf(" [%d]=%s", i, argv[i] ? argv[i] : "(null)");
    vconsole_printf("\nPress a key on the keyboard to exit (10s timeout)...\n");

    int key = input_get_key(10000);
    if (key >= 0)
        vconsole_printf("Got key 0x%02X ('%c') — bye!\n",
                        key, (key >= 0x20 && key < 0x7F) ? (char)key : '?');
    else
        vconsole_printf("Timed out — bye!\n");

    return 0;
}
