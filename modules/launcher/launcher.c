// launcher — an interactive app launcher (itself an app).
//
// Browses the apps in /sdcard/bin and runs the selected one. Demonstrates the
// whole model: an app that lists other apps (app_list), draws a menu to the
// console, reads the keyboard (input service), and launches a chosen app
// (app_run), returning to the menu when it exits. Built on the HW-validated
// console + input + app-run layers (no display dependency yet — a graphical
// version using display present() can come once the display path is verified).

#include <stdint.h>

#define APP_NAME_MAX 32
#define MAX_APPS     16

extern int vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int input_get_key(uint32_t timeout_ms);
extern int app_run(const char *name, int argc, char **argv);
extern int app_list(char (*names)[APP_NAME_MAX], int max);

// M5Stack CardKB arrow key codes.
#define KEY_UP    0xB5
#define KEY_DOWN  0xB6

static void draw_menu(char names[][APP_NAME_MAX], int count, int sel)
{
    vconsole_printf("\n===== App Launcher =====\n");
    if (count == 0) {
        vconsole_printf("  (no apps found in /sdcard/bin)\n");
    } else {
        for (int i = 0; i < count; i++)
            vconsole_printf("  %c %d. %s\n", (i == sel) ? '>' : ' ', i + 1, names[i]);
    }
    vconsole_printf("  [w/UP] [s/DOWN] [Enter]=run [1-9]=quick [q]=quit\n");
}

static void run_app(char names[][APP_NAME_MAX], int idx)
{
    char *av[1] = { names[idx] };
    vconsole_printf("\nlauncher: running '%s'...\n", names[idx]);
    app_run(names[idx], 1, av);
}

int main(int argc, char *argv[])
{
    char names[MAX_APPS][APP_NAME_MAX];
    int count = app_list(names, MAX_APPS);
    int sel = 0;

    draw_menu(names, count, sel);

    for (;;) {
        int key = input_get_key(60000);   // idle out after 60s of no input
        if (key < 0) { vconsole_printf("launcher: idle timeout — bye\n"); break; }

        if (key == 'q' || key == 'Q' || key == 0x1B) {   // quit
            vconsole_printf("launcher: bye\n");
            break;
        }

        if (count == 0) continue;

        if (key == 'w' || key == 'W' || key == KEY_UP) {
            sel = (sel > 0) ? sel - 1 : count - 1;
            draw_menu(names, count, sel);
        } else if (key == 's' || key == 'S' || key == KEY_DOWN) {
            sel = (sel + 1) % count;
            draw_menu(names, count, sel);
        } else if (key == '\r' || key == '\n') {
            run_app(names, sel);
            count = app_list(names, MAX_APPS);   // list may have changed
            if (sel >= count) sel = count ? count - 1 : 0;
            draw_menu(names, count, sel);
        } else if (key >= '1' && key <= '9') {
            int idx = key - '1';
            if (idx < count) {
                run_app(names, idx);
                count = app_list(names, MAX_APPS);
                if (sel >= count) sel = count ? count - 1 : 0;
                draw_menu(names, count, sel);
            }
        }
    }

    return 0;
}
