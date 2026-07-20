// CardKB keyboard input driver (loadable module).
//
// M5Stack CardKB — an I2C keyboard at address 0x5F. Reading one byte returns
// the ASCII code of the pressed key, or 0x00 when nothing is pressed. This
// module polls it in a background task and pushes keys into the kernel input
// service, from where the shell (and later GUI apps) read them.

#include "module_types.h"
#include <stdint.h>
#include <stddef.h>

#define CARDKB_ADDR   0x5F
#define POLL_MS       20
#define TASK_STACK    2048
#define TASK_PRIO     3

extern void module_register(module_exports_t *exports);
extern int  vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Kernel ABI (resolved via the kernel symbol table at load time).
extern int  bus_i2c_read(uint8_t addr, uint8_t *data, size_t len);   // 0 = ESP_OK
extern int  input_push_key(int key);

typedef void (*task_fn_t)(void *);
extern int  xTaskCreate(task_fn_t fn, const char *name, uint32_t stack,
                        void *param, unsigned prio, void **handle);
extern void vTaskDelay(uint32_t ticks);   // CONFIG_FREERTOS_HZ=1000 → ticks == ms
extern void vTaskDelete(void *handle);

static void *volatile s_task;      // NULL once the poll task has exited
static volatile int   s_running;

static void cardkb_task(void *arg)
{
    (void)arg;
    uint8_t key;
    while (s_running) {
        if (bus_i2c_read(CARDKB_ADDR, &key, 1) == 0 && key != 0) {
            input_push_key((int)key);
        }
        vTaskDelay(POLL_MS);
    }
    s_task = NULL;
    vTaskDelete(NULL);   // delete self
}

static int cardkb_start(void)
{
    s_running = 1;
    if (xTaskCreate(cardkb_task, "cardkb", TASK_STACK, NULL, TASK_PRIO,
                    (void **)&s_task) != 1) {
        s_running = 0;
        vconsole_printf("[cardkb] failed to start poll task\n");
        return -1;
    }
    vconsole_printf("[cardkb] keyboard ready — polling I2C 0x%02X every %d ms\n",
                    CARDKB_ADDR, POLL_MS);
    return 0;
}

static int cardkb_stop(void)
{
    s_running = 0;   // task sees this and self-deletes (sets s_task = NULL)

    // Wait until the poll task has actually exited before returning, so the
    // module isn't unloaded (its code freed) while the task is still running
    // it — the task may be mid-i2c-read (up to ~100 ms) when we ask it to stop.
    for (int i = 0; i < 50 && s_task != NULL; i++)
        vTaskDelay(10);

    vconsole_printf("[cardkb] stopped\n");
    return 0;
}

static module_info_t info = {
    .magic       = MODULE_MAGIC,
    .abi_version = MODULE_ABI_VERSION,
    .name        = "cardkb",
    .version     = "1.0.0",
    .requires    = "input",
    .flags       = 0,
};

static module_exports_t exports = {
    .info  = &info,
    .start = cardkb_start,
    .stop  = cardkb_stop,
};

int main(int argc, char *argv[])
{
    module_register(&exports);
    return 0;
}
