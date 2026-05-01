#include "module_types.h"

extern void module_register(module_exports_t *exports);
extern int  vconsole_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static module_info_t info = {
    .magic       = MODULE_MAGIC,
    .abi_version = MODULE_ABI_VERSION,
    .name        = "hello",
    .version     = "1.0.0",
    .requires    = "",
    .flags       = 0,
};

static int hello_start(void)
{
    vconsole_printf("[hello] Module started — greetings from loadable code!\n");
    return 0;
}

static int hello_stop(void)
{
    vconsole_printf("[hello] Module stopped\n");
    return 0;
}

static module_exports_t exports = {
    .info  = &info,
    .start = hello_start,
    .stop  = hello_stop,
};

int main(int argc, char *argv[])
{
    module_register(&exports);
    return 0;
}
