# PlatformIO extra script for the rescue env (flash layout 2, partitions.csv).
#
# Rescue lives in the factory partition. It's uploaded without touching otadata, so the
# bootloader keeps booting whatever it booted before (the launcher or a cartridge).

import csv
import os

Import("env")  # noqa: F821


def partition(csv_path, subtype):
    with open(csv_path) as f:
        for row in csv.reader(line for line in f if line.strip() and not line.startswith("#")):
            fields = [c.strip() for c in row]
            if len(fields) >= 4 and fields[2] == subtype:
                return fields[0], fields[3]
    raise ValueError(f"no '{subtype}' partition in {csv_path}")


partitions = os.path.join(env.subst("$PROJECT_DIR"), env.GetProjectOption("board_build.partitions"))
name, offset = partition(partitions, "factory")


def use_rescue_offset(target, source, env):
    env.Replace(ESP32_APP_OFFSET=offset)


env.Replace(ESP32_APP_OFFSET=offset)
env.AddPreAction("$BUILD_DIR/${PROGNAME}.bin", use_rescue_offset)
env.AddPreAction("checkprogsize", use_rescue_offset)
env.AddPreAction("upload", use_rescue_offset)
env.BoardConfig().update("build.app_partition_name", name)



# The platform copies FLASH_EXTRA_IMAGES (with boot_app0.bin) into the esptool flags as soon
# as it sets up uploading: drop that offset/image pair from the flags themselves.
def keep_otadata(target, source, env):
    flags = list(env.get("UPLOADERFLAGS", []))
    for i, flag in enumerate(flags):
        if os.path.basename(str(flag)) == "boot_app0.bin":
            del flags[i - 1:i + 1]
            break
    env.Replace(UPLOADERFLAGS=flags)


env.AddPreAction("upload", keep_otadata)
