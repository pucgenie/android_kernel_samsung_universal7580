/*
 * Based on arch/arm/include/asm/io.h
 *
 * Copyright (C) 1996-2000 Russell King
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __ASM_IO_H
#define __ASM_IO_H

#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/exynos-ss.h>

#include <asm/byteorder.h>
#include <asm/barrier.h>
#include <asm/pgtable.h>
#include <asm/early_ioremap.h>
#include <asm/alternative.h>
#include <asm/cpufeature.h>

/*
 * Generic IO read/write.  These perform native-endian accesses.
 */
static inline void __raw_writeb(u8 val, volatile void __iomem *addr)
{
	exynos_ss_reg(0, (size_t)val, (size_t)addr, ESS_FLAG_IN);
	asm volatile("strb %w0, [%1]" : : "r" (val), "r" (addr));
	exynos_ss_reg(0, (size_t)val, (size_t)addr, ESS_FLAG_OUT);
}

static inline void __raw_writew(u16 val, volatile void __iomem *addr)
{
	exynos_ss_reg(0, (size_t)val, (size_t)addr, ESS_FLAG_IN);
	asm volatile("strh %w0, [%1]" : : "r" (val), "r" (addr));
	exynos_ss_reg(0, (size_t)val, (size_t)addr, ESS_FLAG_OUT);
}

static inline void __raw_writel(u32 val, volatile void __iomem *addr)
{
	exynos_ss_reg(0, (size_t)val, (size_t)addr, ESS_FLAG_IN);
	asm volatile("str %w0, [%1]" : : "r" (val), "r" (addr));
	exynos_ss_reg(0, (size_t)val, (size_t)addr, ESS_FLAG_OUT);
}

static inline void __raw_writeq(u64 val, volatile void __iomem *addr)
{
	exynos_ss_reg(0, (size_t)val, (size_t)addr, ESS_FLAG_IN);
	asm volatile("str %0, [%1]" : : "r" (val), "r" (addr));
	exynos_ss_reg(0, (size_t)val, (size_t)addr, ESS_FLAG_OUT);
}

static inline u8 __raw_readb(const volatile void __iomem *addr)
{
	u8 val;
	exynos_ss_reg(1, 0, (size_t)addr, ESS_FLAG_IN);
	asm volatile(ALTERNATIVE("ldrb %w0, [%1]",
				 "ldarb %w0, [%1]",
				 ARM64_WORKAROUND_DEVICE_LOAD_ACQUIRE)
		     : "=r" (val) : "r" (addr));
	exynos_ss_reg(1, (size_t)val, (size_t)addr, ESS_FLAG_OUT);
	return val;
}

static inline u16 __raw_readw(const volatile void __iomem *addr)
{
	u16 val;
	exynos_ss_reg(1, 0, (size_t)addr, ESS_FLAG_IN);

	asm volatile(ALTERNATIVE("ldrh %w0, [%1]",
				 "ldarh %w0, [%1]",
				 ARM64_WORKAROUND_DEVICE_LOAD_ACQUIRE)
		     : "=r" (val) : "r" (addr));
	exynos_ss_reg(1, (size_t)val, (size_t)addr, ESS_FLAG_OUT);
	return val;
}

static inline u32 __raw_readl(const volatile void __iomem *addr)
{
	u32 val;
	exynos_ss_reg(1, 0, (size_t)addr, ESS_FLAG_IN);
	asm volatile(ALTERNATIVE("ldr %w0, [%1]",
				 "ldar %w0, [%1]",
				 ARM64_WORKAROUND_DEVICE_LOAD_ACQUIRE)
		     : "=r" (val) : "r" (addr));
	exynos_ss_reg(1, (size_t)val, (size_t)addr, ESS_FLAG_OUT);
	return val;
}

static inline u64 __raw_readq(const volatile void __iomem *addr)
{
	u64 val;
	exynos_ss_reg(1, 0, (size_t)addr, ESS_FLAG_IN);
	asm volatile(ALTERNATIVE("ldr %0, [%1]",
				 "ldar %0, [%1]",
				 ARM64_WORKAROUND_DEVICE_LOAD_ACQUIRE)
		     : "=r" (val) : "r" (addr));
	exynos_ss_reg(1, (size_t)val, (size_t)addr, ESS_FLAG_OUT);
	return val;
}

/* IO barriers */
#define __iormb()		rmb()
#define __iowmb()		wmb()

#define mmiowb()		do { } while (0)

/*
 * Relaxed I/O memory access primitives. These follow the Device memory
 * ordering rules but do not guarantee any ordering relative to Normal memory
 * accesses.
 */
#define readb_relaxed(c)	({ u8  __v = __raw_readb(c); __v; })
#define readw_relaxed(c)	({ u16 __v = le16_to_cpu((__force __le16)__raw_readw(c)); __v; })
#define readl_relaxed(c)	({ u32 __v = le32_to_cpu((__force __le32)__raw_readl(c)); __v; })
#define readq_relaxed(c)	({ u64 __v = le64_to_cpu((__force __le64)__raw_readq(c)); __v; })

#define writeb_relaxed(v,c)	((void)__raw_writeb((v),(c)))
#define writew_relaxed(v,c)	((void)__raw_writew((__force u16)cpu_to_le16(v),(c)))
#define writel_relaxed(v,c)	((void)__raw_writel((__force u32)cpu_to_le32(v),(c)))
#define writeq_relaxed(v,c)	((void)__raw_writeq((__force u64)cpu_to_le64(v),(c)))

/*
 * I/O memory access primitives. Reads are ordered relative to any
 * following Normal memory access. Writes are ordered relative to any prior
 * Normal memory access.
 */
#define readb(c)		({ u8  __v = readb_relaxed(c); __iormb(); __v; })
#define readw(c)		({ u16 __v = readw_relaxed(c); __iormb(); __v; })
#define readl(c)		({ u32 __v = readl_relaxed(c); __iormb(); __v; })
#define readq(c)		({ u64 __v = readq_relaxed(c); __iormb(); __v; })

#define writeb(v,c)		({ __iowmb(); writeb_relaxed((v),(c)); })
#define writew(v,c)		({ __iowmb(); writew_relaxed((v),(c)); })
#define writel(v,c)		({ __iowmb(); writel_relaxed((v),(c)); })
#define writeq(v,c)		({ __iowmb(); writeq_relaxed((v),(c)); })

/*
 * A typesafe __io() helper
 */
static inline void __iomem *__typesafe_io(unsigned long addr)
{
	return (void __iomem *)addr;
}

/*
 *  I/O port access primitives.
 */
#define PCI_IOBASE		((void __iomem *)(MODULES_VADDR - SZ_32M))
#if defined(CONFIG_PCI)
#define IO_SPACE_LIMIT  ((resource_size_t)0xffffffff)
#define __io(a)         __typesafe_io((unsigned long)PCI_IOBASE + \
				      ((a) & IO_SPACE_LIMIT))
#else
#define IO_SPACE_LIMIT	0xffff
#define __io(a)         __typesafe_io((a) & IO_SPACE_LIMIT)
#endif
extern void __iomem *ioport_map(unsigned long port, unsigned int nr);
extern void ioport_unmap(void __iomem *addr);

static inline u8 inb(unsigned long addr)
{
	return readb(addr + PCI_IOBASE);
}

static inline u16 inw(unsigned long addr)
{
	return readw(addr + PCI_IOBASE);
}

static inline u32 inl(unsigned long addr)
{
	return readl(addr + PCI_IOBASE);
}

static inline void outb(u8 b, unsigned long addr)
{
	writeb(b, addr + PCI_IOBASE);
}

static inline void outw(u16 b, unsigned long addr)
{
	writew(b, addr + PCI_IOBASE);
}

static inline void outl(u32 b, unsigned long addr)
{
	writel(b, addr + PCI_IOBASE);
}

#define inb_p(addr)	inb(addr)
#define inw_p(addr)	inw(addr)
#define inl_p(addr)	inl(addr)

#define outb_p(x, addr)	outb((x), (addr))
#define outw_p(x, addr)	outw((x), (addr))
#define outl_p(x, addr)	outl((x), (addr))

static inline void insb(unsigned long addr, void *buffer, int count)
{
	u8 *buf = buffer;
	while (count--)
		*buf++ = __raw_readb(addr + PCI_IOBASE);
}

static inline void insw(unsigned long addr, void *buffer, int count)
{
	u16 *buf = buffer;
	while (count--)
		*buf++ = __raw_readw(addr + PCI_IOBASE);
}

static inline void insl(unsigned long addr, void *buffer, int count)
{
	u32 *buf = buffer;
	while (count--)
		*buf++ = __raw_readl(addr + PCI_IOBASE);
}

static inline void outsb(unsigned long addr, const void *buffer, int count)
{
	const u8 *buf = buffer;
	while (count--)
		__raw_writeb(*buf++, addr + PCI_IOBASE);
}

static inline void outsw(unsigned long addr, const void *buffer, int count)
{
	const u16 *buf = buffer;
	while (count--)
		__raw_writew(*buf++, addr + PCI_IOBASE);
}

static inline void outsl(unsigned long addr, const void *buffer, int count)
{
	const u32 *buf = buffer;
	while (count--)
		__raw_writel(*buf++, addr + PCI_IOBASE);
}

#define insb_p(port,to,len)	insb(port,to,len)
#define insw_p(port,to,len)	insw(port,to,len)
#define insl_p(port,to,len)	insl(port,to,len)

#define outsb_p(port,from,len)	outsb(port,from,len)
#define outsw_p(port,from,len)	outsw(port,from,len)
#define outsl_p(port,from,len)	outsl(port,from,len)

/*
 * String version of I/O memory access operations.
 */
extern void __memcpy_fromio(void *, const volatile void __iomem *, size_t);
extern void __memcpy_toio(volatile void __iomem *, const void *, size_t);
extern void __memset_io(volatile void __iomem *, int, size_t);

#define memset_io(c,v,l)	__memset_io((c),(v),(l))
#define memcpy_fromio(a,c,l)	__memcpy_fromio((a),(c),(l))
#define memcpy_toio(c,a,l)	__memcpy_toio((c),(a),(l))

/*
 * I/O memory mapping functions.
 */
extern void __iomem *__ioremap(phys_addr_t phys_addr, size_t size, pgprot_t prot);
extern void __iounmap(volatile void __iomem *addr);
extern void __iomem *ioremap_cache(phys_addr_t phys_addr, size_t size);

#define ioremap(addr, size)		__ioremap((addr), (size), __pgprot(PROT_DEVICE_nGnRE))
#define ioremap_nocache(addr, size)	__ioremap((addr), (size), __pgprot(PROT_DEVICE_nGnRE))
#define ioremap_wc(addr, size)		__ioremap((addr), (size), __pgprot(PROT_NORMAL_NC))
#define iounmap				__iounmap

#define ARCH_HAS_IOREMAP_WC
#include <asm-generic/iomap.h>

/*
 * More restrictive address range checking than the default implementation
 * (PHYS_OFFSET and PHYS_MASK taken into account).
 */
#define ARCH_HAS_VALID_PHYS_ADDR_RANGE
extern int valid_phys_addr_range(unsigned long addr, size_t size);
extern int valid_mmap_phys_addr_range(unsigned long pfn, size_t size);

extern int devmem_is_allowed(unsigned long pfn);

/*
 * Convert a physical pointer to a virtual kernel pointer for /dev/mem
 * access
 */
#define xlate_dev_mem_ptr(p)	__va(p)

/*
 * Convert a virtual cached pointer to an uncached pointer
 */
#define xlate_dev_kmem_ptr(p)	p

/*
 * Architecture ioremap implementation.
 */
#define MT_DEVICE		0
#define MT_DEVICE_NONSHARED	1
#define MT_DEVICE_CACHED	2
#define MT_DEVICE_WC		3

#endif	/* __KERNEL__ */
#endif	/* __ASM_IO_H */
