#include "module_types.h"

extern void module_register(module_exports_t *exports);

static module_info_t info = {
    .magic       = MODULE_MAGIC,
    .abi_version = 99,
    .name        = "badabi",
    .version     = "0.0.1",
    .requires    = "",
    .flags       = 0,
};

static module_exports_t exports = {
    .info  = &info,
    .start = (void *)0,
    .stop  = (void *)0,
};

int main(int argc, char *argv[])
{
    module_register(&exports);
    return 0;
}
