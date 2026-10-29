// SPDX-License-Identifier: GPL-2.0
/*
 * Device-memory allocator: first fit over a sorted list of free extents, merging neighbours
 * on free. Device memory is carved into buffer objects of 4 KiB granularity; the number of
 * live objects is small (one per tensor), so a list is enough and easy to audit.
 */

#include <linux/list.h>
#include <linux/slab.h>

#include "lacc.h"

struct lacc_extent {
	struct list_head node;
	u64 start, size;
};

int lacc_mem_init(struct lacc_mem *mem, u64 size)
{
	struct lacc_extent *e = kzalloc(sizeof(*e), GFP_KERNEL);

	if (!e)
		return -ENOMEM;
	mutex_init(&mem->lock);
	INIT_LIST_HEAD(&mem->free);
	e->start = 0;
	e->size = size;
	list_add(&e->node, &mem->free);
	mem->size = size;
	mem->used = 0;
	return 0;
}

void lacc_mem_fini(struct lacc_mem *mem)
{
	struct lacc_extent *e, *tmp;

	list_for_each_entry_safe(e, tmp, &mem->free, node) {
		list_del(&e->node);
		kfree(e);
	}
}

int lacc_mem_alloc(struct lacc_mem *mem, u64 size, u64 *offset)
{
	struct lacc_extent *e;
	int ret = -ENOSPC;

	size = round_up(size, LACC_BO_ALIGN);
	mutex_lock(&mem->lock);
	list_for_each_entry(e, &mem->free, node) {
		if (e->size < size)
			continue;
		*offset = e->start;
		e->start += size;
		e->size -= size;
		if (!e->size) {
			list_del(&e->node);
			kfree(e);
		}
		mem->used += size;
		ret = 0;
		break;
	}
	mutex_unlock(&mem->lock);
	return ret;
}

void lacc_mem_free(struct lacc_mem *mem, u64 offset, u64 size)
{
	struct lacc_extent *e, *prev = NULL, *n;

	size = round_up(size, LACC_BO_ALIGN);
	mutex_lock(&mem->lock);
	mem->used -= size;
	/* find the first free extent after the freed range */
	list_for_each_entry(e, &mem->free, node) {
		if (e->start > offset)
			break;
		prev = e;
	}
	if (prev && prev->start + prev->size == offset) {
		prev->size += size;
		n = list_is_last(&prev->node, &mem->free) ? NULL : list_next_entry(prev, node);
		if (n && prev->start + prev->size == n->start) {
			prev->size += n->size;
			list_del(&n->node);
			kfree(n);
		}
		goto out;
	}
	n = prev ? (list_is_last(&prev->node, &mem->free) ? NULL : list_next_entry(prev, node)) :
		   list_first_entry_or_null(&mem->free, struct lacc_extent, node);
	if (n && offset + size == n->start) {
		n->start = offset;
		n->size += size;
		goto out;
	}
	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) {
		/* the range is lost until reload; never happens in practice, but do not corrupt */
		pr_warn_ratelimited("lacc: lost %llu bytes of device memory\n", size);
		goto out;
	}
	e->start = offset;
	e->size = size;
	if (prev)
		list_add(&e->node, &prev->node);
	else
		list_add(&e->node, &mem->free);
out:
	mutex_unlock(&mem->lock);
}
