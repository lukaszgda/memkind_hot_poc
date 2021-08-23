// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2018-2019, Intel Corporation */

/*
 * critnib.c -- implementation of critnib tree
 *
 * It offers identity lookup (like a hashmap) and <= lookup (like a search
 * tree).  Unlike some hashing algorithms (cuckoo hash, perfect hashing) the
 * complexity isn't constant, but for data sizes we expect it's several
 * times as fast as cuckoo, and has no "stop the world" cases that would
 * cause latency (ie, better worst case behaviour).
 */

/*
 * STRUCTURE DESCRIPTION
 *
 * Critnib is a hybrid between a radix tree and DJ Bernstein's critbit:
 * it skips nodes for uninteresting radix nodes (ie, ones that would have
 * exactly one child), this requires adding to every node a field that
 * describes the slice (4-bit in our case) that this radix level is for.
 *
 * This implementation also stores each node's path (ie, bits that are
 * common to every key in that subtree) -- this doesn't help with lookups
 * at all (unused in == match, could be reconstructed at no cost in <=
 * after first dive) but simplifies inserts and removes.  If we ever want
 * that piece of memory it's easy to trim it down.
 */

/*
 * CONCURRENCY ISSUES
 *
 * Reads are completely lock-free sync-free, but only almost wait-free:
 * if for some reason a read thread gets pathologically stalled, it will
 * notice the data being stale and restart the work.  In usual cases,
 * the structure having been modified does _not_ cause a restart.
 *
 * Writes could be easily made lock-free as well (with only a cmpxchg
 * sync), but this leads to problems with removes.  A possible solution
 * would be doing removes by overwriting by NULL w/o freeing -- yet this
 * would lead to the structure growing without bounds.  Complex per-node
 * locks would increase concurrency but they slow down individual writes
 * enough that in practice a simple global write lock works faster.
 *
 * Removes are the only operation that can break reads.  The structure
 * can do local RCU well -- the problem being knowing when it's safe to
 * free.  Any synchronization with reads would kill their speed, thus
 * instead we have a remove count.  The grace period is DELETED_LIFE,
 * after which any read will notice staleness and restart its work.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/mman.h>
#include "memkind/internal/memkind_arena.h"
#include "memkind/internal/critnib.h"
#include "memkind/internal/bigary.h"

// pmdk-compat
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef pthread_mutex_t os_mutex_t;

#define ERR(x) do fprintf(stderr, x); while(0)

#define util_mutex_init(x)	pthread_mutex_init(x, NULL)
#define util_mutex_destroy(x)	pthread_mutex_destroy(x)
#define util_mutex_lock(x)	pthread_mutex_lock(x)
#define util_mutex_unlock(x)	pthread_mutex_unlock(x)
#define util_atomic_load_explicit32	__atomic_load
#define util_atomic_load_explicit64	__atomic_load
#define util_atomic_store_explicit32	__atomic_store_n
#define util_atomic_store_explicit64	__atomic_store_n
#define util_fetch_and_add32	__sync_fetch_and_add
#define util_fetch_and_add64	__sync_fetch_and_add
#define util_fetch_and_sub32	__sync_fetch_and_sub
#define util_fetch_and_sub64	__sync_fetch_and_sub
#define util_fetch_and_and32	__sync_fetch_and_and
#define util_fetch_and_and64	__sync_fetch_and_and
#define util_fetch_and_or32	__sync_fetch_and_or
#define util_fetch_and_or64	__sync_fetch_and_or
#define util_lssb_index64(x)	((unsigned char)__builtin_ctzll(x))
#define util_mssb_index64(x)	((unsigned char)(63 - __builtin_clzll(x)))


#define NOFUNCTION do ; while(0)

// Make these an unthing for now...
#define ASSERT(x) NOFUNCTION
#define ASSERTne(x, y) ASSERT(x != y)
#define VALGRIND_ANNOTATE_NEW_MEMORY(p, s) NOFUNCTION
#define VALGRIND_HG_DRD_DISABLE_CHECKING(p, s) NOFUNCTION
// pmdk-compat end


/*
 * A node that has been deleted is left untouched for this many delete
 * cycles.  Reads have guaranteed correctness if they took no longer than
 * DELETED_LIFE concurrent deletes, otherwise they notice something is
 * wrong and restart.  The memory of deleted nodes is never freed to
 * malloc nor their pointers lead anywhere wrong, thus a stale read will
 * (temporarily) get a wrong answer but won't crash.
 *
 * There's no need to count writes as they never interfere with reads.
 *
 * Allowing stale reads (of arbitrarily old writes or of deletes less than
 * DELETED_LIFE old) might sound counterintuitive, but it doesn't affect
 * semantics in any way: the thread could have been stalled just after
 * returning from our code.  Thus, the guarantee is: the result of get() or
 * find_le() is a value that was current at any point between the call
 * start and end.
 */
#define DELETED_LIFE 16

#define SLICE 4
#define NIB ((1ULL << SLICE) - 1)
#define SLNODES (1 << SLICE)

typedef unsigned char sh_t;
typedef uint64_t cind_t;
typedef cind_t cn_t, cl_t;
#define LEAF_BIT (1ULL<<(sizeof(cind_t)*8-1))

#define ARRAYSZ(x) (sizeof(x)/sizeof((x)[0]))

#define N(i) (c->nodes[i])
#define L(i) (c->leaves[(i) * (size_t)c->leaf_stride])

struct critnib_node {
	/*
	 * path is the part of a tree that's already traversed (be it through
	 * explicit nodes or collapsed links) -- ie, any subtree below has all
	 * those bits set to this value.
	 *
	 * nib is a 4-bit slice that's an index into the node's children.
	 *
	 * shift is the length (in bits) of the part of the key below this node.
	 *
	 *            nib
	 * |XXXXXXXXXX|?|*****|
	 *    path      ^
	 *              +-----+
	 *               shift
	 */
	cn_t child[SLNODES];
	uint64_t path;
	sh_t shift;
};

struct critnib_leaf {
	uint64_t key;
};

struct critnib {
	cn_t root;

	/* pool of freed nodes: singly linked list, next at child[0] */
	cn_t deleted_node;
	const uint64_t *leaves;
	int leaf_stride;
	/* nodes removed but not yet eligible for reuse */
	cn_t pending_del_nodes[DELETED_LIFE];

	uint64_t remove_count;

	bigary ba_critnib;
	os_mutex_t mutex; /* writes/removes */

	cn_t unall_node;
	struct critnib_node nodes[16*1048576]; // TODO: alloc, extend
};

/*
 * atomic load
 */
static void
load64(void *src, void *dst)
{
	util_atomic_load_explicit64((uint64_t *)src, (uint64_t *)dst,
		memory_order_acquire);
}

static void
load_ind(cind_t *src, cind_t *dst)
{
	__atomic_load(src, dst,	memory_order_acquire);
}

/*
 * atomic store
 */
static void
store_ind(cind_t *dst, cind_t src)
{
	__atomic_store_n(dst, src, memory_order_release);
}

/*
 * internal: is_leaf -- check tagged pointer for leafness
 */
static inline bool
is_leaf(cn_t n)
{
	return n & LEAF_BIT;
}

/*
 * internal: to_leaf -- untag a leaf pointer
 */
static cl_t
to_leaf(cn_t n)
{
	return n & ~LEAF_BIT;
}

/*
 * internal: path_mask -- return bit mask of a path above a subtree [shift]
 * bits tall
 */
static inline uint64_t
path_mask(sh_t shift)
{
	return ~NIB << shift;
}

/*
 * internal: slice_index -- return index of child at the given nib
 */
static inline unsigned
slice_index(uint64_t key, sh_t shift)
{
	return (unsigned)((key >> shift) & NIB);
}

/*
 * critnib_new -- allocates a new critnib structure
 */
struct critnib *
critnib_new(const uint64_t *leaves, int leaf_stride)
{
	bigary ba;
	bigary_init(&ba, BIGARY_DRAM, 0);
	struct critnib *c = ba.area;
	c->ba_critnib = ba;

	c->unall_node = 1;
	c->leaves = leaves;
	c->leaf_stride = leaf_stride;

	util_mutex_init(&c->mutex);

	VALGRIND_HG_DRD_DISABLE_CHECKING(&c->root, sizeof(c->root));
	VALGRIND_HG_DRD_DISABLE_CHECKING(&c->remove_count,
					sizeof(c->remove_count));

	return c;
}

/*
 * critnib_delete -- destroy and free a critnib struct
 */
void
critnib_delete(struct critnib *c)
{
	util_mutex_destroy(&c->mutex);
	bigary_free(&c->ba_critnib);
}

/*
 * internal: free_node -- free (to internal pool, not malloc) a node.
 *
 * We cannot free them to malloc as a stalled reader thread may still walk
 * through such nodes; it will notice the result being bogus but only after
 * completing the walk, thus we need to ensure any freed nodes still point
 * to within the critnib structure.
 */
static void
free_node(struct critnib *__restrict c, cn_t n)
{
	if (!n)
		return;

	ASSERT(!is_leaf(n));
	N(n).child[0] = c->deleted_node;
	c->deleted_node = n;
}

/*
 * internal: alloc_node -- allocate a node from our pool or from malloc
 */
static cn_t
alloc_node(struct critnib *__restrict c)
{
	cn_t n = c->deleted_node;

	if (!n) {
		cn_t n = c->unall_node++;
		if (n >= ARRAYSZ(c->nodes))
			return 0;
		return n;
	}

	c->deleted_node = N(n).child[0];

	return n;
}

/*
 * crinib_insert -- write a key:value pair to the critnib structure
 *
 * Returns:
 *  • 0 on success
 *  • EEXIST if such a key already exists
 *  • ENOMEM if we're out of memory
 *
 * Takes a global write lock but doesn't stall any readers.
 */
int
critnib_insert(struct critnib *c, int leaf)
{
	util_mutex_lock(&c->mutex);

	uint64_t key = L(leaf);

	cn_t kn = leaf | LEAF_BIT;

	cn_t n = c->root;
	if (!n) {
		c->root = kn;

		util_mutex_unlock(&c->mutex);

		return 0;
	}

	cn_t *parent = &c->root;
	cn_t prev = c->root;

	while (n && !is_leaf(n) && (key & path_mask(N(n).shift)) == N(n).path) {
		prev = n;
		parent = &N(n).child[slice_index(key, N(n).shift)];
		n = *parent;
	}

	if (!n) {
		n = prev;
		store_ind(&N(n).child[slice_index(key, N(n).shift)], kn);

		util_mutex_unlock(&c->mutex);

		return 0;
	}

	uint64_t path = is_leaf(n) ? L(to_leaf(n)) : N(n).path;
	/* Find where the path differs from our key. */
	uint64_t at = path ^ key;
	if (!at) {
		util_mutex_unlock(&c->mutex);
		return EEXIST;
	}

	/* and convert that to an index. */
	sh_t sh = util_mssb_index64(at) & (sh_t)~(SLICE - 1);

	cn_t m = alloc_node(c);
	if (!m) {
		util_mutex_unlock(&c->mutex);

		return ENOMEM;
	}
	//VALGRIND_HG_DRD_DISABLE_CHECKING(m, sizeof(struct critnib_node));

	for (int i = 0; i < SLNODES; i++)
		N(m).child[i] = 0;

	N(m).child[slice_index(key, sh)] = kn;
	N(m).child[slice_index(path, sh)] = n;
	N(m).shift = sh;
	N(m).path = key & path_mask(sh);
	store_ind(parent, m);

	util_mutex_unlock(&c->mutex);

	return 0;
}

/*
 * critnib_remove -- delete a key from the critnib structure, return its index
 */
int
critnib_remove(struct critnib *c, uint64_t key)
{
	cl_t k;

	util_mutex_lock(&c->mutex);

	cn_t n = c->root;
	if (!n)
		goto not_found;

	uint64_t del = util_fetch_and_add64(&c->remove_count, 1) % DELETED_LIFE;
	free_node(c, c->pending_del_nodes[del]);
	c->pending_del_nodes[del] = 0;

	if (is_leaf(n)) {
		k = to_leaf(n);
		if (L(k) == key) {
			store_ind(&c->root, 0);
			goto del_leaf;
		}

		goto not_found;
	}
	/*
	 * n and k are a parent:child pair (after the first iteration); k is the
	 * leaf that holds the key we're deleting.
	 */
	cn_t *k_parent = &c->root;
	cn_t *n_parent = &c->root;
	cn_t kn = n;

	while (!is_leaf(kn)) {
		n_parent = k_parent;
		n = kn;
		k_parent = &N(kn).child[slice_index(key, N(kn).shift)];
		kn = *k_parent;

		if (!kn)
			goto not_found;
	}

	k = to_leaf(kn);
	if (L(k) != key)
		goto not_found;

	store_ind(&N(n).child[slice_index(key, N(n).shift)], 0);

	/* Remove the node if there's only one remaining child. */
	int ochild = -1;
	for (int i = 0; i < SLNODES; i++) {
		if (N(n).child[i]) {
			if (ochild != -1)
				goto del_leaf;

			ochild = i;
		}
	}

	ASSERTne(ochild, -1);

	store_ind(n_parent, N(n).child[ochild]);
	c->pending_del_nodes[del] = n;

del_leaf:
	util_mutex_unlock(&c->mutex);
	return k;

not_found:
	util_mutex_unlock(&c->mutex);
	return -1;
}

/*
 * critnib_get -- query for a key ("==" match), returns value or NULL
 *
 * Doesn't need a lock but if many deletes happened while our thread was
 * somehow stalled the query is restarted (as freed nodes remain unused only
 * for a grace period).
 *
 * Counterintuitively, it's pointless to return the most current answer,
 * we need only one that was valid at any point after the call started.
 */
int
critnib_get(struct critnib *__restrict c, uint64_t key)
{
	uint64_t wrs1, wrs2;
	int res;

	do {
		cn_t n;

		load64(&c->remove_count, &wrs1);
		load_ind(&c->root, &n);

		/*
		 * critbit algorithm: dive into the tree, looking at nothing but
		 * each node's critical bit^H^H^Hnibble.  This means we risk
		 * going wrong way if our path is missing, but that's ok...
		 */
		while (n && !is_leaf(n))
		{
			struct critnib_node *node = &N(n);
			load_ind(&node->child[slice_index(key, node->shift)], &n);
		}

		/* ... as we check it at the end. */
		cl_t k = to_leaf(n);
		res = (n && L(k) == key) ? k : -1;
		load64(&c->remove_count, &wrs2);
	} while (wrs1 + DELETED_LIFE <= wrs2);

	return res;
}

/*
 * internal: find_successor -- return the rightmost value in a subtree
 */
static int
find_successor(struct critnib *__restrict c, cn_t n)
{
	while (1) {
		int nib;
		for (nib = NIB; nib >= 0; nib--)
			if (N(n).child[nib])
				break;

		if (nib < 0)
			return -1;

		n = N(n).child[nib];
		if (is_leaf(n))
			return to_leaf(n);
	}
}

/*
 * internal: find_le -- recursively search <= in a subtree
 */
static int
find_le(struct critnib *__restrict c, cn_t n, uint64_t key)
{
	if (!n)
		return -1;

	if (is_leaf(n)) {
		cl_t k = to_leaf(n);

		return (L(k) <= key) ? k : -1;
	}

	/*
	 * is our key outside the subtree we're in?
	 *
	 * If we're inside, all bits above the nib will be identical; note
	 * that shift points at the nib's lower rather than upper edge, so it
	 * needs to be masked away as well.
	 */
	if ((key ^ N(n).path) >> (N(n).shift) & ~NIB) {
		/*
		 * subtree is too far to the left?
		 * -> its rightmost value is good
		 */
		if (N(n).path < key)
			return find_successor(c, n);

		/*
		 * subtree is too far to the right?
		 * -> it has nothing of interest to us
		 */
		return -1;
	}

	unsigned nib = slice_index(key, N(n).shift);
	/* recursive call: follow the path */
	{
		cn_t m;
		load_ind(&N(n).child[nib], &m);
		int k = find_le(c, m, key);
		if (k == -1)
			return k;
	}

	/*
	 * nothing in that subtree?  We strayed from the path at this point,
	 * thus need to search every subtree to our left in this node.  No
	 * need to dive into any but the first non-null, though.
	 */
	for (; nib > 0; nib--) {
		cn_t m;
		load_ind(&N(n).child[nib - 1], &m);
		if (m) {
			n = m;
			if (is_leaf(n))
				return to_leaf(n);

			return find_successor(c, n);
		}
	}

	return -1;
}

/*
 * critnib_find_le -- query for a key ("<=" match), returns value or NULL
 *
 * Same guarantees as critnib_get().
 */
int
critnib_find_le(struct critnib *c, uint64_t key)
{
	uint64_t wrs1, wrs2;
	int res;

	do {
		load64(&c->remove_count, &wrs1);
		cn_t n; /* avoid a subtle TOCTOU */
		load_ind(&c->root, &n);
		res = n ? find_le(c, n, key) : -1;
		load64(&c->remove_count, &wrs2);
	} while (wrs1 + DELETED_LIFE <= wrs2);

	return res;
}

static int
iter(struct critnib *c, cn_t n, int (*func)(int k))
{
	if (is_leaf(n))
		return func(to_leaf(n));

	for (int i=0; i < SLNODES; i++) {
		cn_t m = N(n).child[i];
		if (m)
			if (iter(c, m, func))
				return 1;
	}
	return 0;
}

/*
 * critnib_iter -- visit the whole struct, calling func() for every leaf
 *
 * If func() returns 1, the walk is aborted.
 */
void
critnib_iter(struct critnib *c, int (*func)(int leaf))
{
	util_mutex_lock(&c->mutex);
	if (c->root)
		iter(c, c->root, func);
	util_mutex_unlock(&c->mutex);
}
