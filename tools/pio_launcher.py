# PlatformIO extra script for the launcher env.
#
# Cartridge envs upload to ota_0 (PlatformIO's default when the partition table has
# one) together with boot_app0.bin, which tells the bootloader to boot ota_0.
# The launcher lives in the factory partition instead, and is uploaded with a blank
# otadata so the bootloader boots the launcher rather than an installed cartridge.

import csv
import os

Import("env")  # noqa: F821


def partition_offset(csv_path, subtype):
    with open(csv_path) as f:
        for row in csv.reader(line for line in f if line.strip() and not line.startswith("#")):
            fields = [c.strip() for c in row]
            if len(fields) >= 4 and fields[2] == subtype:
                return fields[3]
    raise ValueError(f"no '{subtype}' partition in {csv_path}")


partitions = os.path.join(env.subst("$PROJECT_DIR"), env.GetProjectOption("board_build.partitions"))
factory_offset = partition_offset(partitions, "factory")


# The platform recomputes ESP32_APP_OFFSET (as the ota_0 offset) while building, so
# set it back right before each step that uses it. Actions run in the order added,
# so these run after the platform's own.
def use_factory_offset(target, source, env):
    env.Replace(ESP32_APP_OFFSET=factory_offset)


env.Replace(ESP32_APP_OFFSET=factory_offset)
env.AddPreAction("$BUILD_DIR/${PROGNAME}.bin", use_factory_offset)
env.AddPreAction("checkprogsize", use_factory_offset)
env.AddPreAction("upload", use_factory_offset)
# Size check against the factory partition, not ota_0.
env.BoardConfig().update("build.app_partition_name", "factory")

build_dir = env.subst("$BUILD_DIR")
os.makedirs(build_dir, exist_ok=True)
blank_otadata = os.path.join(build_dir, "otadata_blank.bin")
with open(blank_otadata, "wb") as f:
    f.write(b"\xff" * 0x2000)

env.Replace(FLASH_EXTRA_IMAGES=[
    (offset, blank_otadata if os.path.basename(path) == "boot_app0.bin" else path)
    for offset, path in env.get("FLASH_EXTRA_IMAGES", [])
])
