# PlatformIO extra script for the launcher env (flash layout 2, partitions.csv).
#
# Cartridge envs upload to ota_0 (PlatformIO's default when the partition table has
# one) together with boot_app0.bin, which tells the bootloader to boot ota_0.
# The launcher lives in ota_1 instead, and is uploaded with an otadata that selects ota_1,
# so the bootloader boots the launcher rather than an installed cartridge.

import csv
import os
import struct
import zlib

Import("env")  # noqa: F821


def partition(csv_path, subtype):
    with open(csv_path) as f:
        for row in csv.reader(line for line in f if line.strip() and not line.startswith("#")):
            fields = [c.strip() for c in row]
            if len(fields) >= 4 and fields[2] == subtype:
                return fields[0], fields[3]
    raise ValueError(f"no '{subtype}' partition in {csv_path}")


partitions = os.path.join(env.subst("$PROJECT_DIR"), env.GetProjectOption("board_build.partitions"))
name, offset = partition(partitions, "ota_1")


# The platform recomputes ESP32_APP_OFFSET (as the ota_0 offset) while building, so
# set it back right before each step that uses it. Actions run in the order added,
# so these run after the platform's own.
def use_launcher_offset(target, source, env):
    env.Replace(ESP32_APP_OFFSET=offset)


env.Replace(ESP32_APP_OFFSET=offset)
env.AddPreAction("$BUILD_DIR/${PROGNAME}.bin", use_launcher_offset)
env.AddPreAction("checkprogsize", use_launcher_offset)
env.AddPreAction("upload", use_launcher_offset)
# Size check against the launcher's partition, not ota_0.
env.BoardConfig().update("build.app_partition_name", name)

# otadata selecting ota_1: entry 0 = {ota_seq 2, seq_label, ota_state, crc}; the bootloader
# boots OTA slot (seq - 1) % 2 = 1. crc = CRC-32 of ota_seq (as boot_app0.bin's for seq 1).
# ota_state 0xFFFFFFFF (undefined) counts as valid: no trial for a USB-flashed launcher.
build_dir = env.subst("$BUILD_DIR")
os.makedirs(build_dir, exist_ok=True)
otadata = os.path.join(build_dir, "otadata_launcher.bin")
seq = 2
entry = struct.pack("<I", seq) + b"\xff" * 20 + struct.pack("<I", 0xFFFFFFFF) + \
    struct.pack("<I", zlib.crc32(struct.pack("<I", seq), 0xFFFFFFFF))
with open(otadata, "wb") as f:
    f.write(entry + b"\xff" * (0x1000 - len(entry)) + b"\xff" * 0x1000)



# The platform copies FLASH_EXTRA_IMAGES (with boot_app0.bin, which boots ota_0) into the
# esptool flags as soon as it sets up uploading, so swap it in the flags themselves.
def use_launcher_otadata(target, source, env):
    env.Replace(UPLOADERFLAGS=[
        otadata if os.path.basename(str(flag)) == "boot_app0.bin" else flag
        for flag in env.get("UPLOADERFLAGS", [])
    ])


env.AddPreAction("upload", use_launcher_otadata)
