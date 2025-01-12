#ifndef _M68K_BYTEORDER_H
#define _M68K_BYTEORDER_H

#include <asm/types.h>
#include <linux/compiler.h>

#define __BIG_ENDIAN
#define __SWAB_64_THRU_32__

static inline __attribute_const__ __u32 __arch_swab32(__u32 val)
{
	__asm__("rolw #8,%0; swap %0; rolw #8,%0" : "=d" (val) : "0" (val));
	return val;
}
#define __arch_swab32 __arch_swab32

#include <linux/byteorder.h>

#endif /* _M68K_BYTEORDER_H */
