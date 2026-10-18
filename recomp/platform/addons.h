#ifndef FCOP_ADDONS_H
#define FCOP_ADDONS_H

#include "host.h"

int addons_enabled(void);
void addons_set_enabled(int on);
u8 *addons_patch_file(const char *host_path, u32 *len);

/* gdi_misc.c registry helpers (defined in gdi_misc.c, declared here) */
int reg_host_get_dword(const char *key_path, const char *value, u32 *out);
void reg_host_set_dword(const char *key_path, const char *value, u32 v);

#endif
