/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sysdeps.h"
#include "user_strings.h"

user_string_def platform_strings[] = {
    {STR_EXTFS_VOLUME_NAME, "Host"},
    {-1, nullptr}
};
const char *GetString(int number)
{
    // Resolve core diagnostics without consulting Windows registry or desktop services
    for (unsigned i = 0; platform_strings[i].num >= 0; ++i)
        if (platform_strings[i].num == number) return platform_strings[i].str;
    for (unsigned i = 0; common_strings[i].num >= 0; ++i)
        if (common_strings[i].num == number) return common_strings[i].str;
    return "Unknown SheepShaver diagnostic";
}
