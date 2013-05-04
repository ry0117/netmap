/*
 * Copyright (C) 2012-2013 Matteo Landi, Luigi Rizzo, Giuseppe Lettieri. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef linux
#include "bsd_glue.h"
#endif /* linux */

#ifdef __APPLE__
#include "osx_glue.h"
#endif /* __APPLE__ */

#ifdef __FreeBSD__
#include <sys/cdefs.h> /* prerequisite */
__FBSDID("$FreeBSD: head/sys/dev/netmap/netmap.c 241723 2012-10-19 09:41:45Z glebius $");

#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <vm/vm.h>	/* vtophys */
#include <vm/pmap.h>	/* vtophys */
#include <sys/socket.h> /* sockaddrs */
#include <sys/selinfo.h>
#include <sys/sysctl.h>
#include <net/if.h>
#include <net/vnet.h>
#include <machine/bus.h>	/* bus_dmamap_* */

#endif /* __FreeBSD__ */

#include <net/netmap.h>
#include <dev/netmap/netmap_kern.h>
#include "netmap_mem2.h"

#ifdef linux
#define NMA_LOCK_INIT()		sema_init(&nm_mem.nm_mtx, 1)
#define NMA_LOCK_DESTROY()	
#define NMA_LOCK()		down(&nm_mem.nm_mtx)
#define NMA_UNLOCK()		up(&nm_mem.nm_mtx)
#else /* !linux */
#define NMA_LOCK_INIT()		mtx_init(&nm_mem.nm_mtx, "netmap memory allocator lock", NULL, MTX_DEF)
#define NMA_LOCK_DESTROY()	mtx_destroy(&nm_mem.nm_mtx)
#define NMA_LOCK()		mtx_lock(&nm_mem.nm_mtx)
#define NMA_UNLOCK()		mtx_unlock(&nm_mem.nm_mtx)
#endif /* linux */

struct netmap_obj_params netmap_params[NETMAP_POOLS_NR] = {
	[NETMAP_IF_POOL] = {
		.size = 1024,
		.num  = 100,
	},
	[NETMAP_RING_POOL] = {
		.size = 9*PAGE_SIZE,
		.num  = 200,
	},
	[NETMAP_BUF_POOL] = {
		.size = 2048,
		.num  = NETMAP_BUF_MAX_NUM,
	},
};


/*
 * nm_mem is the memory allocator used for all physical interfaces
 * running in netmap mode.
 * Virtual (VALE) ports will have each its own allocator.
 */
struct netmap_mem_d nm_mem = {	/* Our memory allocator. */
	.pools = {
		[NETMAP_IF_POOL] = {
			.name 	= "netmap_if",
			.objminsize = sizeof(struct netmap_if),
			.objmaxsize = 4096,
			.nummin     = 10,	/* don't be stingy */
			.nummax	    = 10000,	/* XXX very large */
		},
		[NETMAP_RING_POOL] = {
			.name 	= "netmap_ring",
			.objminsize = sizeof(struct netmap_ring),
			.objmaxsize = 32*PAGE_SIZE,
			.nummin     = 2,
			.nummax	    = 1024,
		},
		[NETMAP_BUF_POOL] = {
			.name	= "netmap_buf",
			.objminsize = 64,
			.objmaxsize = 65536,
			.nummin     = 4,
			.nummax	    = 1000000, /* one million! */
		},
	},
};

// XXX logically belongs to nm_mem
struct lut_entry *netmap_buffer_lut;	/* exported */

/* memory allocator related sysctls */

#define STRINGIFY(x) #x


#define DECLARE_SYSCTLS(id, name) \
	SYSCTL_INT(_dev_netmap, OID_AUTO, name##_size, \
	    CTLFLAG_RW, &netmap_params[id].size, 0, "Requested size of netmap " STRINGIFY(name) "s"); \
        SYSCTL_INT(_dev_netmap, OID_AUTO, name##_curr_size, \
            CTLFLAG_RD, &nm_mem.pools[id]._objsize, 0, "Current size of netmap " STRINGIFY(name) "s"); \
        SYSCTL_INT(_dev_netmap, OID_AUTO, name##_num, \
            CTLFLAG_RW, &netmap_params[id].num, 0, "Requested number of netmap " STRINGIFY(name) "s"); \
        SYSCTL_INT(_dev_netmap, OID_AUTO, name##_curr_num, \
            CTLFLAG_RD, &nm_mem.pools[id].objtotal, 0, "Current number of netmap " STRINGIFY(name) "s")

SYSCTL_DECL(_dev_netmap);
DECLARE_SYSCTLS(NETMAP_IF_POOL, if);
DECLARE_SYSCTLS(NETMAP_RING_POOL, ring);
DECLARE_SYSCTLS(NETMAP_BUF_POOL, buf);

/*
 * Convert a userspace offset to a physical address.
 * XXX only called in the FreeBSD's netmap_mmap()
 * because in linux we map everything at once.
 *
 * First, find the allocator that contains the requested offset,
 * then locate the cluster through a lookup table.
 */
vm_paddr_t
netmap_mem_ofstophys(vm_ooffset_t offset)
{
	int i;
	vm_offset_t o = offset;
	vm_paddr_t pa;
	struct netmap_obj_pool *p;

	NMA_LOCK();
	p = nm_mem.pools;

	for (i = 0; i < NETMAP_POOLS_NR; offset -= p[i]._memtotal, i++) {
		if (offset >= p[i]._memtotal)
			continue;
		// now lookup the cluster's address
		pa = p[i].lut[offset / p[i]._objsize].paddr +
			offset % p[i]._objsize;
		NMA_UNLOCK();
		return pa;
	}
	/* this is only in case of errors */
	D("invalid ofs 0x%x out of 0x%x 0x%x 0x%x", (u_int)o,
		p[NETMAP_IF_POOL]._memtotal,
		p[NETMAP_IF_POOL]._memtotal
			+ p[NETMAP_RING_POOL]._memtotal,
		p[NETMAP_IF_POOL]._memtotal
			+ p[NETMAP_RING_POOL]._memtotal
			+ p[NETMAP_BUF_POOL]._memtotal);
	NMA_UNLOCK();
	return 0;	// XXX bad address
}

/*
 * we store objects by kernel address, need to find the offset
 * within the pool to export the value to userspace.
 * Algorithm: scan until we find the cluster, then add the
 * actual offset in the cluster
 */
static ssize_t
netmap_obj_offset(struct netmap_obj_pool *p, const void *vaddr)
{
	int i, k = p->clustentries, n = p->objtotal;
	ssize_t ofs = 0;

	for (i = 0; i < n; i += k, ofs += p->_clustsize) {
		const char *base = p->lut[i].vaddr;
		ssize_t relofs = (const char *) vaddr - base;

		if (relofs < 0 || relofs >= p->_clustsize)
			continue;

		ofs = ofs + relofs;
		ND("%s: return offset %d (cluster %d) for pointer %p",
		    p->name, ofs, i, vaddr);
		return ofs;
	}
	D("address %p is not contained inside any cluster (%s)",
	    vaddr, p->name);
	return 0; /* An error occurred */
}

/* Helper functions which convert virtual addresses to offsets */
#define netmap_if_offset(v)					\
	netmap_obj_offset(&nm_mem.pools[NETMAP_IF_POOL], (v))

#define netmap_ring_offset(v)					\
    (nm_mem.pools[NETMAP_IF_POOL]._memtotal + 			\
	netmap_obj_offset(&nm_mem.pools[NETMAP_RING_POOL], (v)))

#define netmap_buf_offset(v)					\
    (nm_mem.pools[NETMAP_IF_POOL]._memtotal +			\
	nm_mem.pools[NETMAP_RING_POOL]._memtotal +		\
	netmap_obj_offset(&nm_mem.pools[NETMAP_BUF_POOL], (v)))


ssize_t
netmap_mem_if_offset(const void *addr)
{
	ssize_t v;
	NMA_LOCK();
	v = netmap_if_offset(addr);
	NMA_UNLOCK();
	return v;
}

/*
 * report the index, and use start position as a hint,
 * otherwise buffer allocation becomes terribly expensive.
 */
static void *
netmap_obj_malloc(struct netmap_obj_pool *p, u_int len, uint32_t *start, uint32_t *index)
{
	uint32_t i = 0;			/* index in the bitmap */
	uint32_t mask, j;		/* slot counter */
	void *vaddr = NULL;

	if (len > p->_objsize) {
		D("%s request size %d too large", p->name, len);
		// XXX cannot reduce the size
		return NULL;
	}

	if (p->objfree == 0) {
		D("%s allocator: run out of memory", p->name);
		return NULL;
	}
	if (start)
		i = *start;

	/* termination is guaranteed by p->free, but better check bounds on i */
	while (vaddr == NULL && i < p->bitmap_slots)  {
		uint32_t cur = p->bitmap[i];
		if (cur == 0) { /* bitmask is fully used */
			i++;
			continue;
		}
		/* locate a slot */
		for (j = 0, mask = 1; (cur & mask) == 0; j++, mask <<= 1)
			;

		p->bitmap[i] &= ~mask; /* mark object as in use */
		p->objfree--;

		vaddr = p->lut[i * 32 + j].vaddr;
		if (index)
			*index = i * 32 + j;
	}
	ND("%s allocator: allocated object @ [%d][%d]: vaddr %p", i, j, vaddr);

	if (start)
		*start = i;
	return vaddr;
}


/*
 * free by index, not by address. This is slow, but is only used
 * for a small number of objects (rings, nifp)
 */
static void
netmap_obj_free(struct netmap_obj_pool *p, uint32_t j)
{
	if (j >= p->objtotal) {
		D("invalid index %u, max %u", j, p->objtotal);
		return;
	}
	p->bitmap[j / 32] |= (1 << (j % 32));
	p->objfree++;
	return;
}

static void
netmap_obj_free_va(struct netmap_obj_pool *p, void *vaddr)
{
	u_int i, j, n = p->_memtotal / p->_clustsize;

	for (i = 0, j = 0; i < n; i++, j += p->clustentries) {
		void *base = p->lut[i * p->clustentries].vaddr;
		ssize_t relofs = (ssize_t) vaddr - (ssize_t) base;

		/* Given address, is out of the scope of the current cluster.*/
		if (vaddr < base || relofs >= p->_clustsize)
			continue;

		j = j + relofs / p->_objsize;
		KASSERT(j != 0, ("Cannot free object 0"));
		netmap_obj_free(p, j);
		return;
	}
	D("address %p is not contained inside any cluster (%s)",
	    vaddr, p->name);
}

#define netmap_if_malloc(len)	netmap_obj_malloc(&nm_mem.pools[NETMAP_IF_POOL], len, NULL, NULL)
#define netmap_if_free(v)	netmap_obj_free_va(&nm_mem.pools[NETMAP_IF_POOL], (v))
#define netmap_ring_malloc(len)	netmap_obj_malloc(&nm_mem.pools[NETMAP_RING_POOL], len, NULL, NULL)
#define netmap_ring_free(v)	netmap_obj_free_va(&nm_mem.pools[NETMAP_RING_POOL], (v))
#define netmap_buf_malloc(_pos, _index)			\
	netmap_obj_malloc(&nm_mem.pools[NETMAP_BUF_POOL], NETMAP_BUF_SIZE, _pos, _index)


/* Return the index associated to the given packet buffer */
#define netmap_buf_index(v)						\
    (netmap_obj_offset(&nm_mem.pools[NETMAP_BUF_POOL], (v)) / nm_mem.pools[NETMAP_BUF_POOL]._objsize)


/* Return nonzero on error */
static int
netmap_new_bufs(struct netmap_if *nifp,
                struct netmap_slot *slot, u_int n)
{
	struct netmap_obj_pool *p = &nm_mem.pools[NETMAP_BUF_POOL];
	u_int i = 0;	/* slot counter */
	uint32_t pos = 0;	/* slot in p->bitmap */
	uint32_t index = 0;	/* buffer index */

	(void)nifp;	/* UNUSED */
	for (i = 0; i < n; i++) {
		void *vaddr = netmap_buf_malloc(&pos, &index);
		if (vaddr == NULL) {
			D("unable to locate empty packet buffer");
			goto cleanup;
		}
		slot[i].buf_idx = index;
		slot[i].len = p->_objsize;
		/* XXX setting flags=NS_BUF_CHANGED forces a pointer reload
		 * in the NIC ring. This is a hack that hides missing
		 * initializations in the drivers, and should go away.
		 */
		// slot[i].flags = NS_BUF_CHANGED;
	}

	ND("allocated %d buffers, %d available, first at %d", n, p->objfree, pos);
	return (0);

cleanup:
	while (i > 0) {
		i--;
		netmap_obj_free(p, slot[i].buf_idx);
	}
	bzero(slot, n * sizeof(slot[0]));
	return (ENOMEM);
}


static void
netmap_free_buf(struct netmap_if *nifp, uint32_t i)
{
	struct netmap_obj_pool *p = &nm_mem.pools[NETMAP_BUF_POOL];

	(void)nifp;
	if (i < 2 || i >= p->objtotal) {
		D("Cannot free buf#%d: should be in [2, %d[", i, p->objtotal);
		return;
	}
	netmap_obj_free(p, i);
}

static void
netmap_reset_obj_allocator(struct netmap_obj_pool *p)
{

	if (p == NULL)
		return;
	if (p->bitmap)
		free(p->bitmap, M_NETMAP);
	p->bitmap = NULL;
	if (p->lut) {
		u_int i;
		size_t sz = p->_clustsize;

		for (i = 0; i < p->objtotal; i += p->clustentries) {
			if (p->lut[i].vaddr)
				contigfree(p->lut[i].vaddr, sz, M_NETMAP);
		}
		bzero(p->lut, sizeof(struct lut_entry) * p->objtotal);
#ifdef linux
		vfree(p->lut);
#else
		free(p->lut, M_NETMAP);
#endif
	}
	p->lut = NULL;
}

/*
 * Free all resources related to an allocator.
 */
static void
netmap_destroy_obj_allocator(struct netmap_obj_pool *p)
{
	if (p == NULL)
		return;
	netmap_reset_obj_allocator(p);
}

/*
 * We receive a request for objtotal objects, of size objsize each.
 * Internally we may round up both numbers, as we allocate objects
 * in small clusters multiple of the page size.
 * In the allocator we don't need to store the objsize,
 * but we do need to keep track of objtotal' and clustentries,
 * as they are needed when freeing memory.
 *
 * XXX note -- userspace needs the buffers to be contiguous,
 *	so we cannot afford gaps at the end of a cluster.
 */


/* call with NMA_LOCK held */
static int
netmap_config_obj_allocator(struct netmap_obj_pool *p, u_int objtotal, u_int objsize)
{
	int i, n;
	u_int clustsize;	/* the cluster size, multiple of page size */
	u_int clustentries;	/* how many objects per entry */

#define MAX_CLUSTSIZE	(1<<17)
#define LINE_ROUND	64
	if (objsize >= MAX_CLUSTSIZE) {
		/* we could do it but there is no point */
		D("unsupported allocation for %d bytes", objsize);
		goto error;
	}
	/* make sure objsize is a multiple of LINE_ROUND */
	i = (objsize & (LINE_ROUND - 1));
	if (i) {
		D("XXX aligning object by %d bytes", LINE_ROUND - i);
		objsize += LINE_ROUND - i;
	}
	if (objsize < p->objminsize || objsize > p->objmaxsize) {
		D("requested objsize %d out of range [%d, %d]",
			objsize, p->objminsize, p->objmaxsize);
		goto error;
	}
	if (objtotal < p->nummin || objtotal > p->nummax) {
		D("requested objtotal %d out of range [%d, %d]",
			objtotal, p->nummin, p->nummax);
		goto error;
	}
	/*
	 * Compute number of objects using a brute-force approach:
	 * given a max cluster size,
	 * we try to fill it with objects keeping track of the
	 * wasted space to the next page boundary.
	 */
	for (clustentries = 0, i = 1;; i++) {
		u_int delta, used = i * objsize;
		if (used > MAX_CLUSTSIZE)
			break;
		delta = used % PAGE_SIZE;
		if (delta == 0) { // exact solution
			clustentries = i;
			break;
		}
		if (delta > ( (clustentries*objsize) % PAGE_SIZE) )
			clustentries = i;
	}
	// D("XXX --- ouch, delta %d (bad for buffers)", delta);
	/* compute clustsize and round to the next page */
	clustsize = clustentries * objsize;
	i =  (clustsize & (PAGE_SIZE - 1));
	if (i)
		clustsize += PAGE_SIZE - i;
	if (netmap_verbose)
		D("objsize %d clustsize %d objects %d",
			objsize, clustsize, clustentries);

	/*
	 * The number of clusters is n = ceil(objtotal/clustentries)
	 * objtotal' = n * clustentries
	 */
	p->clustentries = clustentries;
	p->_clustsize = clustsize;
	n = (objtotal + clustentries - 1) / clustentries;
	p->_numclusters = n;
	p->objtotal = n * clustentries;
	p->objfree = p->objtotal - 2; /* obj 0 and 1 are reserved */
	p->_memtotal = p->_numclusters * p->_clustsize;
	p->_objsize = objsize;

	return 0;

error:
	p->_objsize = objsize;
	p->objtotal = objtotal;

	return EINVAL;
}


/* call with NMA_LOCK held */
static int
netmap_finalize_obj_allocator(struct netmap_obj_pool *p)
{
	int i; /* must be signed */
	size_t n;

	n = sizeof(struct lut_entry) * p->objtotal;
#ifdef linux
	p->lut = vmalloc(n);
#else
	p->lut = malloc(n, M_NETMAP, M_NOWAIT | M_ZERO);
#endif
	if (p->lut == NULL) {
		D("Unable to create lookup table (%d bytes) for '%s'", (int)n, p->name);
		goto clean;
	}

	/* Allocate the bitmap */
	n = (p->objtotal + 31) / 32;
	p->bitmap = malloc(sizeof(uint32_t) * n, M_NETMAP, M_NOWAIT | M_ZERO);
	if (p->bitmap == NULL) {
		D("Unable to create bitmap (%d entries) for allocator '%s'", (int)n,
		    p->name);
		goto clean;
	}
	p->bitmap_slots = n;

	/*
	 * Allocate clusters, init pointers and bitmap
	 */
	n = p->_clustsize;
	for (i = 0; i < (int)p->objtotal;) {
		int lim = i + p->clustentries;
		char *clust;

		clust = contigmalloc(n, M_NETMAP, M_NOWAIT | M_ZERO,
		    (size_t)0, -1UL, PAGE_SIZE, 0);
		if (clust == NULL) {
			/*
			 * If we get here, there is a severe memory shortage,
			 * so halve the allocated memory to reclaim some.
			 * XXX check boundaries
			 */
			D("Unable to create cluster at %d for '%s' allocator",
			    i, p->name);
			lim = i / 2;
			for (i--; i >= lim; i--) {
				p->bitmap[ (i>>5) ] &=  ~( 1 << (i & 31) );
				if (i % p->clustentries == 0 && p->lut[i].vaddr)
					contigfree(p->lut[i].vaddr,
						n, M_NETMAP);
			}
			p->objtotal = i;
			p->objfree = p->objtotal - 2;
			p->_numclusters = i / p->clustentries;
			p->_memtotal = p->_numclusters * p->_clustsize;
			break;
		}
		for (; i < lim; i++, clust += p->_objsize) {
			p->bitmap[ (i>>5) ] |=  ( 1 << (i & 31) );
			p->lut[i].vaddr = clust;
			p->lut[i].paddr = vtophys(clust);
		}
	}
	p->bitmap[0] = (uint32_t)(~3); /* objs 0 and 1 is always busy */
	if (netmap_verbose)
		D("Pre-allocated %d clusters (%d/%dKB) for '%s'",
		    p->_numclusters, p->_clustsize >> 10,
		    p->_memtotal >> 10, p->name);

	return 0;

clean:
	netmap_reset_obj_allocator(p);
	return ENOMEM;
}

/* call with lock held */
static int
netmap_memory_config_changed(void)
{
	int i;

	for (i = 0; i < NETMAP_POOLS_NR; i++) {
		if (nm_mem.pools[i]._objsize != netmap_params[i].size ||
		    nm_mem.pools[i].objtotal != netmap_params[i].num)
		    return 1;
	}
	return 0;
}


/* call with lock held */
static int
netmap_memory_config(void)
{
	int i;

	if (!netmap_memory_config_changed())
		goto out;

	D("reconfiguring");

	if (nm_mem.finalized) {
		/* reset previous allocation */
		for (i = 0; i < NETMAP_POOLS_NR; i++) {
			netmap_reset_obj_allocator(&nm_mem.pools[i]);
		}
		nm_mem.finalized = 0;
        }

	for (i = 0; i < NETMAP_POOLS_NR; i++) {
		nm_mem.lasterr = netmap_config_obj_allocator(&nm_mem.pools[i],
				netmap_params[i].num, netmap_params[i].size);
		if (nm_mem.lasterr)
			goto out;
	}

	D("Have %d KB for interfaces, %d KB for rings and %d MB for buffers",
	    nm_mem.pools[NETMAP_IF_POOL]._memtotal >> 10,
	    nm_mem.pools[NETMAP_RING_POOL]._memtotal >> 10,
	    nm_mem.pools[NETMAP_BUF_POOL]._memtotal >> 20);

out:

	return nm_mem.lasterr;
}

int
netmap_mem_finalize(void)
{
	int i, err;
	u_int totalsize = 0;

	NMA_LOCK();

	nm_mem.refcount++;
	if (nm_mem.refcount > 1) {
		ND("busy (refcount %d)", nm_mem.refcount);
		goto out;
	}

	/* update configuration if changed */
	if (netmap_memory_config())
		goto out;

	if (nm_mem.finalized) {
		/* may happen if config is not changed */
		ND("nothing to do");
		goto out;
	}

	for (i = 0; i < NETMAP_POOLS_NR; i++) {
		nm_mem.lasterr = netmap_finalize_obj_allocator(&nm_mem.pools[i]);
		if (nm_mem.lasterr)
			goto cleanup;
		totalsize += nm_mem.pools[i]._memtotal;
	}
	nm_mem.nm_totalsize = totalsize;

	/* backward compatibility */
	netmap_buf_size = nm_mem.pools[NETMAP_BUF_POOL]._objsize;
	netmap_total_buffers = nm_mem.pools[NETMAP_BUF_POOL].objtotal;

	netmap_buffer_lut = nm_mem.pools[NETMAP_BUF_POOL].lut;
	netmap_buffer_base = nm_mem.pools[NETMAP_BUF_POOL].lut[0].vaddr;

	nm_mem.finalized = 1;
	nm_mem.lasterr = 0;

	/* make sysctl values match actual values in the pools */
	for (i = 0; i < NETMAP_POOLS_NR; i++) {
		netmap_params[i].size = nm_mem.pools[i]._objsize;
		netmap_params[i].num  = nm_mem.pools[i].objtotal;
	}

out:
	if (nm_mem.lasterr)
		nm_mem.refcount--;
	err = nm_mem.lasterr;

	NMA_UNLOCK();

	return err;

cleanup:
	for (i = 0; i < NETMAP_POOLS_NR; i++) {
		netmap_reset_obj_allocator(&nm_mem.pools[i]);
	}
	nm_mem.refcount--;
	err = nm_mem.lasterr;

	NMA_UNLOCK();

	return err;
}

int
netmap_mem_init(void)
{
	NMA_LOCK_INIT();
	return (0);
}

void
netmap_mem_fini(void)
{
	int i;

	for (i = 0; i < NETMAP_POOLS_NR; i++) {
	    netmap_destroy_obj_allocator(&nm_mem.pools[i]);
	}
	NMA_LOCK_DESTROY();
}

static void
netmap_free_rings(struct netmap_adapter *na)
{
	u_int i;
	if (!na->tx_rings)
		return;
	for (i = 0; i < na->num_tx_rings + 1; i++) {
		netmap_ring_free(na->tx_rings[i].ring);
		na->tx_rings[i].ring = NULL;
	}
	for (i = 0; i < na->num_rx_rings + 1; i++) {
		netmap_ring_free(na->rx_rings[i].ring);
		na->rx_rings[i].ring = NULL;
	}
	free(na->tx_rings, M_DEVBUF);
	na->tx_rings = na->rx_rings = NULL;
}



/* call with NMA_LOCK held */
/*
 * Allocate the per-fd structure netmap_if.
 * If this is the first instance, also allocate the krings, rings etc.
 *
 * We assume that the configuration stored in na
 * (number of tx/rx rings and descs) does not change while
 * the interface is in netmap mode.
 */
extern int nma_is_vp(struct netmap_adapter *na);
void *
netmap_mem_if_new(const char *ifname, struct netmap_adapter *na)
{
	struct netmap_if *nifp;
	struct netmap_ring *ring;
	ssize_t base; /* handy for relative offsets between rings and nifp */
	u_int i, len, ndesc, ntx, nrx;
	struct netmap_kring *kring;
	uint32_t *nkr_leases = NULL;

	/*
	 * verify whether virtual port need the stack ring
	 */
	ntx = na->num_tx_rings + 1; /* shorthand, include stack ring */
	nrx = na->num_rx_rings + 1; /* shorthand, include stack ring */
	/*
	 * the descriptor is followed inline by an array of offsets
	 * to the tx and rx rings in the shared memory region.
	 * For virtual rx rings we also allocate an array of
	 * pointers to assign to nkr_leases.
	 */

	NMA_LOCK();

	len = sizeof(struct netmap_if) + (nrx + ntx) * sizeof(ssize_t);
	nifp = netmap_if_malloc(len);
	if (nifp == NULL) {
		NMA_UNLOCK();
		return NULL;
	}

	/* initialize base fields -- override const */
	*(int *)(uintptr_t)&nifp->ni_tx_rings = na->num_tx_rings;
	*(int *)(uintptr_t)&nifp->ni_rx_rings = na->num_rx_rings;
	strncpy(nifp->ni_name, ifname, (size_t)IFNAMSIZ);

	if (na->refcount) { /* already setup, we are done */
		goto final;
	}

	len = (ntx + nrx) * sizeof(struct netmap_kring);
	if (nma_is_vp(na)) {
		D("VP %s allocate leases for %d rings %d slots",
			ifname, na->num_rx_rings, na->num_rx_desc);
		len += sizeof(uint32_t) * na->num_rx_desc * na->num_rx_rings;
	}
	na->tx_rings = malloc((size_t)len, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (na->tx_rings == NULL) {
		D("Cannot allocate krings for %s", ifname);
		goto cleanup;
	}
	na->rx_rings = na->tx_rings + ntx;
	if (nma_is_vp(na))
		nkr_leases = (uint32_t *)(na->rx_rings + nrx);

	/*
	 * First instance, allocate netmap rings and buffers for this card
	 * The rings are contiguous, but have variable size.
	 */
	for (i = 0; i < ntx; i++) { /* Transmit rings */
		kring = &na->tx_rings[i];
		ndesc = na->num_tx_desc;
		bzero(kring, sizeof(*kring));
		len = sizeof(struct netmap_ring) +
			  ndesc * sizeof(struct netmap_slot);
		ring = netmap_ring_malloc(len);
		if (ring == NULL) {
			D("Cannot allocate tx_ring[%d] for %s", i, ifname);
			goto cleanup;
		}
		ND("txring[%d] at %p ofs %d", i, ring);
		kring->na = na;
		kring->ring = ring;
		*(int *)(uintptr_t)&ring->num_slots = kring->nkr_num_slots = ndesc;
		*(ssize_t *)(uintptr_t)&ring->buf_ofs =
		    (nm_mem.pools[NETMAP_IF_POOL]._memtotal +
			nm_mem.pools[NETMAP_RING_POOL]._memtotal) -
			netmap_ring_offset(ring);

		/*
		 * IMPORTANT:
		 * Always keep one slot empty, so we can detect new
		 * transmissions comparing cur and nr_hwcur (they are
		 * the same only if there are no new transmissions).
		 */
		ring->avail = kring->nr_hwavail = ndesc - 1;
		ring->cur = kring->nr_hwcur = 0;
		*(int *)(uintptr_t)&ring->nr_buf_size = NETMAP_BUF_SIZE;
		ND("initializing slots for txring[%d]", i);
		if (netmap_new_bufs(nifp, ring->slot, ndesc)) {
			D("Cannot allocate buffers for tx_ring[%d] for %s", i, ifname);
			goto cleanup;
		}
	}

	for (i = 0; i < nrx; i++) { /* Receive rings */
		kring = &na->rx_rings[i];
		ndesc = na->num_rx_desc;
		bzero(kring, sizeof(*kring));
		len = sizeof(struct netmap_ring) +
			  ndesc * sizeof(struct netmap_slot);
		ring = netmap_ring_malloc(len);
		if (ring == NULL) {
			D("Cannot allocate rx_ring[%d] for %s", i, ifname);
			goto cleanup;
		}
		ND("rxring[%d] at %p ofs %d", i, ring);

		kring->na = na;
		kring->ring = ring;
		if (nkr_leases && i < na->num_rx_rings) {
			kring->nkr_leases = nkr_leases;
			nkr_leases += ndesc;
		}
		*(int *)(uintptr_t)&ring->num_slots = kring->nkr_num_slots = ndesc;
		*(ssize_t *)(uintptr_t)&ring->buf_ofs =
		    (nm_mem.pools[NETMAP_IF_POOL]._memtotal +
		        nm_mem.pools[NETMAP_RING_POOL]._memtotal) -
			netmap_ring_offset(ring);

		ring->cur = kring->nr_hwcur = 0;
		ring->avail = kring->nr_hwavail = 0; /* empty */
		*(int *)(uintptr_t)&ring->nr_buf_size = NETMAP_BUF_SIZE;
		ND("initializing slots for rxring[%d]", i);
		if (netmap_new_bufs(nifp, ring->slot, ndesc)) {
			D("Cannot allocate buffers for rx_ring[%d] for %s", i, ifname);
			goto cleanup;
		}
	}
#ifdef linux
	// XXX initialize the selrecord structs.
	for (i = 0; i < ntx; i++)
		init_waitqueue_head(&na->tx_rings[i].si);
	for (i = 0; i < nrx; i++)
		init_waitqueue_head(&na->rx_rings[i].si);
	init_waitqueue_head(&na->tx_si);
	init_waitqueue_head(&na->rx_si);
#endif
final:
	/*
	 * fill the slots for the rx and tx rings. They contain the offset
	 * between the ring and nifp, so the information is usable in
	 * userspace to reach the ring from the nifp.
	 */
	base = netmap_if_offset(nifp);
	for (i = 0; i < ntx; i++) {
		*(ssize_t *)(uintptr_t)&nifp->ring_ofs[i] =
			netmap_ring_offset(na->tx_rings[i].ring) - base;
	}
	for (i = 0; i < nrx; i++) {
		*(ssize_t *)(uintptr_t)&nifp->ring_ofs[i+ntx] =
			netmap_ring_offset(na->rx_rings[i].ring) - base;
	}

	NMA_UNLOCK();

	return (nifp);
cleanup:
	netmap_free_rings(na);
	netmap_if_free(nifp);

	NMA_UNLOCK();

	return NULL;
}

void
netmap_mem_if_delete(struct netmap_adapter *na, struct netmap_if *nifp)
{
	if (nifp == NULL)
		/* nothing to do */
		return;
	NMA_LOCK();

	if (na->refcount <= 0) {
		/* last instance, release bufs and rings */
		u_int i, j, lim;
		struct netmap_ring *ring;

		for (i = 0; i < na->num_tx_rings + 1; i++) {
			ring = na->tx_rings[i].ring;
			lim = na->tx_rings[i].nkr_num_slots;
			for (j = 0; j < lim; j++)
				netmap_free_buf(nifp, ring->slot[j].buf_idx);
		}
		for (i = 0; i < na->num_rx_rings + 1; i++) {
			ring = na->rx_rings[i].ring;
			lim = na->rx_rings[i].nkr_num_slots;
			for (j = 0; j < lim; j++)
				netmap_free_buf(nifp, ring->slot[j].buf_idx);
		}
		netmap_free_rings(na);	
	}
	netmap_if_free(nifp);

	NMA_UNLOCK();
}

/* call with NMA_LOCK held */
void
netmap_mem_deref(void)
{
	NMA_LOCK();

	nm_mem.refcount--;
	if (netmap_verbose)
		D("refcount = %d", nm_mem.refcount);

	NMA_UNLOCK();
}
