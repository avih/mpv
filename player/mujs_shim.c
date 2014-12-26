#include "config.h"

// If only one of the backends is present, then javascript.c is already
// compiled for it, however, if both are present, then javascript.c
// is compiled with Duktape, so we need to compile it again for MuJS.
#if HAVE_DUKTAPE && HAVE_MUJS
  #define MUD_USE_DUK 0
  #include "javascript.c"
#endif
