
if(CONFIG_ELF_LOADER)
    set(srcs  "src/esp_elf_symbol.c"
              "src/esp_elf.c"
              "src/esp_elf_adapter.c")

    if(CONFIG_ELF_LOADER_CUSTOMER_SYMBOLS)
        list(APPEND srcs "src/esp_all_symbol.c")
    endif()

    if(CONFIG_IDF_TARGET_ARCH_XTENSA)
        list(APPEND srcs "src/arch/esp_elf_xtensa.c")

        # ESP32-S2 need to set MMU to run ELF
        if(CONFIG_IDF_TARGET_ESP32S2 AND (CONFIG_ELF_LOADER_LOAD_PSRAM))
            list(APPEND srcs "src/soc/esp_elf_esp32s2.c")
        endif()
    elseif(CONFIG_IDF_TARGET_ARCH_RISCV)
        list(APPEND srcs "src/arch/esp_elf_riscv.c")
    endif()

    if(CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT)
        list(APPEND srcs "src/dlso/dlfcn.c"
                         "src/dlso/dlmod.c")
    endif()

    set(include_dirs "include")
    set(ldfragments "linker.lf")
endif()

set(priv_req spi_flash)

if(CONFIG_IDF_TARGET_ESP32P4)
    list(APPEND priv_req esp_mm)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include_dirs}
                       PRIV_REQUIRES ${priv_req}
                       LDFRAGMENTS ${ldfragments})

# micro_arch PATCH-000: vendored fork — the cmake_utilities package_manager
# dependency (cu_pkg_define_version) is replaced by explicit version
# definitions matching the upstream release this fork is based on.
if(CONFIG_ELF_LOADER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        ELF_LOADER_VER_MAJOR=1
        ELF_LOADER_VER_MINOR=3
        ELF_LOADER_VER_PATCH=1)
endif()
