/*
 *
 * (C) COPYRIGHT 2010-2015 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */





/**
 * @file mali_kbase_mem_linux.c
 * Base kernel memory APIs, Linux implementation.
 */

#include <linux/compat.h>
#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/version.h>
#include <linux/dma-mapping.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0))
	#include <linux/dma-attrs.h>
#endif
#ifdef CONFIG_DMA_SHARED_BUFFER
#include <linux/dma-buf.h>
#endif				/* defined(CONFIG_DMA_SHARED_BUFFER) */

#include <mali_kbase.h>
#include <mali_kbase_mem_linux.h>
#include <mali_kbase_config_defaults.h>
#include <mali_kbase_hwaccess_time.h>

static int kbase_tracking_page_setup(struct kbase_context *kctx, struct vm_area_struct *vma);
static const struct vm_operations_struct kbase_vm_ops;

struct kbase_va_region *kbase_mem_alloc(struct kbase_context *kctx, u64 va_pages, u64 commit_pages, u64 extent, u64 *flags, u64 *gpu_va, u16 *va_alignment)
{
	int zone;
	int gpu_pc_bits;
	int cpu_va_bits;
	struct kbase_va_region *reg;
	struct device *dev;

	KBASE_DEBUG_ASSERT(kctx);
	KBASE_DEBUG_ASSERT(flags);
	KBASE_DEBUG_ASSERT(gpu_va);
	KBASE_DEBUG_ASSERT(va_alignment);

	dev = kctx->kbdev->dev;
	*va_alignment = 0; /* no alignment by default */
	*gpu_va = 0; /* return 0 on failure */

	gpu_pc_bits = kctx->kbdev->gpu_props.props.core_props.log2_program_counter_size;
	cpu_va_bits = BITS_PER_LONG;

	if (0 == va_pages) {
		dev_warn(dev, "kbase_mem_alloc called with 0 va_pages!");
		goto bad_size;
	}

	if (va_pages > (U64_MAX / PAGE_SIZE))
		/* 64-bit address range is the max */
		goto bad_size;

#if defined(CONFIG_64BIT)
	if (kctx->is_compat)
		cpu_va_bits = 32;
	else
		/* force SAME_VA if a 64-bit client */
		*flags |= BASE_MEM_SAME_VA;
#endif

	if (!kbase_check_alloc_flags(*flags)) {
		dev_warn(dev,
				"kbase_mem_alloc called with bad flags (%llx)",
				(unsigned long long)*flags);
		goto bad_flags;
	}

	if ((*flags & BASE_MEM_COHERENT_SYSTEM_REQUIRED) != 0 &&
			kctx->kbdev->system_coherency != COHERENCY_ACE) {
		dev_warn(dev, "kbase_mem_alloc call required coherent mem when unavailable");
		goto bad_flags;
	}
	if ((*flags & BASE_MEM_COHERENT_SYSTEM) != 0 &&
			kctx->kbdev->system_coherency != COHERENCY_ACE) {
		/* Remove COHERENT_SYSTEM flag if coherent mem is unavailable */
		*flags &= ~BASE_MEM_COHERENT_SYSTEM;
	}

	/* Limit GPU executable allocs to GPU PC size */
	if ((*flags & BASE_MEM_PROT_GPU_EX) &&
	    (va_pages > (1ULL << gpu_pc_bits >> PAGE_SHIFT)))
		goto bad_ex_size;

	/* find out which VA zone to use */
	if (*flags & BASE_MEM_SAME_VA)
		zone = KBASE_REG_ZONE_SAME_VA;
	else if (*flags & BASE_MEM_PROT_GPU_EX)
		zone = KBASE_REG_ZONE_EXEC;
	else
		zone = KBASE_REG_ZONE_CUSTOM_VA;

	reg = kbase_alloc_free_region(kctx, 0, va_pages, zone);
	if (!reg) {
		dev_err(dev, "Failed to allocate free region");
		goto no_region;
	}

	kbase_update_region_flags(kctx, reg, *flags);

	if (kbase_reg_prepare_native(reg, kctx) != 0) {
		dev_err(dev, "Failed to prepare region");
		goto prepare_failed;
	}

	if (*flags & BASE_MEM_GROW_ON_GPF)
		reg->extent = extent;
	else
		reg->extent = 0;

	if (kbase_alloc_phy_pages(reg, va_pages, commit_pages) != 0) {
		dev_warn(dev, "Failed to allocate %lld pages (va_pages=%lld)",
				(unsigned long long)commit_pages,
				(unsigned long long)va_pages);
		goto no_mem;
	}

	kbase_gpu_vm_lock(kctx);

	/* mmap needed to setup VA? */
	if (*flags & BASE_MEM_SAME_VA) {
		/* Bind to a cookie */
		if (!kctx->cookies) {
			dev_err(dev, "No cookies available for allocation!");
			goto no_cookie;
		}
		/* return a cookie */
		*gpu_va = __ffs(kctx->cookies);
		kctx->cookies &= ~(1UL << *gpu_va);
		BUG_ON(kctx->pending_regions[*gpu_va]);
		kctx->pending_regions[*gpu_va] = reg;

		/* relocate to correct base */
		*gpu_va += PFN_DOWN(BASE_MEM_COOKIE_BASE);
		*gpu_va <<= PAGE_SHIFT;

		/* See if we must align memory due to GPU PC bits vs CPU VA */
		if ((*flags & BASE_MEM_PROT_GPU_EX) &&
		    (cpu_va_bits > gpu_pc_bits)) {
			*va_alignment = gpu_pc_bits;
			reg->flags |= KBASE_REG_ALIGNED;
		}
	} else /* we control the VA */ {
		if (kbase_gpu_mmap(kctx, reg, 0, va_pages, 1) != 0) {
			dev_warn(dev, "Failed to map memory on GPU");
			goto no_mmap;
		}
		/* return real GPU VA */
		*gpu_va = reg->start_pfn << PAGE_SHIFT;
	}

	kbase_gpu_vm_unlock(kctx);
	return reg;

no_mmap:
no_cookie:
	kbase_gpu_vm_unlock(kctx);
no_mem:
	kbase_mem_phy_alloc_put(reg->cpu_alloc);
	kbase_mem_phy_alloc_put(reg->gpu_alloc);
prepare_failed:
	kfree(reg);
no_region:
bad_ex_size:
bad_flags:
bad_size:
	return NULL;
}
KBASE_EXPORT_TEST_API(kbase_mem_alloc);

int kbase_mem_query(struct kbase_context *kctx, u64 gpu_addr, int query, u64 * const out)
{
	struct kbase_va_region *reg;
	int ret = -EINVAL;

	KBASE_DEBUG_ASSERT(kctx);
	KBASE_DEBUG_ASSERT(out);

	kbase_gpu_vm_lock(kctx);

	/* Validate the region */
	reg = kbase_region_tracker_find_region_base_address(kctx, gpu_addr);
	if (!reg || (reg->flags & KBASE_REG_FREE))
		goto out_unlock;

	switch (query) {
	case KBASE_MEM_QUERY_COMMIT_SIZE:
		if (reg->cpu_alloc->type != KBASE_MEM_TYPE_ALIAS) {
			*out = kbase_reg_current_backed_size(reg);
		} else {
			size_t i;
			struct kbase_aliased *aliased;
			*out = 0;
			aliased = reg->cpu_alloc->imported.alias.aliased;
			for (i = 0; i < reg->cpu_alloc->imported.alias.nents; i++)
				*out += aliased[i].length;
		}
		break;
	case KBASE_MEM_QUERY_VA_SIZE:
		*out = reg->nr_pages;
		break;
	case KBASE_MEM_QUERY_FLAGS:
	{
		*out = 0;
		if (KBASE_REG_CPU_WR & reg->flags)
			*out |= BASE_MEM_PROT_CPU_WR;
		if (KBASE_REG_CPU_RD & reg->flags)
			*out |= BASE_MEM_PROT_CPU_RD;
		if (KBASE_REG_CPU_CACHED & reg->flags)
			*out |= BASE_MEM_CACHED_CPU;
		if (KBASE_REG_GPU_WR & reg->flags)
			*out |= BASE_MEM_PROT_GPU_WR;
		if (KBASE_REG_GPU_RD & reg->flags)
			*out |= BASE_MEM_PROT_GPU_RD;
		if (!(KBASE_REG_GPU_NX & reg->flags))
			*out |= BASE_MEM_PROT_GPU_EX;
		if (KBASE_REG_SHARE_BOTH & reg->flags)
			*out |= BASE_MEM_COHERENT_SYSTEM;
		if (KBASE_REG_SHARE_IN & reg->flags)
			*out |= BASE_MEM_COHERENT_LOCAL;
		break;
	}
	default:
		*out = 0;
		goto out_unlock;
	}

	ret = 0;

out_unlock:
	kbase_gpu_vm_unlock(kctx);
	return ret;
}

int kbase_mem_flags_change(struct kbase_context *kctx, u64 gpu_addr, unsigned int flags, unsigned int mask)
{
	struct kbase_va_region *reg;
	int ret = -EINVAL;
	unsigned int real_flags = 0;
	unsigned int prev_flags = 0;

	KBASE_DEBUG_ASSERT(kctx);

	if (!gpu_addr)
		return -EINVAL;

	/* nuke other bits */
	flags &= mask;

	/* check for only supported flags */
	if (flags & ~(BASE_MEM_COHERENT_SYSTEM | BASE_MEM_COHERENT_LOCAL))
		goto out;

	/* mask covers bits we don't support? */
	if (mask & ~(BASE_MEM_COHERENT_SYSTEM | BASE_MEM_COHERENT_LOCAL))
		goto out;

	/* convert flags */
	if (BASE_MEM_COHERENT_SYSTEM & flags)
		real_flags |= KBASE_REG_SHARE_BOTH;
	else if (BASE_MEM_COHERENT_LOCAL & flags)
		real_flags |= KBASE_REG_SHARE_IN;

	/* now we can lock down the context, and find the region */
	kbase_gpu_vm_lock(kctx);

	/* Validate the region */
	reg = kbase_region_tracker_find_region_base_address(kctx, gpu_addr);
	if (!reg || (reg->flags & KBASE_REG_FREE))
		goto out_unlock;

	/* limit to imported memory */
	if ((reg->gpu_alloc->type != KBASE_MEM_TYPE_IMPORTED_UMP) &&
	     (reg->gpu_alloc->type != KBASE_MEM_TYPE_IMPORTED_UMM))
		goto out_unlock;

	/* no change? */
	if (real_flags == (reg->flags & (KBASE_REG_SHARE_IN | KBASE_REG_SHARE_BOTH))) {
		ret = 0;
		goto out_unlock;
	}

	/* save for roll back */
	prev_flags = reg->flags;
	reg->flags &= ~(KBASE_REG_SHARE_IN | KBASE_REG_SHARE_BOTH);
	reg->flags |= real_flags;

	/* Currently supporting only imported memory */
	switch (reg->gpu_alloc->type) {
#ifdef CONFIG_UMP
	case KBASE_MEM_TYPE_IMPORTED_UMP:
		ret = kbase_mmu_update_pages(kctx, reg->start_pfn, kbase_get_cpu_phy_pages(reg), reg->gpu_alloc->nents, reg->flags);
		break;
#endif
#ifdef CONFIG_DMA_SHARED_BUFFER
	case KBASE_MEM_TYPE_IMPORTED_UMM:
		/* Future use will use the new flags, existing mapping will NOT be updated
		 * as memory should not be in use by the GPU when updating the flags.
		 */
		ret = 0;
		WARN_ON(reg->gpu_alloc->imported.umm.current_mapping_usage_count);
		break;
#endif
	default:
		break;
	}

	/* roll back on error, i.e. not UMP */
	if (ret)
		reg->flags = prev_flags;

out_unlock:
	kbase_gpu_vm_unlock(kctx);
out:
	return ret;
}

#define KBASE_MEM_IMPORT_HAVE_PAGES (1UL << BASE_MEM_FLAGS_NR_BITS)

#ifdef CONFIG_UMP
static struct kbase_va_region *kbase_mem_from_ump(struct kbase_context *kctx, ump_secure_id id, u64 *va_pages, u64 *flags)
{
	struct kbase_va_region *reg;
	ump_dd_handle umph;
	u64 block_count;
	const ump_dd_physical_block_64 *block_array;
	u64 i, j;
	int page = 0;
	ump_alloc_flags ump_flags;
	ump_alloc_flags cpu_flags;
	ump_alloc_flags gpu_flags;

	KBASE_DEBUG_ASSERT(kctx);
	KBASE_DEBUG_ASSERT(va_pages);
	KBASE_DEBUG_ASSERT(flags);

	if (*flags & BASE_MEM_SECURE)
		goto bad_flags;

	umph = ump_dd_from_secure_id(id);
	if (UMP_DD_INVALID_MEMORY_HANDLE == umph)
		goto bad_id;

	ump_flags = ump_dd_allocation_flags_get(umph);
	cpu_flags = (ump_flags >> UMP_DEVICE_CPU_SHIFT) & UMP_DEVICE_MASK;
	gpu_flags = (ump_flags >> DEFAULT_UMP_GPU_DEVICE_SHIFT) &
			UMP_DEVICE_MASK;

	*va_pages = ump_dd_size_get_64(umph);
	*va_pages >>= PAGE_SHIFT;

	if (!*va_pages)
		goto bad_size;

	if (*va_pages > (U64_MAX / PAGE_SIZE))
		/* 64-bit address range is the max */
		goto bad_size;

	if (*flags & BASE_MEM_SAME_VA)
		reg = kbase_alloc_free_region(kctx, 0, *va_pages, KBASE_REG_ZONE_SAME_VA);
	else
		reg = kbase_alloc_free_region(kctx, 0, *va_pages, KBASE_REG_ZONE_CUSTOM_VA);

	if (!reg)
		goto no_region;

	/* we've got pages to map now, and support SAME_VA */
	*flags |= KBASE_MEM_IMPORT_HAVE_PAGES;

	reg->gpu_alloc = kbase_alloc_create(*va_pages, KBASE_MEM_TYPE_IMPORTED_UMP);
	if (IS_ERR_OR_NULL(reg->gpu_alloc))
		goto no_alloc_obj;

	reg->cpu_alloc = kbase_mem_phy_alloc_get(reg->gpu_alloc);

	reg->gpu_alloc->imported.ump_handle = umph;

	reg->flags &= ~KBASE_REG_FREE;
	reg->flags |= KBASE_REG_GPU_NX;	/* UMP is always No eXecute */
	reg->flags &= ~KBASE_REG_GROWABLE;	/* UMP cannot be grown */

	/* Override import flags based on UMP flags */
	*flags &= ~(BASE_MEM_CACHED_CPU);
	*flags &= ~(BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR);
	*flags &= ~(BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR);

	if ((cpu_flags & (UMP_HINT_DEVICE_RD | UMP_HINT_DEVICE_WR)) ==
	    (UMP_HINT_DEVICE_RD | UMP_HINT_DEVICE_WR)) {
		reg->flags |= KBASE_REG_CPU_CACHED;
		*flags |= BASE_MEM_CACHED_CPU;
	}

	if (cpu_flags & UMP_PROT_CPU_WR) {
		reg->flags |= KBASE_REG_CPU_WR;
		*flags |= BASE_MEM_PROT_CPU_WR;
	}

	if (cpu_flags & UMP_PROT_CPU_RD) {
		reg->flags |= KBASE_REG_CPU_RD;
		*flags |= BASE_MEM_PROT_CPU_RD;
	}

	if ((gpu_flags & (UMP_HINT_DEVICE_RD | UMP_HINT_DEVICE_WR)) ==
	    (UMP_HINT_DEVICE_RD | UMP_HINT_DEVICE_WR))
		reg->flags |= KBASE_REG_GPU_CACHED;

	if (gpu_flags & UMP_PROT_DEVICE_WR) {
		reg->flags |= KBASE_REG_GPU_WR;
		*flags |= BASE_MEM_PROT_GPU_WR;
	}

	if (gpu_flags & UMP_PROT_DEVICE_RD) {
		reg->flags |= KBASE_REG_GPU_RD;
		*flags |= BASE_MEM_PROT_GPU_RD;
	}

	/* ump phys block query */
	ump_dd_phys_blocks_get_64(umph, &block_count, &block_array);

	for (i = 0; i < block_count; i++) {
		for (j = 0; j < (block_array[i].size >> PAGE_SHIFT); j++) {
			reg->gpu_alloc->pages[page] = block_array[i].addr + (j << PAGE_SHIFT);
			page++;
		}
	}
	reg->gpu_alloc->nents = *va_pages;
	reg->extent = 0;

	return reg;

no_alloc_obj:
	kfree(reg);
no_region:
bad_size:
	ump_dd_release(umph);
bad_id:
bad_flags:
	return NULL;
}
#endif				/* CONFIG_UMP */

#ifdef CONFIG_DMA_SHARED_BUFFER
static struct kbase_va_region *kbase_mem_from_umm(struct kbase_context *kctx, int fd, u64 *va_pages, u64 *flags)
{
	struct kbase_va_region *reg;
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *dma_attachment;

	dma_buf = dma_buf_get(fd);
	if (IS_ERR_OR_NULL(dma_buf))
		goto no_buf;

	dma_attachment = dma_buf_attach(dma_buf, kctx->kbdev->dev);
	if (!dma_attachment)
		goto no_attachment;

	*va_pages = PAGE_ALIGN(dma_buf->size) >> PAGE_SHIFT;
	if (!*va_pages)
		goto bad_size;

	if (*va_pages > (U64_MAX / PAGE_SIZE))
		/* 64-bit address range is the max */
		goto bad_size;

	/* ignore SAME_VA */
	*flags &= ~BASE_MEM_SAME_VA;

#ifdef CONFIG_64BIT
	if (!kctx->is_compat) {
		/* 64-bit tasks must MMAP anyway, but not expose this address to clients */
		*flags |= BASE_MEM_NEED_MMAP;
		reg = kbase_alloc_free_region(kctx, 0, *va_pages, KBASE_REG_ZONE_SAME_VA);
	} else {
#else
	if (1) {
#endif
		reg = kbase_alloc_free_region(kctx, 0, *va_pages, KBASE_REG_ZONE_CUSTOM_VA);
	}

	if (!reg)
		goto no_region;

	reg->gpu_alloc = kbase_alloc_create(*va_pages, KBASE_MEM_TYPE_IMPORTED_UMM);
	if (IS_ERR_OR_NULL(reg->gpu_alloc))
		goto no_alloc_obj;

	reg->cpu_alloc = kbase_mem_phy_alloc_get(reg->gpu_alloc);

	/* No pages to map yet */
	reg->gpu_alloc->nents = 0;

	reg->flags &= ~KBASE_REG_FREE;
	reg->flags |= KBASE_REG_GPU_NX;	/* UMM is always No eXecute */
	reg->flags &= ~KBASE_REG_GROWABLE;	/* UMM cannot be grown */
	reg->flags |= KBASE_REG_GPU_CACHED;

	if (*flags & BASE_MEM_PROT_CPU_WR)
		reg->flags |= KBASE_REG_CPU_WR;

	if (*flags & BASE_MEM_PROT_CPU_RD)
		reg->flags |= KBASE_REG_CPU_RD;

	if (*flags & BASE_MEM_PROT_GPU_WR)
		reg->flags |= KBASE_REG_GPU_WR;

	if (*flags & BASE_MEM_PROT_GPU_RD)
		reg->flags |= KBASE_REG_GPU_RD;

	if (*flags & BASE_MEM_SECURE)
		reg->flags |= KBASE_REG_SECURE;

	/* no read or write permission given on import, only on run do we give the right permissions */

	reg->gpu_alloc->type = BASE_MEM_IMPORT_TYPE_UMM;
	reg->gpu_alloc->imported.umm.sgt = NULL;
	reg->gpu_alloc->imported.umm.dma_buf = dma_buf;
	reg->gpu_alloc->imported.umm.dma_attachment = dma_attachment;
	reg->gpu_alloc->imported.umm.current_mapping_usage_count = 0;
	reg->extent = 0;

	return reg;

no_alloc_obj:
	kfree(reg);
no_region:
bad_size:
	dma_buf_detach(dma_buf, dma_attachment);
no_attachment:
	dma_buf_put(dma_buf);
no_buf:
	return NULL;
}
#endif  /* CONFIG_DMA_SHARED_BUFFER */

u64 kbase_mem_alias(struct kbase_context *kctx, u64 *flags, u64 stride,
		    u64 nents, struct base_mem_aliasing_info *ai,
		    u64 *num_pages)
{
	struct kbase_va_region *reg;
	u64 gpu_va;
	size_t i;
	bool coherent;

	KBASE_DEBUG_ASSERT(kctx);
	KBASE_DEBUG_ASSERT(flags);
	KBASE_DEBUG_ASSERT(ai);
	KBASE_DEBUG_ASSERT(num_pages);

	/* mask to only allowed flags */
	*flags &= (BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
		   BASE_MEM_COHERENT_SYSTEM | BASE_MEM_COHERENT_LOCAL |
		   BASE_MEM_COHERENT_SYSTEM_REQUIRED);

	if (!(*flags & (BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR))) {
		dev_warn(kctx->kbdev->dev,
				"kbase_mem_alias called with bad flags (%llx)",
				(unsigned long long)*flags);
		goto bad_flags;
	}
	coherent = (*flags & BASE_MEM_COHERENT_SYSTEM) != 0 ||
			(*flags & BASE_MEM_COHERENT_SYSTEM_REQUIRED) != 0;

	if (!stride)
		goto bad_stride;

	if (!nents)
		goto bad_nents;

	if ((nents * stride) > (U64_MAX / PAGE_SIZE))
		/* 64-bit address range is the max */
		goto bad_size;

	/* calculate the number of pages this alias will cover */
	*num_pages = nents * stride;

#ifdef CONFIG_64BIT
	if (!kctx->is_compat) {
		/* 64-bit tasks must MMAP anyway, but not expose this address to
		 * clients */
		*flags |= BASE_MEM_NEED_MMAP;
		reg = kbase_alloc_free_region(kctx, 0, *num_pages,
					      KBASE_REG_ZONE_SAME_VA);
	} else {
#else
	if (1) {
#endif
		reg = kbase_alloc_free_region(kctx, 0, *num_pages,
					      KBASE_REG_ZONE_CUSTOM_VA);
	}

	if (!reg)
		goto no_reg;

	/* zero-sized page array, as we don't need one/can support one */
	reg->gpu_alloc = kbase_alloc_create(0, KBASE_MEM_TYPE_ALIAS);
	if (IS_ERR_OR_NULL(reg->gpu_alloc))
		goto no_alloc_obj;

	reg->cpu_alloc = kbase_mem_phy_alloc_get(reg->gpu_alloc);

	kbase_update_region_flags(kctx, reg, *flags);

	reg->gpu_alloc->imported.alias.nents = nents;
	reg->gpu_alloc->imported.alias.stride = stride;
	reg->gpu_alloc->imported.alias.aliased = vzalloc(sizeof(*reg->gpu_alloc->imported.alias.aliased) * nents);
	if (!reg->gpu_alloc->imported.alias.aliased)
		goto no_aliased_array;

	kbase_gpu_vm_lock(kctx);

	/* validate and add src handles */
	for (i = 0; i < nents; i++) {
		if (ai[i].handle < BASE_MEM_FIRST_FREE_ADDRESS) {
			if (ai[i].handle != BASE_MEM_WRITE_ALLOC_PAGES_HANDLE)
				goto bad_handle; /* unsupported magic handle */
			if (!ai[i].length)
				goto bad_handle; /* must be > 0 */
			if (ai[i].length > stride)
				goto bad_handle; /* can't be larger than the
						    stride */
			reg->gpu_alloc->imported.alias.aliased[i].length = ai[i].length;
		} else {
			struct kbase_va_region *aliasing_reg;
			struct kbase_mem_phy_alloc *alloc;

			aliasing_reg = kbase_region_tracker_find_region_base_address(kctx, (ai[i].handle >> PAGE_SHIFT) << PAGE_SHIFT);

			/* validate found region */
			if (!aliasing_reg)
				goto bad_handle; /* Not found */
			if (aliasing_reg->flags & KBASE_REG_FREE)
				goto bad_handle; /* Free region */
			if (!aliasing_reg->gpu_alloc)
				goto bad_handle; /* No alloc */
			if (aliasing_reg->gpu_alloc->type != KBASE_MEM_TYPE_NATIVE)
				goto bad_handle; /* Not a native alloc */
			if (coherent != ((aliasing_reg->flags & KBASE_REG_SHARE_BOTH) != 0))
				goto bad_handle;
				/* Non-coherent memory cannot alias
				   coherent memory, and vice versa.*/

			/* check size against stride */
			if (!ai[i].length)
				goto bad_handle; /* must be > 0 */
			if (ai[i].length > stride)
				goto bad_handle; /* can't be larger than the
						    stride */

			alloc = aliasing_reg->gpu_alloc;

			/* check against the alloc's size */
			if (ai[i].offset > alloc->nents)
				goto bad_handle; /* beyond end */
			if (ai[i].offset + ai[i].length > alloc->nents)
				goto bad_handle; /* beyond end */

			reg->gpu_alloc->imported.alias.aliased[i].alloc = kbase_mem_phy_alloc_get(alloc);
			reg->gpu_alloc->imported.alias.aliased[i].length = ai[i].length;
			reg->gpu_alloc->imported.alias.aliased[i].offset = ai[i].offset;
		}
	}

#ifdef CONFIG_64BIT
	if (!kctx->is_compat) {
		/* Bind to a cookie */
		if (!kctx->cookies) {
			dev_err(kctx->kbdev->dev, "No cookies available for allocation!");
			goto no_cookie;
		}
		/* return a cookie */
		gpu_va = __ffs(kctx->cookies);
		kctx->cookies &= ~(1UL << gpu_va);
		BUG_ON(kctx->pending_regions[gpu_va]);
		kctx->pending_regions[gpu_va] = reg;

		/* relocate to correct base */
		gpu_va += PFN_DOWN(BASE_MEM_COOKIE_BASE);
		gpu_va <<= PAGE_SHIFT;
	} else /* we control the VA */ {
#else
	if (1) {
#endif
		if (kbase_gpu_mmap(kctx, reg, 0, *num_pages, 1) != 0) {
			dev_warn(kctx->kbdev->dev, "Failed to map memory on GPU");
			goto no_mmap;
		}
		/* return real GPU VA */
		gpu_va = reg->start_pfn << PAGE_SHIFT;
	}

	reg->flags &= ~KBASE_REG_FREE;
	reg->flags &= ~KBASE_REG_GROWABLE;

	kbase_gpu_vm_unlock(kctx);

	return gpu_va;

#ifdef CONFIG_64BIT
no_cookie:
#endif
no_mmap:
bad_handle:
	kbase_gpu_vm_unlock(kctx);
no_aliased_array:
	kbase_mem_phy_alloc_put(reg->cpu_alloc);
	kbase_mem_phy_alloc_put(reg->gpu_alloc);
no_alloc_obj:
	kfree(reg);
no_reg:
bad_size:
bad_nents:
bad_stride:
bad_flags:
	return 0;
}

int kbase_mem_import(struct kbase_context *kctx, enum base_mem_import_type type, int handle, u64 *gpu_va, u64 *va_pages, u64 *flags)
{
	struct kbase_va_region *reg;

	KBASE_DEBUG_ASSERT(kctx);
	KBASE_DEBUG_ASSERT(gpu_va);
	KBASE_DEBUG_ASSERT(va_pages);
	KBASE_DEBUG_ASSERT(flags);

#ifdef CONFIG_64BIT
	if (!kctx->is_compat)
		*flags |= BASE_MEM_SAME_VA;
#endif

	if (!kbase_check_import_flags(*flags)) {
		dev_warn(kctx->kbdev->dev,
				"kbase_mem_import called with bad flags (%llx)",
				(unsigned long long)*flags);
		goto bad_flags;
	}

	switch (type) {
#ifdef CONFIG_UMP
	case BASE_MEM_IMPORT_TYPE_UMP:
		reg = kbase_mem_from_ump(kctx, (ump_secure_id)handle, va_pages, flags);
		break;
#endif /* CONFIG_UMP */
#ifdef CONFIG_DMA_SHARED_BUFFER
	case BASE_MEM_IMPORT_TYPE_UMM:
		reg = kbase_mem_from_umm(kctx, handle, va_pages, flags);
		break;
#endif /* CONFIG_DMA_SHARED_BUFFER */
	default:
		reg = NULL;
		break;
	}

	if (!reg)
		goto no_reg;

	kbase_gpu_vm_lock(kctx);

	/* mmap needed to setup VA? */
	if (*flags & (BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP)) {
		/* Bind to a cookie */
		if (!kctx->cookies)
			goto no_cookie;
		/* return a cookie */
		*gpu_va = __ffs(kctx->cookies);
		kctx->cookies &= ~(1UL << *gpu_va);
		BUG_ON(kctx->pending_regions[*gpu_va]);
		kctx->pending_regions[*gpu_va] = reg;

		/* relocate to correct base */
		*gpu_va += PFN_DOWN(BASE_MEM_COOKIE_BASE);
		*gpu_va <<= PAGE_SHIFT;

	} else if (*flags & KBASE_MEM_IMPORT_HAVE_PAGES)  {
		/* we control the VA, mmap now to the GPU */
		if (kbase_gpu_mmap(kctx, reg, 0, *va_pages, 1) != 0)
			goto no_gpu_va;
		/* return real GPU VA */
		*gpu_va = reg->start_pfn << PAGE_SHIFT;
	} else {
		/* we control the VA, but nothing to mmap yet */
		if (kbase_add_va_region(kctx, reg, 0, *va_pages, 1) != 0)
			goto no_gpu_va;
		/* return real GPU VA */
		*gpu_va = reg->start_pfn << PAGE_SHIFT;
	}

	/* clear out private flags */
	*flags &= ((1UL << BASE_MEM_FLAGS_NR_BITS) - 1);

	kbase_gpu_vm_unlock(kctx);

	return 0;

no_gpu_va:
no_cookie:
	kbase_gpu_vm_unlock(kctx);
	kbase_mem_phy_alloc_put(reg->cpu_alloc);
	kbase_mem_phy_alloc_put(reg->gpu_alloc);
	kfree(reg);
no_reg:
bad_flags:
	*gpu_va = 0;
	*va_pages = 0;
	*flags = 0;
	return -ENOMEM;
}


static int zap_range_nolock(struct mm_struct *mm,
		const struct vm_operations_struct *vm_ops,
		unsigned long start, unsigned long end)
{
	struct vm_area_struct *vma;
	int err = -EINVAL; /* in case end < start */

	while (start < end) {
		unsigned long local_end;

		vma = find_vma_intersection(mm, start, end);
		if (!vma)
			break;

		/* is it ours? */
		if (vma->vm_ops != vm_ops)
			goto try_next;

		local_end = vma->vm_end;

		if (end < local_end)
			local_end = end;

		err = zap_vma_ptes(vma, start, local_end - start);
		if (unlikely(err))
			break;

try_next:
		/* go to next vma, if any */
		start = vma->vm_end;
	}

	return err;
}

int kbase_mem_commit(struct kbase_context *kctx, u64 gpu_addr, u64 new_pages, enum base_backing_threshold_status *failure_reason)
{
	u64 old_pages;
	u64 delta;
	int res = -EINVAL;
	struct kbase_va_region *reg;
	phys_addr_t *phy_pages;

	KBASE_DEBUG_ASSERT(kctx);
	KBASE_DEBUG_ASSERT(failure_reason);
	KBASE_DEBUG_ASSERT(gpu_addr != 0);

	down_read(&current->mm->mmap_sem);
	kbase_gpu_vm_lock(kctx);

	/* Validate the region */
	reg = kbase_region_tracker_find_region_base_address(kctx, gpu_addr);
	if (!reg || (reg->flags & KBASE_REG_FREE)) {
		*failure_reason = BASE_BACKING_THRESHOLD_ERROR_INVALID_ARGUMENTS;
		goto out_unlock;
	}

	KBASE_DEBUG_ASSERT(reg->cpu_alloc);
	KBASE_DEBUG_ASSERT(reg->gpu_alloc);

	if (reg->gpu_alloc->type != KBASE_MEM_TYPE_NATIVE) {
		*failure_reason = BASE_BACKING_THRESHOLD_ERROR_NOT_GROWABLE;
		goto out_unlock;
	}

	if (0 == (reg->flags & KBASE_REG_GROWABLE)) {
		*failure_reason = BASE_BACKING_THRESHOLD_ERROR_NOT_GROWABLE;
		goto out_unlock;
	}

	if (new_pages > reg->nr_pages) {
		/* Would overflow the VA region */
		*failure_reason = BASE_BACKING_THRESHOLD_ERROR_INVALID_ARGUMENTS;
		goto out_unlock;
	}

	/* can't be mapped more than once on the GPU */
	if (atomic_read(&reg->gpu_alloc->gpu_mappings) > 1) {
		*failure_reason = BASE_BACKING_THRESHOLD_ERROR_NOT_GROWABLE;
		goto out_unlock;
	}

	if (new_pages == reg->gpu_alloc->nents) {
		/* no change */
		res = 0;
		goto out_unlock;
	}

	phy_pages = kbase_get_gpu_phy_pages(reg);
	old_pages = kbase_reg_current_backed_size(reg);

	if (new_pages > old_pages) {
		/* growing */
		int err;

		delta = new_pages - old_pages;
		/* Allocate some more pages */
		if (kbase_alloc_phy_pages_helper(reg->cpu_alloc, delta) != 0) {
			*failure_reason = BASE_BACKING_THRESHOLD_ERROR_OOM;
			goto out_unlock;
		}
		if (reg->cpu_alloc != reg->gpu_alloc) {
			if (kbase_alloc_phy_pages_helper(
					reg->gpu_alloc, delta) != 0) {
				*failure_reason = BASE_BACKING_THRESHOLD_ERROR_OOM;
				kbase_free_phy_pages_helper(reg->cpu_alloc,
						delta);
				goto out_unlock;
			}
		}
		err = kbase_mmu_insert_pages(kctx, reg->start_pfn + old_pages,
				phy_pages + old_pages, delta, reg->flags);
		if (err) {
			kbase_free_phy_pages_helper(reg->cpu_alloc, delta);
			if (reg->cpu_alloc != reg->gpu_alloc)
				kbase_free_phy_pages_helper(reg->gpu_alloc,
						delta);
			*failure_reason = BASE_BACKING_THRESHOLD_ERROR_OOM;
			goto out_unlock;
		}
	} else {
		/* shrinking */
		struct kbase_cpu_mapping *mapping;
		int err;

		/* first, unmap from any mappings affected */
		list_for_each_entry(mapping, &reg->cpu_alloc->mappings, mappings_list) {
			unsigned long mapping_size = (mapping->vm_end - mapping->vm_start) >> PAGE_SHIFT;

			/* is this mapping affected ?*/
			if ((mapping->page_off + mapping_size) > new_pages) {
				unsigned long first_bad = 0;
				int zap_res;

				if (new_pages > mapping->page_off)
					first_bad = new_pages - mapping->page_off;

				zap_res = zap_range_nolock(current->mm,
						&kbase_vm_ops,
						mapping->vm_start +
						(first_bad << PAGE_SHIFT),
						mapping->vm_end);
				WARN(zap_res,
				     "Failed to zap VA range (0x%lx - 0x%lx);\n",
				     mapping->vm_start +
				     (first_bad << PAGE_SHIFT),
				     mapping->vm_end
				     );
			}
		}

		/* Free some pages */
		delta = old_pages - new_pages;
		err = kbase_mmu_teardown_pages(kctx, reg->start_pfn + new_pages,
				delta);
		if (err) {
			*failure_reason = BASE_BACKING_THRESHOLD_ERROR_OOM;
			goto out_unlock;
		}
#ifndef CONFIG_MALI_NO_MALI
		if (kbase_hw_has_issue(kctx->kbdev, BASE_HW_ISSUE_6367)) {
			/* Wait for GPU to flush write buffer before freeing physical pages */
			kbase_wait_write_flush(kctx);
		}
#endif
		kbase_free_phy_pages_helper(reg->cpu_alloc, delta);
		if (reg->cpu_alloc != reg->gpu_alloc)
			kbase_free_phy_pages_helper(reg->gpu_alloc, delta);
	}

	res = 0;

out_unlock:
	kbase_gpu_vm_unlock(kctx);
	up_read(&current->mm->mmap_sem);

	return res;
}

static void kbase_cpu_vm_open(struct vm_area_struct *vma)
{
	struct kbase_cpu_mapping *map = vma->vm_private_data;

	KBASE_DEBUG_ASSERT(map);
	KBASE_DEBUG_ASSERT(map->count > 0);
	/* non-atomic as we're under Linux' mm lock */
	map->count++;
}

static void kbase_cpu_vm_close(struct vm_area_struct *vma)
{
	struct kbase_cpu_mapping *map = vma->vm_private_data;

	KBASE_DEBUG_ASSERT(map);
	KBASE_DEBUG_ASSERT(map->count > 0);

	/* non-atomic as we're under Linux' mm lock */
	if (--map->count)
		return;

	KBASE_DEBUG_ASSERT(map->kctx);
	KBASE_DEBUG_ASSERT(map->alloc);

	kbase_gpu_vm_lock(map->kctx);

	if (map->region) {
		KBASE_DEBUG_ASSERT((map->region->flags & KBASE_REG_ZONE_MASK) ==
				KBASE_REG_ZONE_SAME_VA);
		/* Avoid freeing memory on the process death which results in
		 * GPU Page Fault. Memory will be freed in kbase_destroy_context
		 */
		if (!(current->flags & PF_EXITING))
			kbase_mem_free_region(map->kctx, map->region);
	}

	list_del(&map->mappings_list);

	kbase_gpu_vm_unlock(map->kctx);

	kbase_mem_phy_alloc_put(map->alloc);
	kfree(map);
}

KBASE_EXPORT_TEST_API(kbase_cpu_vm_close);


static int kbase_cpu_vm_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct kbase_cpu_mapping *map = vma->vm_private_data;
	pgoff_t rel_pgoff;
	size_t i;

	KBASE_DEBUG_ASSERT(map);
	KBASE_DEBUG_ASSERT(map->count > 0);
	KBASE_DEBUG_ASSERT(map->kctx);
	KBASE_DEBUG_ASSERT(map->alloc);

	/* we don't use vmf->pgoff as it's affected by our mmap with
	 * offset being a GPU VA or a cookie */
	rel_pgoff = ((unsigned long)vmf->virtual_address - map->vm_start)
			>> PAGE_SHIFT;

	kbase_gpu_vm_lock(map->kctx);
	if (map->page_off + rel_pgoff >= map->alloc->nents)
		goto locked_bad_fault;

	/* insert all valid pages from the fault location */
	for (i = rel_pgoff;
	     i < MIN((vma->vm_end - vma->vm_start) >> PAGE_SHIFT,
	     map->alloc->nents - map->page_off); i++) {
		int ret = vm_insert_pfn(vma, map->vm_start + (i << PAGE_SHIFT),
		    PFN_DOWN(map->alloc->pages[map->page_off + i]));
		if (ret < 0 && ret != -EBUSY)
			goto locked_bad_fault;
	}

	kbase_gpu_vm_unlock(map->kctx);
	/* we resolved it, nothing for VM to do */
	return VM_FAULT_NOPAGE;

locked_bad_fault:
	kbase_gpu_vm_unlock(map->kctx);
	return VM_FAULT_SIGBUS;
}

static const struct vm_operations_struct kbase_vm_ops = {
	.open  = kbase_cpu_vm_open,
	.close = kbase_cpu_vm_close,
	.fault = kbase_cpu_vm_fault
};

static int kbase_cpu_mmap(struct kbase_va_region *reg, struct vm_area_struct *vma, void *kaddr, size_t nr_pages, unsigned long aligned_offset, int free_on_close)
{
	struct kbase_cpu_mapping *map;
	u64 start_off = vma->vm_pgoff - reg->start_pfn;
	phys_addr_t *page_array;
	int err = 0;
	int i;

	map = kzalloc(sizeof(*map), GFP_KERNEL);

	if (!map) {
		WARN_ON(1);
		err = -ENOMEM;
		goto out;
	}

	/*
	 * VM_DONTCOPY - don't make this mapping available in fork'ed processes
	 * VM_DONTEXPAND - disable mremap on this region
	 * VM_IO - disables paging
	 * VM_DONTDUMP - Don't include in core dumps (3.7 only)
	 * VM_MIXEDMAP - Support mixing struct page*s and raw pfns.
	 *               This is needed to support using the dedicated and
	 *               the OS based memory backends together.
	 */
	/*
	 * This will need updating to propagate coherency flags
	 * See MIDBASE-1057
	 */

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 7, 0))
	vma->vm_flags |= VM_DONTCOPY | VM_DONTDUMP | VM_DONTEXPAND | VM_IO;
#else
	vma->vm_flags |= VM_DONTCOPY | VM_DONTEXPAND | VM_RESERVED | VM_IO;
#endif
	vma->vm_ops = &kbase_vm_ops;
	vma->vm_private_data = map;

	page_array = kbase_get_cpu_phy_pages(reg);

	if (!(reg->flags & KBASE_REG_CPU_CACHED) &&
	    (reg->flags & (KBASE_REG_CPU_WR|KBASE_REG_CPU_RD))) {
		/* We can't map vmalloc'd memory uncached.
		 * Other memory will have been returned from
		 * kbase_mem_pool which would be
		 * suitable for mapping uncached.
		 */
		BUG_ON(kaddr);
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	}

	if (!kaddr) {
		unsigned long addr = vma->vm_start + aligned_offset;

		vma->vm_flags |= VM_PFNMAP;
		for (i = 0; i < nr_pages; i++) {
			unsigned long pfn = PFN_DOWN(page_array[i + start_off]);

			err = vm_insert_pfn(vma, addr, pfn);
			if (WARN_ON(err))
				break;

			addr += PAGE_SIZE;
		}
	} else {
		WARN_ON(aligned_offset);
		/* MIXEDMAP so we can vfree the kaddr early and not track it after map time */
		vma->vm_flags |= VM_MIXEDMAP;
		/* vmalloc remaping is easy... */
		err = remap_vmalloc_range(vma, kaddr, 0);
		WARN_ON(err);
	}

	if (err) {
		kfree(map);
		goto out;
	}

	map->page_off = start_off;
	map->region = free_on_close ? reg : NULL;
	map->kctx = reg->kctx;
	map->vm_start = vma->vm_start + aligned_offset;
	if (aligned_offset) {
		KBASE_DEBUG_ASSERT(!start_off);
		map->vm_end = map->vm_start + (reg->nr_pages << PAGE_SHIFT);
	} else {
		map->vm_end = vma->vm_end;
	}
	map->alloc = kbase_mem_phy_alloc_get(reg->cpu_alloc);
	map->count = 1; /* start with one ref */

	if (reg->flags & KBASE_REG_CPU_CACHED)
		map->alloc->properties |= KBASE_MEM_PHY_ALLOC_ACCESSED_CACHED;

	list_add(&map->mappings_list, &map->alloc->mappings);

 out:
	return err;
}

static int kbase_trace_buffer_mmap(struct kbase_context *kctx, struct vm_area_struct *vma, struct kbase_va_region **const reg, void **const kaddr)
{
	struct kbase_va_region *new_reg;
	u32 nr_pages;
	size_t size;
	int err = 0;
	u32 *tb;
	int owns_tb = 1;

	dev_dbg(kctx->kbdev->dev, "in %s\n", __func__);
	size = (vma->vm_end - vma->vm_start);
	nr_pages = size >> PAGE_SHIFT;

	if (!kctx->jctx.tb) {
		KBASE_DEBUG_ASSERT(0 != size);
		tb = vmalloc_user(size);

		if (NULL == tb) {
			err = -ENOMEM;
			goto out;
		}

		kbase_device_trace_buffer_install(kctx, tb, size);
	} else {
		err = -EINVAL;
		goto out;
	}

	*kaddr = kctx->jctx.tb;

	new_reg = kbase_alloc_free_region(kctx, 0, nr_pages, KBASE_REG_ZONE_SAME_VA);
	if (!new_reg) {
		err = -ENOMEM;
		WARN_ON(1);
		goto out_no_region;
	}

	new_reg->cpu_alloc = kbase_alloc_create(0, KBASE_MEM_TYPE_TB);
	if (IS_ERR_OR_NULL(new_reg->cpu_alloc)) {
		err = -ENOMEM;
		new_reg->cpu_alloc = NULL;
		WARN_ON(1);
		goto out_no_alloc;
	}

	new_reg->gpu_alloc = kbase_mem_phy_alloc_get(new_reg->cpu_alloc);

	new_reg->cpu_alloc->imported.kctx = kctx;
	new_reg->flags &= ~KBASE_REG_FREE;
	new_reg->flags |= KBASE_REG_CPU_CACHED;

	/* alloc now owns the tb */
	owns_tb = 0;

	if (kbase_add_va_region(kctx, new_reg, vma->vm_start, nr_pages, 1) != 0) {
		err = -ENOMEM;
		WARN_ON(1);
		goto out_no_va_region;
	}

	*reg = new_reg;

	/* map read only, noexec */
	vma->vm_flags &= ~(VM_WRITE | VM_MAYWRITE | VM_EXEC | VM_MAYEXEC);
	/* the rest of the flags is added by the cpu_mmap handler */

	dev_dbg(kctx->kbdev->dev, "%s done\n", __func__);
	return 0;

out_no_va_region:
out_no_alloc:
	kbase_free_alloced_region(new_reg);
out_no_region:
	if (owns_tb) {
		kbase_device_trace_buffer_uninstall(kctx);
		vfree(tb);
	}
out:
	return err;
}

static int kbase_mmu_dump_mmap(struct kbase_context *kctx, struct vm_area_struct *vma, struct kbase_va_region **const reg, void **const kmap_addr)
{
	struct kbase_va_region *new_reg;
	void *kaddr;
	u32 nr_pages;
	size_t size;
	int err = 0;

	dev_dbg(kctx->kbdev->dev, "in kbase_mmu_dump_mmap\n");
	size = (vma->vm_end - vma->vm_start);
	nr_pages = size >> PAGE_SHIFT;

	kaddr = kbase_mmu_dump(kctx, nr_pages);

	if (!kaddr) {
		err = -ENOMEM;
		goto out;
	}

	new_reg = kbase_alloc_free_region(kctx, 0, nr_pages, KBASE_REG_ZONE_SAME_VA);
	if (!new_reg) {
		err = -ENOMEM;
		WARN_ON(1);
		goto out;
	}

	new_reg->cpu_alloc = kbase_alloc_create(0, KBASE_MEM_TYPE_RAW);
	if (IS_ERR_OR_NULL(new_reg->cpu_alloc)) {
		err = -ENOMEM;
		new_reg->cpu_alloc = NULL;
		WARN_ON(1);
		goto out_no_alloc;
	}

	new_reg->gpu_alloc = kbase_mem_phy_alloc_get(new_reg->cpu_alloc);

	new_reg->flags &= ~KBASE_REG_FREE;
	new_reg->flags |= KBASE_REG_CPU_CACHED;
	if (kbase_add_va_region(kctx, new_reg, vma->vm_start, nr_pages, 1) != 0) {
		err = -ENOMEM;
		WARN_ON(1);
		goto out_va_region;
	}

	*kmap_addr = kaddr;
	*reg = new_reg;

	dev_dbg(kctx->kbdev->dev, "kbase_mmu_dump_mmap done\n");
	return 0;

out_no_alloc:
out_va_region:
	kbase_free_alloced_region(new_reg);
out:
	return err;
}


void kbase_os_mem_map_lock(struct kbase_context *kctx)
{
	struct mm_struct *mm = current->mm;
	(void)kctx;
	down_read(&mm->mmap_sem);
}

void kbase_os_mem_map_unlock(struct kbase_context *kctx)
{
	struct mm_struct *mm = current->mm;
	(void)kctx;
	up_read(&mm->mmap_sem);
}

#if defined(CONFIG_DMA_SHARED_BUFFER) && defined(CONFIG_MALI_TRACE_TIMELINE)
/* This section is required only for instrumentation. */

static void kbase_dma_buf_vm_open(struct vm_area_struct *vma)
{
	struct kbase_cpu_mapping *map = vma->vm_private_data;

	KBASE_DEBUG_ASSERT(map);
	KBASE_DEBUG_ASSERT(map->count > 0);
	/* Non-atomic as we're under Linux's mm lock. */
	map->count++;
}

static void kbase_dma_buf_vm_close(struct vm_area_struct *vma)
{
	struct kbase_cpu_mapping *map = vma->vm_private_data;

	KBASE_DEBUG_ASSERT(map);
	KBASE_DEBUG_ASSERT(map->count > 0);

	/* Non-atomic as we're under Linux's mm lock. */
	if (--map->count)
		return;

	KBASE_DEBUG_ASSERT(map->kctx);

	kbase_gpu_vm_lock(map->kctx);
	list_del(&map->mappings_list);
	kbase_gpu_vm_unlock(map->kctx);
	kfree(map);
}

static const struct vm_operations_struct kbase_dma_mmap_ops = {
	.open  = kbase_dma_buf_vm_open,
	.close = kbase_dma_buf_vm_close,
};
#endif /* CONFIG_DMA_SHARED_BUFFER && CONFIG_MALI_TRACE_TIMELINE */

int kbase_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct kbase_context *kctx = file->private_data;
	struct kbase_va_region *reg;
	void *kaddr = NULL;
	size_t nr_pages;
	int err = 0;
	int free_on_close = 0;
	struct device *dev = kctx->kbdev->dev;
	size_t aligned_offset = 0;

	dev_dbg(dev, "kbase_mmap\n");
	nr_pages = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;

	/* strip away corresponding VM_MAY% flags to the VM_% flags requested */
	vma->vm_flags &= ~((vma->vm_flags & (VM_READ | VM_WRITE)) << 4);

	if (0 == nr_pages) {
		err = -EINVAL;
		goto out;
	}

	if (!(vma->vm_flags & VM_SHARED)) {
		err = -EINVAL;
		goto out;
	}

	kbase_gpu_vm_lock(kctx);

	if (vma->vm_pgoff == PFN_DOWN(BASE_MEM_MAP_TRACKING_HANDLE)) {
		/* The non-mapped tracking helper page */
		err = kbase_tracking_page_setup(kctx, vma);
		goto out_unlock;
	}

	/* if not the MTP, verify that the MTP has been mapped */
	rcu_read_lock();
	/* catches both when the special page isn't present or
	 * when we've forked */
	if (rcu_dereference(kctx->process_mm) != current->mm) {
		err = -EINVAL;
		rcu_read_unlock();
		goto out_unlock;
	}
	rcu_read_unlock();

	switch (vma->vm_pgoff) {
	case PFN_DOWN(BASE_MEM_INVALID_HANDLE):
	case PFN_DOWN(BASE_MEM_WRITE_ALLOC_PAGES_HANDLE):
		/* Illegal handle for direct map */
		err = -EINVAL;
		goto out_unlock;
	case PFN_DOWN(BASE_MEM_TRACE_BUFFER_HANDLE):
		err = kbase_trace_buffer_mmap(kctx, vma, &reg, &kaddr);
		if (0 != err)
			goto out_unlock;
		dev_dbg(dev, "kbase_trace_buffer_mmap ok\n");
		/* free the region on munmap */
		free_on_close = 1;
		goto map;
	case PFN_DOWN(BASE_MEM_MMU_DUMP_HANDLE):
		/* MMU dump */
		err = kbase_mmu_dump_mmap(kctx, vma, &reg, &kaddr);
		if (0 != err)
			goto out_unlock;
		/* free the region on munmap */
		free_on_close = 1;
		goto map;
	case PFN_DOWN(BASE_MEM_COOKIE_BASE) ...
	     PFN_DOWN(BASE_MEM_FIRST_FREE_ADDRESS) - 1: {
		/* SAME_VA stuff, fetch the right region */
		int gpu_pc_bits;
		int cookie = vma->vm_pgoff - PFN_DOWN(BASE_MEM_COOKIE_BASE);

		gpu_pc_bits = kctx->kbdev->gpu_props.props.core_props.log2_program_counter_size;
		reg = kctx->pending_regions[cookie];
		if (!reg) {
			err = -ENOMEM;
			goto out_unlock;
		}

		if (reg->flags & KBASE_REG_ALIGNED) {
			/* nr_pages must be able to hold alignment pages
			 * plus actual pages */
			unsigned long align = 1ULL << gpu_pc_bits;
			unsigned long extra_pages = 3 * PFN_DOWN(align);
			unsigned long aligned_addr;
			unsigned long aligned_addr_end;
			unsigned long nr_bytes = reg->nr_pages << PAGE_SHIFT;

			if (kctx->api_version < KBASE_API_VERSION(8, 5))
				/* Maintain compatibility with old userspace */
				extra_pages = PFN_DOWN(align);

			if (nr_pages != reg->nr_pages + extra_pages) {
				/* incorrect mmap size */
				/* leave the cookie for a potential
				 * later mapping, or to be reclaimed
				 * later when the context is freed */
				err = -ENOMEM;
				goto out_unlock;
			}

			aligned_addr = ALIGN(vma->vm_start, align);
			aligned_addr_end = aligned_addr + nr_bytes;

			if (kctx->api_version >= KBASE_API_VERSION(8, 5)) {
				if ((aligned_addr_end & BASE_MEM_MASK_4GB) == 0) {
					/* Can't end at 4GB boundary */
					aligned_addr += 2 * align;
				} else if ((aligned_addr & BASE_MEM_MASK_4GB) == 0) {
					/* Can't start at 4GB boundary */
					aligned_addr += align;
				}
			}

			aligned_offset = aligned_addr - vma->vm_start;
		} else if (reg->nr_pages != nr_pages) {
			/* incorrect mmap size */
			/* leave the cookie for a potential later
			 * mapping, or to be reclaimed later when the
			 * context is freed */
			err = -ENOMEM;
			goto out_unlock;
		}

		if ((vma->vm_flags & VM_READ &&
					!(reg->flags & KBASE_REG_CPU_RD)) ||
				(vma->vm_flags & VM_WRITE &&
				 !(reg->flags & KBASE_REG_CPU_WR))) {
			/* VM flags inconsistent with region flags */
			err = -EPERM;
			dev_err(dev, "%s:%d inconsistent VM flags\n",
					__FILE__, __LINE__);
			goto out_unlock;
		}

		/* adjust down nr_pages to what we have physically */
		nr_pages = kbase_reg_current_backed_size(reg);

		if (kbase_gpu_mmap(kctx, reg,
					vma->vm_start + aligned_offset,
					reg->nr_pages, 1) != 0) {
			dev_err(dev, "%s:%d\n", __FILE__, __LINE__);
			/* Unable to map in GPU space. */
			WARN_ON(1);
			err = -ENOMEM;
			goto out_unlock;
		}

		/* no need for the cookie anymore */
		kctx->pending_regions[cookie] = NULL;
		kctx->cookies |= (1UL << cookie);

		/*
		 * Overwrite the offset with the
		 * region start_pfn, so we effectively
		 * map from offset 0 in the region.
		 */
		vma->vm_pgoff = reg->start_pfn;

		/* free the region on munmap */
		free_on_close = 1;
		goto map;
	}
	default: {
		reg = kbase_region_tracker_find_region_enclosing_address(kctx, (u64)vma->vm_pgoff << PAGE_SHIFT);

		if (reg && !(reg->flags & KBASE_REG_FREE)) {
			/* will this mapping overflow the size of the region? */
			if (nr_pages > (reg->nr_pages - (vma->vm_pgoff - reg->start_pfn)))
				goto overflow;

			if ((vma->vm_flags & VM_READ &&
			     !(reg->flags & KBASE_REG_CPU_RD)) ||
			    (vma->vm_flags & VM_WRITE &&
			     !(reg->flags & KBASE_REG_CPU_WR))) {
				/* VM flags inconsistent with region flags */
				err = -EPERM;
				dev_err(dev, "%s:%d inconsistent VM flags\n",
					__FILE__, __LINE__);
				goto out_unlock;
			}

#ifdef CONFIG_DMA_SHARED_BUFFER
			if (reg->cpu_alloc->type == KBASE_MEM_TYPE_IMPORTED_UMM)
				goto dma_map;
#endif /* CONFIG_DMA_SHARED_BUFFER */

			/* limit what we map to the amount currently backed */
			if (reg->cpu_alloc->nents < (vma->vm_pgoff - reg->start_pfn + nr_pages)) {
				if ((vma->vm_pgoff - reg->start_pfn) >= reg->cpu_alloc->nents)
					nr_pages = 0;
				else
					nr_pages = reg->cpu_alloc->nents - (vma->vm_pgoff - reg->start_pfn);
			}

			goto map;
		}

overflow:
		err = -ENOMEM;
		goto out_unlock;
	} /* default */
	} /* switch */
map:
	err = kbase_cpu_mmap(reg, vma, kaddr, nr_pages, aligned_offset, free_on_close);

	if (vma->vm_pgoff == PFN_DOWN(BASE_MEM_MMU_DUMP_HANDLE)) {
		/* MMU dump - userspace should now have a reference on
		 * the pages, so we can now free the kernel mapping */
		vfree(kaddr);
	}
	goto out_unlock;

#ifdef CONFIG_DMA_SHARED_BUFFER
dma_map:
	err = dma_buf_mmap(reg->cpu_alloc->imported.umm.dma_buf, vma, vma->vm_pgoff - reg->start_pfn);
#if defined(CONFIG_MALI_TRACE_TIMELINE)
	/* This section is required only for instrumentation. */
	/* Add created mapping to imported region mapping list.
	 * It is important to make it visible to dumping infrastructure.
	 * Add mapping only if vm_ops structure is not used by memory owner. */
	WARN_ON(vma->vm_ops);
	WARN_ON(vma->vm_private_data);
	if (!err && !vma->vm_ops && !vma->vm_private_data) {
		struct kbase_cpu_mapping *map = kzalloc(
			sizeof(*map),
			GFP_KERNEL);

		if (map) {
			map->kctx     = reg->kctx;
			map->region   = NULL;
			map->page_off = vma->vm_pgoff;
			map->vm_start = vma->vm_start;
			map->vm_end   = vma->vm_end;
			map->count    = 1; /* start with one ref */

			vma->vm_ops          = &kbase_dma_mmap_ops;
			vma->vm_private_data = map;

			list_add(
				&map->mappings_list,
				&reg->cpu_alloc->mappings);
		}
	}
#endif /* CONFIG_MALI_TRACE_TIMELINE */
#endif /* CONFIG_DMA_SHARED_BUFFER */
out_unlock:
	kbase_gpu_vm_unlock(kctx);
out:
	if (err)
		dev_err(dev, "mmap failed %d\n", err);

	return err;
}

KBASE_EXPORT_TEST_API(kbase_mmap);

void *kbase_vmap(struct kbase_context *kctx, u64 gpu_addr, size_t size,
		struct kbase_vmap_struct *map)
{
	struct kbase_va_region *reg;
	unsigned long page_index;
	unsigned int offset = gpu_addr & ~PAGE_MASK;
	size_t page_count = PFN_UP(offset + size);
	phys_addr_t *page_array;
	struct page **pages;
	void *cpu_addr = NULL;
	pgprot_t prot;
	size_t i;
	bool sync_needed;

	if (!size || !map)
		return NULL;

	/* check if page_count calculation will wrap */
	if (size > ((size_t)-1 / PAGE_SIZE))
		return NULL;

	kbase_gpu_vm_lock(kctx);

	reg = kbase_region_tracker_find_region_enclosing_address(kctx, gpu_addr);
	if (!reg || (reg->flags & KBASE_REG_FREE))
		goto out_unlock;

	page_index = (gpu_addr >> PAGE_SHIFT) - reg->start_pfn;

	/* check if page_index + page_count will wrap */
	if (-1UL - page_count < page_index)
		goto out_unlock;

	if (page_index + page_count > kbase_reg_current_backed_size(reg))
		goto out_unlock;

	page_array = kbase_get_cpu_phy_pages(reg);
	if (!page_array)
		goto out_unlock;

	pages = kmalloc_array(page_count, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		goto out_unlock;

	for (i = 0; i < page_count; i++)
		pages[i] = pfn_to_page(PFN_DOWN(page_array[page_index + i]));

	prot = PAGE_KERNEL;
	if (!(reg->flags & KBASE_REG_CPU_CACHED)) {
		/* Map uncached */
		prot = pgprot_writecombine(prot);
	}

	cpu_addr = vmap(pages, page_count, VM_MAP, prot);

	kfree(pages);

	if (!cpu_addr)
		goto out_unlock;

	map->gpu_addr = gpu_addr;
	map->cpu_alloc = kbase_mem_phy_alloc_get(reg->cpu_alloc);
	map->cpu_pages = &kbase_get_cpu_phy_pages(reg)[page_index];
	map->gpu_alloc = kbase_mem_phy_alloc_get(reg->gpu_alloc);
	map->gpu_pages = &kbase_get_gpu_phy_pages(reg)[page_index];
	map->addr = (void *)((uintptr_t)cpu_addr + offset);
	map->size = size;
	map->is_cached = (reg->flags & KBASE_REG_CPU_CACHED) != 0;
	sync_needed = map->is_cached;

	if (sync_needed) {
		/* Sync first page */
		size_t sz = MIN(((size_t) PAGE_SIZE - offset), size);
		phys_addr_t cpu_pa = map->cpu_pages[0];
		phys_addr_t gpu_pa = map->gpu_pages[0];

		kbase_sync_single(kctx, cpu_pa, gpu_pa, offset, sz,
				KBASE_SYNC_TO_CPU);

		/* Sync middle pages (if any) */
		for (i = 1; page_count > 2 && i < page_count - 1; i++) {
			cpu_pa = map->cpu_pages[i];
			gpu_pa = map->gpu_pages[i];
			kbase_sync_single(kctx, cpu_pa, gpu_pa, 0, PAGE_SIZE,
					KBASE_SYNC_TO_CPU);
		}

		/* Sync last page (if any) */
		if (page_count > 1) {
			cpu_pa = map->cpu_pages[page_count - 1];
			gpu_pa = map->gpu_pages[page_count - 1];
			sz = ((offset + size - 1) & ~PAGE_MASK) + 1;
			kbase_sync_single(kctx, cpu_pa, gpu_pa, 0, sz,
					KBASE_SYNC_TO_CPU);
		}
	}
	kbase_gpu_vm_unlock(kctx);

	return map->addr;

out_unlock:
	kbase_gpu_vm_unlock(kctx);
	return NULL;
}
KBASE_EXPORT_TEST_API(kbase_vmap);

void kbase_vunmap(struct kbase_context *kctx, struct kbase_vmap_struct *map)
{
	void *addr = (void *)((uintptr_t)map->addr & PAGE_MASK);
	bool sync_needed = map->is_cached;
	vunmap(addr);
	if (sync_needed) {
		off_t offset = (uintptr_t)map->addr & ~PAGE_MASK;
		size_t size = map->size;
		size_t page_count = PFN_UP(offset + size);
		size_t i;

		/* Sync first page */
		size_t sz = MIN(((size_t) PAGE_SIZE - offset), size);
		phys_addr_t cpu_pa = map->cpu_pages[0];
		phys_addr_t gpu_pa = map->gpu_pages[0];

		kbase_sync_single(kctx, cpu_pa, gpu_pa, offset, sz,
				KBASE_SYNC_TO_DEVICE);

		/* Sync middle pages (if any) */
		for (i = 1; page_count > 2 && i < page_count - 1; i++) {
			cpu_pa = map->cpu_pages[i];
			gpu_pa = map->gpu_pages[i];
			kbase_sync_single(kctx, cpu_pa, gpu_pa, 0, PAGE_SIZE,
					KBASE_SYNC_TO_DEVICE);
		}

		/* Sync last page (if any) */
		if (page_count > 1) {
			cpu_pa = map->cpu_pages[page_count - 1];
			gpu_pa = map->gpu_pages[page_count - 1];
			sz = ((offset + size - 1) & ~PAGE_MASK) + 1;
			kbase_sync_single(kctx, cpu_pa, gpu_pa, 0, sz,
					KBASE_SYNC_TO_DEVICE);
		}
	}
	map->gpu_addr = 0;
	map->cpu_alloc = kbase_mem_phy_alloc_put(map->cpu_alloc);
	map->gpu_alloc = kbase_mem_phy_alloc_put(map->gpu_alloc);
	map->cpu_pages = NULL;
	map->gpu_pages = NULL;
	map->addr = NULL;
	map->size = 0;
	map->is_cached = false;
}
KBASE_EXPORT_TEST_API(kbase_vunmap);

void kbasep_os_process_page_usage_update(struct kbase_context *kctx, int pages)
{
	struct mm_struct *mm;

	rcu_read_lock();
	mm = rcu_dereference(kctx->process_mm);
	if (mm) {
		atomic_add(pages, &kctx->nonmapped_pages);
#ifdef SPLIT_RSS_COUNTING
		add_mm_counter(mm, MM_FILEPAGES, pages);
#else
		spin_lock(&mm->page_table_lock);
		add_mm_counter(mm, MM_FILEPAGES, pages);
		spin_unlock(&mm->page_table_lock);
#endif
	}
	rcu_read_unlock();
}

static void kbasep_os_process_page_usage_drain(struct kbase_context *kctx)
{
	int pages;
	struct mm_struct *mm;

	spin_lock(&kctx->mm_update_lock);
	mm = rcu_dereference_protected(kctx->process_mm, lockdep_is_held(&kctx->mm_update_lock));
	if (!mm) {
		spin_unlock(&kctx->mm_update_lock);
		return;
	}

	rcu_assign_pointer(kctx->process_mm, NULL);
	spin_unlock(&kctx->mm_update_lock);
	synchronize_rcu();

	pages = atomic_xchg(&kctx->nonmapped_pages, 0);
#ifdef SPLIT_RSS_COUNTING
	add_mm_counter(mm, MM_FILEPAGES, -pages);
#else
	spin_lock(&mm->page_table_lock);
	add_mm_counter(mm, MM_FILEPAGES, -pages);
	spin_unlock(&mm->page_table_lock);
#endif
}

static void kbase_special_vm_close(struct vm_area_struct *vma)
{
	struct kbase_context *kctx;

	kctx = vma->vm_private_data;
	kbasep_os_process_page_usage_drain(kctx);
}

static const struct vm_operations_struct kbase_vm_special_ops = {
	.close = kbase_special_vm_close,
};

static int kbase_tracking_page_setup(struct kbase_context *kctx, struct vm_area_struct *vma)
{
	/* check that this is the only tracking page */
	spin_lock(&kctx->mm_update_lock);
	if (rcu_dereference_protected(kctx->process_mm, lockdep_is_held(&kctx->mm_update_lock))) {
		spin_unlock(&kctx->mm_update_lock);
		return -EFAULT;
	}

	rcu_assign_pointer(kctx->process_mm, current->mm);

	spin_unlock(&kctx->mm_update_lock);

	/* no real access */
	vma->vm_flags &= ~(VM_READ | VM_MAYREAD | VM_WRITE | VM_MAYWRITE | VM_EXEC | VM_MAYEXEC);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 7, 0))
	vma->vm_flags |= VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP | VM_IO;
#else
	vma->vm_flags |= VM_DONTCOPY | VM_DONTEXPAND | VM_RESERVED | VM_IO;
#endif
	vma->vm_ops = &kbase_vm_special_ops;
	vma->vm_private_data = kctx;

	return 0;
}
void *kbase_va_alloc(struct kbase_context *kctx, u32 size, struct kbase_hwc_dma_mapping *handle)
{
	int i;
	int res;
	void *va;
	dma_addr_t  dma_pa;
	struct kbase_va_region *reg;
	phys_addr_t *page_array;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0))
	DEFINE_DMA_ATTRS(attrs);
#endif

	u32 pages = ((size - 1) >> PAGE_SHIFT) + 1;
	u32 flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
		    BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;

	KBASE_DEBUG_ASSERT(kctx != NULL);
	KBASE_DEBUG_ASSERT(0 != size);
	KBASE_DEBUG_ASSERT(0 != pages);

	if (size == 0)
		goto err;

	/* All the alloc calls return zeroed memory */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0))
	///dma_set_attr(DMA_ATTR_WRITE_COMBINE, &attrs);
	///va = dma_alloc_attrs(kctx->kbdev->dev, size, &dma_pa, GFP_KERNEL, &attrs);
	va = dma_alloc_coherent(kctx->kbdev->dev, size, &dma_pa, GFP_KERNEL);
#else
	va = dma_alloc_writecombine(kctx->kbdev->dev, size, &dma_pa, GFP_KERNEL);
#endif
	if (!va)
		goto err;

	/* Store the state so we can free it later. */
	handle->cpu_va = va;
	handle->dma_pa = dma_pa;
	handle->size   = size;


	reg = kbase_alloc_free_region(kctx, 0, pages, KBASE_REG_ZONE_SAME_VA);
	if (!reg)
		goto no_reg;

	reg->flags &= ~KBASE_REG_FREE;
	kbase_update_region_flags(kctx, reg, flags);

	reg->cpu_alloc = kbase_alloc_create(pages, KBASE_MEM_TYPE_RAW);
	if (IS_ERR_OR_NULL(reg->cpu_alloc))
		goto no_alloc;

	reg->gpu_alloc = kbase_mem_phy_alloc_get(reg->cpu_alloc);

	page_array = kbase_get_cpu_phy_pages(reg);

	for (i = 0; i < pages; i++)
		page_array[i] = dma_pa + (i << PAGE_SHIFT);

	reg->cpu_alloc->nents = pages;

	kbase_gpu_vm_lock(kctx);
	res = kbase_gpu_mmap(kctx, reg, (uintptr_t) va, pages, 1);
	kbase_gpu_vm_unlock(kctx);
	if (res)
		goto no_mmap;

	return va;

no_mmap:
	kbase_mem_phy_alloc_put(reg->cpu_alloc);
	kbase_mem_phy_alloc_put(reg->gpu_alloc);
no_alloc:
	kfree(reg);
no_reg:
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0))
	///dma_free_attrs(kctx->kbdev->dev, size, va, dma_pa, &attrs);
	dma_free_coherent(kctx->kbdev->dev, size, va, dma_pa);
#else
	dma_free_writecombine(kctx->kbdev->dev, size, va, dma_pa);
#endif
err:
	return NULL;
}
KBASE_EXPORT_SYMBOL(kbase_va_alloc);

void kbase_va_free(struct kbase_context *kctx, struct kbase_hwc_dma_mapping *handle)
{
	struct kbase_va_region *reg;
	int err;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0))
	DEFINE_DMA_ATTRS(attrs);
#endif

	KBASE_DEBUG_ASSERT(kctx != NULL);
	KBASE_DEBUG_ASSERT(handle->cpu_va != NULL);

	kbase_gpu_vm_lock(kctx);
	reg = kbase_region_tracker_find_region_base_address(kctx, (uintptr_t)handle->cpu_va);
	KBASE_DEBUG_ASSERT(reg);
	err = kbase_gpu_munmap(kctx, reg);
	kbase_gpu_vm_unlock(kctx);
	KBASE_DEBUG_ASSERT(!err);

	kbase_mem_phy_alloc_put(reg->cpu_alloc);
	kbase_mem_phy_alloc_put(reg->gpu_alloc);
	kfree(reg);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0))
	///dma_set_attr(DMA_ATTR_WRITE_COMBINE, &attrs);
	///dma_free_attrs(kctx->kbdev->dev, handle->size,
	///		handle->cpu_va, handle->dma_pa, &attrs);
        dma_free_coherent(kctx->kbdev->dev, handle->size,
                        handle->cpu_va, handle->dma_pa);
#else
	dma_free_writecombine(kctx->kbdev->dev, handle->size,
				handle->cpu_va, handle->dma_pa);
#endif
}
KBASE_EXPORT_SYMBOL(kbase_va_free);

