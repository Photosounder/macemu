/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sysdeps.h"
#include "../CrossPlatform/sigsegv.cpp"

bool wahe_skip_fault(EXCEPTION_POINTERS *ExceptionInfo)
{
    // Reuse the existing instruction decoder with the context supplied by our scoped vectored handler
    return SIGSEGV_SKIP_INSTRUCTION(SIGSEGV_REGISTER_FILE);
}
