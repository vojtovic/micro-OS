#!/usr/bin/env python3
"""
mkpkg.py — Build .mpkg packages from .elf + .ini module pairs.

Usage:
    python3 mkpkg.py <module_dir> [output_dir]

Example:
    python3 mkpkg.py modules/hello repo/
    -> Creates repo/hello.mpkg

The MPKG format:
    4 bytes  magic   (0x4D504B47 = "MPKG")
    4 bytes  version (1)
    4 bytes  elf_size
    4 bytes  ini_size
    32 bytes elf_hash (SHA-256)
    [elf_size bytes of ELF data]
    [ini_size bytes of INI data]
"""

import struct
import hashlib
import sys
import os


PKG_MAGIC = 0x4D504B47
PKG_VERSION = 1


def build_mpkg(module_dir, output_dir):
    name = os.path.basename(module_dir.rstrip("/"))

    elf_path = os.path.join(module_dir, f"{name}.elf")
    if not os.path.exists(elf_path):
        elf_path = os.path.join(os.path.dirname(module_dir), f"{name}.elf")
    if not os.path.exists(elf_path):
        print(f"Error: {name}.elf not found in {module_dir}")
        return None

    ini_path = os.path.join(module_dir, f"{name}.ini")
    if not os.path.exists(ini_path):
        ini_path = os.path.join(os.path.dirname(module_dir), f"{name}.ini")

    with open(elf_path, "rb") as f:
        elf_data = f.read()

    ini_data = b""
    if os.path.exists(ini_path):
        with open(ini_path, "rb") as f:
            ini_data = f.read()

    elf_hash = hashlib.sha256(elf_data).digest()

    header = struct.pack("<IIII32s",
                         PKG_MAGIC, PKG_VERSION,
                         len(elf_data), len(ini_data),
                         elf_hash)

    os.makedirs(output_dir, exist_ok=True)
    out_path = os.path.join(output_dir, f"{name}.mpkg")
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(elf_data)
        f.write(ini_data)

    total = len(header) + len(elf_data) + len(ini_data)
    print(f"  {name}.mpkg ({total} bytes, elf={len(elf_data)}, ini={len(ini_data)})")
    return {
        "name": name,
        "size": total,
        "hash": elf_hash.hex(),
        "filename": f"{name}.mpkg",
    }


def read_ini_field(ini_path, section, key):
    if not os.path.exists(ini_path):
        return None
    current_section = None
    with open(ini_path, "r") as f:
        for line in f:
            line = line.strip()
            if line.startswith("[") and line.endswith("]"):
                current_section = line[1:-1]
            elif "=" in line and current_section == section:
                k, v = line.split("=", 1)
                if k.strip() == key:
                    return v.strip()
    return None


def main():
    if len(sys.argv) < 2:
        print("Usage: mkpkg.py <modules_dir> [output_dir]")
        print("  Builds .mpkg for all modules and generates repo.idx")
        sys.exit(1)

    modules_dir = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(modules_dir, "..", "repo")

    subdirs = [d for d in os.listdir(modules_dir)
               if os.path.isdir(os.path.join(modules_dir, d))
               and not d.startswith(".")]

    if not subdirs:
        print(f"No module subdirectories found in {modules_dir}")
        sys.exit(1)

    print(f"Building packages from {modules_dir} -> {output_dir}")

    entries = []
    for name in sorted(subdirs):
        module_path = os.path.join(modules_dir, name)
        info = build_mpkg(module_path, output_dir)
        if info:
            ini_path = os.path.join(module_path, f"{name}.ini")
            version = read_ini_field(ini_path, "module", "version") or "1.0"
            description = read_ini_field(ini_path, "module", "description") or ""
            info["version"] = version
            info["description"] = description
            entries.append(info)

    idx_path = os.path.join(output_dir, "repo.idx")
    with open(idx_path, "w") as f:
        f.write("# micro_arch package repository\n")
        for e in entries:
            f.write(f"{e['name']}|{e['version']}|{e['size']}|{e['hash']}|{e['filename']}|{e['description']}\n")

    print(f"\nGenerated {idx_path} with {len(entries)} packages")
    print(f"\nTo serve: cd {output_dir} && python3 -m http.server 8080")
    print(f"On device: pkg repo http://<your-ip>:8080")
    print(f"           pkg sync")
    print(f"           pkg install <name>")


if __name__ == "__main__":
    main()
