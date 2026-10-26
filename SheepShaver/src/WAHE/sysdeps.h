/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SHEEPSHAVER_WAHE_SYSDEPS_H
#define SHEEPSHAVER_WAHE_SYSDEPS_H

#define SHEEPSHAVER_WAHE 1
#include "../Windows/sysdeps.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>

#undef USE_SDL
#undef USE_SDL2
#undef USE_SDL3
#undef USE_SDL_VIDEO
#undef USE_SDL_AUDIO
#undef PRECISE_TIMING
#undef PRECISE_TIMING_WINDOWS
#define PRECISE_TIMING 0
#define PRECISE_TIMING_WINDOWS 0
#define SHEEPSHAVER_WAHE 1

int wahe_printf(const char *, ...);
int wahe_fprintf(FILE *, const char *, ...);
int wahe_fflush(FILE *);
FILE *wahe_fopen(const char *, const char *);
int wahe_fclose(FILE *);
char *wahe_fgets(char *, int, FILE *);

#define printf wahe_printf
#define fprintf wahe_fprintf
#define fflush wahe_fflush
#define fopen wahe_fopen
#define fclose wahe_fclose
#define fgets wahe_fgets
#endif
