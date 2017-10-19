/*  this file should be at <tcc-prefix>/lib/tcc/include/math.h  */

#include_next <math.h>

#ifdef __TINYC__
/* musl's math.h defines NAN as (0.0f/0.0f) for non-gcc, which is arguably an
 * invalid static initializer. Define to tcc's __nan__ after musl's math.h .
 */

#ifdef NAN
#  undef NAN
#endif

#define NAN __nan__

#endif /* __TINYC__ */
