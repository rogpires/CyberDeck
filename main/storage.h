#pragma once

#include <stdbool.h>

#define SD_MOUNT_POINT "/sd"

// Mounts the microSD card (FAT) at /sd
bool storage_init(void);
bool storage_is_mounted(void);
