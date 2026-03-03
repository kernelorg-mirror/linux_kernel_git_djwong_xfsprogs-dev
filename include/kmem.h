// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2008 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __KMEM_H__
#define __KMEM_H__

#define KM_NOFS		0x0004u
#define KM_MAYFAIL	0x0008u
#define KM_LARGE	0x0010u
#define KM_NOLOCKDEP	0x0020u

struct kmem_cache {
	int		cache_unitsize;	/* Size in bytes of cache unit */
	int		allocated;	/* debug: How many allocated? */
	unsigned int	align;
	const char	*cache_name;	/* tag name */
	void		(*ctor)(void *);
};

typedef unsigned int __bitwise gfp_t;

#define GFP_KERNEL	((__force gfp_t)0)
#define GFP_NOFS	((__force gfp_t)0)
#define __GFP_NOFAIL	((__force gfp_t)0)
#define __GFP_NOLOCKDEP	((__force gfp_t)0)
#define __GFP_RETRY_MAYFAIL	((__force gfp_t)0)

#define __GFP_ZERO	((__force gfp_t)1)

struct kmem_cache * kmem_cache_create(const char *name, unsigned int size,
		unsigned int align, unsigned int slab_flags,
		void (*ctor)(void *));

static inline struct kmem_cache *
kmem_cache_init(unsigned int size, const char *name)
{
	return kmem_cache_create(name, size, 0, 0, NULL);
}

extern void	*kmem_cache_alloc(struct kmem_cache *, gfp_t);
extern void	*kmem_cache_zalloc(struct kmem_cache *, gfp_t);
extern int	kmem_cache_destroy(struct kmem_cache *);

static inline void
kmem_cache_free(struct kmem_cache *cache, void *ptr)
{
	cache->allocated--;
	free(ptr);
}

extern void	*kvmalloc(size_t, gfp_t);
extern void	*krealloc(void *, size_t, int);

static inline void *kmalloc(size_t size, gfp_t flags)
{
	return kvmalloc(size, flags);
}

#define kzalloc(size, gfp)	kvmalloc((size), (gfp) | __GFP_ZERO)
#define kvzalloc(size, gfp)	kzalloc((size), (gfp))

/**
 * kmalloc_array - allocate memory for an array.
 * @n: number of elements.
 * @size: element size.
 * @flags: the type of memory to allocate (see kmalloc).
 */
static inline void *kmalloc_array(size_t n, size_t size, gfp_t flags)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;
	return kmalloc(bytes, flags);
}
#define kcalloc(n, size, gfp)	kmalloc_array((n), (size), (gfp) | __GFP_ZERO)

/**
 * size_mul() - Calculate size_t multiplication with saturation at SIZE_MAX
 * @factor1: first factor
 * @factor2: second factor
 *
 * Returns: calculate @factor1 * @factor2, both promoted to size_t,
 * with any overflow causing the return value to be SIZE_MAX. The
 * lvalue must be size_t to avoid implicit type conversion.
 */
static inline size_t __must_check size_mul(size_t factor1, size_t factor2)
{
	size_t bytes;

	if (check_mul_overflow(factor1, factor2, &bytes))
		return SIZE_MAX;

	return bytes;
}

#define __alloc_objs(KMALLOC, GFP, TYPE, COUNT)				\
({									\
	const size_t __obj_size = size_mul(sizeof(TYPE), COUNT);	\
	(TYPE *)KMALLOC(__obj_size, GFP);				\
})

/* Helper macro to avoid gfp flags if they are the default one */
#define __default_gfp(a,b,...) b
#define default_gfp(...) __default_gfp(,##__VA_ARGS__,GFP_KERNEL)

#define kzalloc_obj(P, ...) \
	__alloc_objs(kzalloc, default_gfp(__VA_ARGS__), typeof(P), 1)
#define kmalloc_obj(VAR_OR_TYPE, ...) \
	__alloc_objs(kmalloc, default_gfp(__VA_ARGS__), typeof(VAR_OR_TYPE), 1)

static inline void kfree(const void *ptr)
{
	free((void *)ptr);
}

static inline void kvfree(const void *ptr)
{
	kfree(ptr);
}

static inline void kfree_rcu_mightsleep(const void *ptr)
{
	kfree(ptr);
}

__attribute__((format(printf,2,3)))
char *kasprintf(gfp_t gfp, const char *fmt, ...);

static inline void *kmemdup(const void *src, size_t len, gfp_t gfp)
{
	void *p = kmalloc(len, gfp);

	if (p)
		memcpy(p, src, len);
	return p;
}

#endif
