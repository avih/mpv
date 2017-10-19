/* This file should be at <tcc-prefix>/lib/tcc/include/asm-generic/ioctl.h */

#ifdef __TINYC__
/* In Alpine linux, musl's <sys/ioctl.h> and Linux's <asm-generic/ioctl.h>
 * (e.g. the latter included from the linux <sys/soundcard.h>) both define
 * the following macros. They're equivalent so gcc doesn't warn, but tcc
 * does. Undef them for tcc before the linux header.
 */

#ifdef _IO
#  undef _IO
#endif

#ifdef _IOC
#  undef _IOC
#endif

#ifdef _IOW
#  undef _IOW
#endif

#ifdef _IOR
#  undef _IOR
#endif

#ifdef _IOWR
#  undef _IOWR
#endif

#endif /* __TINYC__ */

#include_next <asm-generic/ioctl.h>

