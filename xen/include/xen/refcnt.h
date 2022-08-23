#ifndef __XEN_REFCNT_H__
#define __XEN_REFCNT_H__

#include <asm/atomic.h>

typedef atomic_t refcnt_t;

static inline void refcnt_init(refcnt_t *refcnt)
{
	atomic_set(refcnt, 1);
}

static inline void refcnt_get(refcnt_t *refcnt)
{
#ifndef NDEBUG
	ASSERT(atomic_add_unless(refcnt, 1, 0) > 0);
#else
	atomic_add_unless(refcnt, 1, 0);
#endif
}

static inline void refcnt_put(refcnt_t *refcnt, void (*destructor)(refcnt_t *refcnt))
{
	if ( atomic_dec_and_test(refcnt) )
		destructor(refcnt);
}

#endif
