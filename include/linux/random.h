/*
 * include/linux/random.h
 *
 * Include file for the random number generator.
 */
#ifndef _LINUX_RANDOM_H
#define _LINUX_RANDOM_H

#include <uapi/linux/random.h>

extern void add_device_randomness(const void *, unsigned int);
extern void add_input_randomness(unsigned int type, unsigned int code,
				 unsigned int value);
extern void add_interrupt_randomness(int irq, int irq_flags);

extern void get_random_bytes(void *buf, int nbytes);
extern void get_random_bytes_arch(void *buf, int nbytes);
extern int random_int_secret_init(void);

#ifndef MODULE
extern const struct file_operations random_fops, urandom_fops;
#endif

unsigned int get_random_int(void);
unsigned long get_random_long(void);
unsigned long randomize_range(unsigned long start, unsigned long end, unsigned long len);

/*
 * On 64-bit architectures, protect against non-terminated C string overflows
 * by zeroing out the first byte of the canary; this leaves 56 bits of entropy.
 */
#ifdef CONFIG_64BIT
# ifdef __LITTLE_ENDIAN
#  define CANARY_MASK 0xffffffffffffff00UL
# else /* big endian, 64 bits: */
#  define CANARY_MASK 0x00ffffffffffffffUL
# endif
#else /* 32 bits: */
# define CANARY_MASK 0xffffffffUL
#endif

static inline unsigned long get_random_canary(void)
{
	unsigned long val = get_random_long();

	return val & CANARY_MASK;
}

u32 prandom_u32(void);
void prandom_bytes(void *buf, int nbytes);
void prandom_seed(u32 seed);
void prandom_reseed_late(void);

u32 prandom_u32_state(struct rnd_state *);
void prandom_bytes_state(struct rnd_state *state, void *buf, int nbytes);

/**
 * prandom_u32_max - returns a pseudo-random number in interval [0, ep_ro)
 * @ep_ro: right open interval endpoint
 *
 * Returns a pseudo-random number that is in interval [0, ep_ro). Note
 * that the result depends on PRNG being well distributed in [0, ~0U]
 * u32 space. Here we use maximally equidistributed combined Tausworthe
 * generator, that is, prandom_u32(). This is useful when requesting a
 * random index of an array containing ep_ro elements, for example.
 *
 * Returns: pseudo-random number in interval [0, ep_ro)
 */
static inline u32 prandom_u32_max(u32 ep_ro)
{
	return (u32)(((u64) prandom_u32() * ep_ro) >> 32);
}

/*
 * Handle minimum values for seeds
 */
static inline u32 __seed(u32 x, u32 m)
{
	return (x < m) ? x + m : x;
}

/**
 * prandom_seed_state - set seed for prandom_u32_state().
 * @state: pointer to state structure to receive the seed.
 * @seed: arbitrary 64-bit value to use as a seed.
 */
static inline void prandom_seed_state(struct rnd_state *state, u64 seed)
{
	u32 i = ((seed >> 32) ^ (seed << 10) ^ seed) & 0xffffffffUL;

	state->s1 = __seed(i, 2);
	state->s2 = __seed(i, 8);
	state->s3 = __seed(i, 16);
}

#ifdef CONFIG_ARCH_RANDOM
# include <asm/archrandom.h>
#else
static inline int arch_get_random_long(unsigned long *v)
{
	return 0;
}
static inline int arch_get_random_int(unsigned int *v)
{
	return 0;
}
#endif

/* Pseudo random number generator from numerical recipes. */
static inline u32 next_pseudo_random32(u32 seed)
{
	return seed * 1664525 + 1013904223;
}

#endif /* _LINUX_RANDOM_H */
