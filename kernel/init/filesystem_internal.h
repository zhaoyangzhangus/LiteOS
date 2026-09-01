#pragma once

#include <kernel/bootinfo.h>

BOOLEAN filesystem_vfs_file_api_self_test(void);
BOOLEAN filesystem_fat32_self_test(void);
BOOLEAN filesystem_file_mapping_self_test(void);
