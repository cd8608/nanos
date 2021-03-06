#include <kernel.h>
#include <page.h>
#include <pagecache.h>

//#define PAGECACHE_DEBUG
#if defined(PAGECACHE_DEBUG)
#define pagecache_debug(x, ...) do {rprintf("PGC: " x, ##__VA_ARGS__);} while(0)
#else
#define pagecache_debug(x, ...)
#endif

static inline u64 pagecache_pagesize(pagecache pc)
{
    return U64_FROM_BIT(pc->page_order);
}

static int page_state(pagecache_page pp)
{
    return pp->state_phys >> PAGECACHE_PAGESTATE_SHIFT;
}

static inline void set_page_state_cache_locked(pagecache pc, pagecache_page pp, int state)
{
    int old_state = page_state(pp);
    switch (state) {
    case PAGECACHE_PAGESTATE_FREE:
        assert(old_state == PAGECACHE_PAGESTATE_NEW || old_state == PAGECACHE_PAGESTATE_ACTIVE);
        list_delete(&pp->l);
        list_insert_before(&pc->free, &pp->l);
        break;
    case PAGECACHE_PAGESTATE_ALLOC:
        assert(old_state == PAGECACHE_PAGESTATE_FREE);
        break;
    case PAGECACHE_PAGESTATE_READING:
        assert(old_state == PAGECACHE_PAGESTATE_ALLOC);
        break;
    case PAGECACHE_PAGESTATE_NEW:
        /* later we can allow full page writes to move to new list after sync */
        assert(old_state == PAGECACHE_PAGESTATE_READING);
        list_insert_before(&pc->new, &pp->l);
        break;
    case PAGECACHE_PAGESTATE_ACTIVE:
        assert(old_state == PAGECACHE_PAGESTATE_NEW);
        list_delete(&pp->l);
        list_insert_before(&pc->active, &pp->l);
        break;
    default:
        halt("%s: bad state %d, old %d\n", __func__, state, old_state);
    }

    pp->state_phys = (pp->state_phys & MASK(PAGECACHE_PAGESTATE_SHIFT)) |
        ((u64)state << PAGECACHE_PAGESTATE_SHIFT);
}

/* Usually this completion is called without the cache lock held - exceptions noted below. */
closure_function(2, 1, void, read_page_complete,
                 pagecache, pc, pagecache_page, pp,
                 status, s)
{
    pagecache pc = bound(pc);
    pagecache_page pp = bound(pp);
    pagecache_debug("%s: pc %p, pp %p, status %v\n", __func__, pc, bound(pp), s);
    spin_lock(&pp->lock);
    assert(page_state(pp) == PAGECACHE_PAGESTATE_READING);
    if (!is_ok(s)) {
        /* TODO need policy for capturing/reporting I/O errors... */
        msg_err("error reading page %R: %v\n", pp->node.r, s);
    } else {
        /* Sadly, the cache may already be locked here (covering block
           read issue) as some block devices (e.g. ATA) issue
           completions immediately, without blocking. */
        boolean unlocked = spin_try(&pc->lock);
        set_page_state_cache_locked(bound(pc), pp, PAGECACHE_PAGESTATE_NEW);
        if (unlocked)
            spin_unlock(&pc->lock);
    }

    /* TODO We technically shouldn't be releasing the page lock here
       if we are walking the completions vector, but for the time
       being we are safe due to the big kernel lock. Maybe clone the
       vector or replace with a new one? */
    spin_unlock(&pp->lock);
    status_handler sh;
    vector_foreach(pp->completions, sh) {
        apply(sh, s);
    }
    vector_clear(pp->completions);
    closure_finish();
}

static boolean pagecache_page_touch_if_filled_cache_locked(pagecache pc, pagecache_page pp)
{
    int state = page_state(pp);
    if (state == PAGECACHE_PAGESTATE_READING ||
        state == PAGECACHE_PAGESTATE_ALLOC) {
        return false;
    }

    /* move to bottom of active list */
    if (state == PAGECACHE_PAGESTATE_ACTIVE) {
        list_delete(&pp->l);
        list_insert_before(&pc->active, &pp->l);
    } else if (state == PAGECACHE_PAGESTATE_NEW) {
        /* cache hit -> active */
        set_page_state_cache_locked(pc, pp, PAGECACHE_PAGESTATE_ACTIVE);
    } else {
        assert(state == PAGECACHE_PAGESTATE_DIRTY);
    }
    return true;
}

static void pagecache_page_fill_cache_locked(pagecache pc, pagecache_page pp, status_handler sh)
{
    vector_push(pp->completions, sh);
    if (page_state(pp) == PAGECACHE_PAGESTATE_ALLOC) {
        set_page_state_cache_locked(pc, pp, PAGECACHE_PAGESTATE_READING);

        /* zero pad anything extending past end of backing storage */
        u64 end = pp->node.r.end;
        if (end > pc->length) {
            zero(pp->kvirt + (pc->length - pp->node.r.start), end - pc->length);
            end = pc->length;
        }

        /* issue block reads */
        range blocks = range_rshift(irange(pp->node.r.start, end), pc->block_order);
        pagecache_debug("%s: pc %p, pp %p, blocks %R, reading...\n", __func__, pc, pp, blocks);
        apply(pc->block_read, pp->kvirt, blocks, closure(pc->h, read_page_complete, pc, pp));
    }
}

static void pagecache_read_page_internal_cache_locked(pagecache pc, pagecache_page pp,
                                                      sg_list sg, range q, merge m)
{
    range r = pp->node.r;
    pagecache_debug("%s: pc %p, sg %p, q %R, m %p, r %R, pp %p, refcount %d, state %d\n",
                    __func__, pc, sg, q, m, r, pp, pp->refcount.c, page_state(pp));

    range i = range_intersection(q, r);
    bytes length = range_span(i);
    bytes offset = i.start - r.start;
    sg_buf sgb = sg_list_tail_add(sg, length);

    sgb->buf = pp->kvirt + offset;
    sgb->length = length;
    sgb->refcount = &pp->refcount;
    refcount_reserve(&pp->refcount); /* reference for being on sg list */

    if (!pagecache_page_touch_if_filled_cache_locked(pc, pp)) {
        pagecache_page_fill_cache_locked(pc, pp, apply_merge(m));
    }
}

/* for existing pages, load blocks as necessary and move from new to active list
   note: sg vec building depends on rangemap traversal being in order... */
closure_function(4, 1, void, pagecache_read_page_cache_locked,
                 pagecache, pc, sg_list, sg, range, q, merge, m,
                 rmnode, node)
{
    pagecache_page pp = (pagecache_page)node;
    pagecache_read_page_internal_cache_locked(bound(pc), pp, bound(sg), bound(q), bound(m));
}

closure_function(2, 0, void, pagecache_page_release,
                 pagecache, pc, pagecache_page, pp)
{
    pagecache_page pp = bound(pp);
    /* remove from existing list depending on state */
    int state = page_state(pp);
    if (state != PAGECACHE_PAGESTATE_NEW || state != PAGECACHE_PAGESTATE_ACTIVE)
        halt("%s: pc %p, pp %p, invalid state %d\n", __func__, bound(pc), pp, page_state(pp));

    pagecache pc = bound(pc);
    spin_lock(&pc->lock);
    rangemap_remove_node(pc->pages, &pp->node);
    set_page_state_cache_locked(pc, pp, PAGECACHE_PAGESTATE_FREE);
    zero(pp->kvirt, pagecache_pagesize(pc));
    spin_unlock(&pc->lock);
    /* leave closure intact and reuse */
}

static pagecache_page allocate_pagecache_page_cache_locked(pagecache pc, range r)
{
    pagecache_page pp;
    if (!list_empty(&pc->free)) {
        list l = list_get_next(&pc->free);
        assert(l);
        pp = struct_from_list(l, pagecache_page, l);
        list_delete(l);
        set_page_state_cache_locked(pc, pp, PAGECACHE_PAGESTATE_ALLOC);
        refcount_reserve(&pp->refcount);
        goto pp_insert;
    }

    /* allocate - later we can look at blocks of pages at a time */
    u64 pagesize = U64_FROM_BIT(pc->page_order);
    void *p = allocate(pc->backed, pagesize);
    if (p == INVALID_ADDRESS)
        return INVALID_ADDRESS;

    pp = allocate(pc->h, sizeof(struct pagecache_page));
    if (pp == INVALID_ADDRESS)
        goto fail_dealloc_backed;

    spin_lock_init(&pp->lock);
    pp->completions = allocate_vector(pc->h, 8);
    if (pp->completions == INVALID_ADDRESS)
        goto fail_dealloc_pp;
    pp->l.next = pp->l.prev = 0;

    /* keeping physical for demand paging / multiple mappings */
    pp->state_phys = ((u64)PAGECACHE_PAGESTATE_ALLOC << PAGECACHE_PAGESTATE_SHIFT) |
        (physical_from_virtual(p) >> pc->page_order);
    pp->kvirt = p;
    init_refcount(&pp->refcount, 1, closure(pc->h, pagecache_page_release, pc, pp));
  pp_insert:
    pp->node.r = r;
    assert(rangemap_insert(pc->pages, &pp->node));
    return pp;
  fail_dealloc_pp:
    deallocate(pc->h, pp, sizeof(struct pagecache_page));
  fail_dealloc_backed:
    deallocate(pc->backed, p, pagesize);
    return INVALID_ADDRESS;
}

/* populate missing pages, allocate buffers and install kernel mappings */
closure_function(4, 1, void, pagecache_read_gap_cache_locked,
                 pagecache, pc, sg_list, sg, range, q, merge, m,
                 range, r)
{
    pagecache_debug("%s:    q %R, r %R\n", __func__, bound(q), r);
    pagecache pc = bound(pc);
    int order = pc->page_order;
    u64 pagesize = U64_FROM_BIT(order);
    u64 start = r.start & ~MASK(order);
    for (u64 offset = start; offset < r.end; offset += pagesize) {
        pagecache_page pp = allocate_pagecache_page_cache_locked(pc, irange(offset, offset + pagesize));
        if (pp == INVALID_ADDRESS) {
            apply(apply_merge(bound(m)), timm("result", "failed to allocate pagecache_page"));
            return;
        }
        pagecache_read_page_internal_cache_locked(pc, pp, bound(sg), bound(q), bound(m));
    }
}

/* TODO rangemap -> single point tree lookup */
static boolean pagecache_read_internal(pagecache pc, sg_list sg, range q, status_handler completion)
{
    pagecache_debug("%s: pc %p, sg %p, q %R, completion %p\n", __func__, pc, sg, q, completion);
    assert(range_span(q) > 0);
    merge m = allocate_merge(pc->h, completion);
    status_handler sh = apply_merge(m);

    /* fill gaps and initiate reads */
    spin_lock(&pc->lock);
    rmnode_handler nh = stack_closure(pagecache_read_page_cache_locked, pc, sg, q, m);
    range_handler rh = stack_closure(pagecache_read_gap_cache_locked, pc, sg, q, m);
    boolean match = rangemap_range_lookup_with_gaps(pc->pages, q, nh, rh);
    spin_unlock(&pc->lock);

    if (!match) {
        apply(sh, timm("result", "%s: no matching pages for range %R", __func__, q));
        return false;
    }
    /* finished issuing requests */
    apply(sh, STATUS_OK);
    return true;
}

closure_function(1, 3, void, pagecache_read_sg,
                 pagecache, pc,
                 sg_list, sg, range, q, status_handler, sh)
{
    pagecache_read_internal(bound(pc), sg, q, sh);
}

/* TODO for pagecache writing:

   immediate:
   * buffers being synced to storage can still be modified - re set to dirty
   - get rid of annoying write test output
   - don't wait for block write to apply write completion, but do track (and report) any write errors

   future:
   - use the block mapper to convert between byte offset and block numbers
     - this paves the way for per-fsfile cache, bypassing tfs extent lookup
   - implement write-back
*/

/* cache lock may or may not be held here */
static void pagecache_write_page_internal_page_locked(pagecache pc, pagecache_page pp,
                                                      void *buf, range q, status_handler sh)
{
    int state = page_state(pp);
    range i = range_intersection(q, pp->node.r);
    u64 len = range_span(i);
    u64 page_offset = i.start - pp->node.r.start;
    void *dest = pp->kvirt + page_offset;
    void *src = buf + (i.start - q.start);
    pagecache_debug("%s: pc %p, pp %p, refcount %d, state %d, src %p, i %R, offset %d, len %d\n",
                    __func__, pc, pp, pp->refcount.c, state, src, i, page_offset, len);

    assert(state == PAGECACHE_PAGESTATE_ALLOC || state == PAGECACHE_PAGESTATE_NEW ||
           state == PAGECACHE_PAGESTATE_ACTIVE || state == PAGECACHE_PAGESTATE_DIRTY);

    pagecache_debug("   copy %p <- %p %d bytes\n", dest, src, len);
    assert(pp->node.r.start + len <= pc->length);
    runtime_memcpy(dest, src, len);
    range blocks = range_rshift(i, pc->block_order);
    pagecache_debug("   write %p to block range %R\n", dest, blocks);
    apply(pc->block_write, dest, blocks, sh);
}

/* cache lock may or may not be held here */
closure_function(5, 0, void, pagecache_write_io_complete,
                 pagecache, pc, pagecache_page, pp, void *, buf, range, q, status_handler, sh)
{
    spin_lock(&bound(pp)->lock);
    pagecache_write_page_internal_page_locked(bound(pc), bound(pp), bound(buf), bound(q), bound(sh));
    spin_unlock(&bound(pp)->lock);
    closure_finish();
}

/* cache lock may or may not be held here */
static void pagecache_write_page_io_check(pagecache pc, pagecache_page pp,
                                          void *buf, range q, status_handler sh)
{
    spin_lock(&pp->lock);
    int state = page_state(pp);
    assert(state != PAGECACHE_PAGESTATE_ALLOC);
    if (state == PAGECACHE_PAGESTATE_READING) {
        vector_push(pp->completions, closure(pc->h, pagecache_write_io_complete,
                                             pc, pp, buf, q, sh));
    } else {
        pagecache_write_page_internal_page_locked(pc, pp, buf, q, sh);
    }
    spin_unlock(&pp->lock);
}

closure_function(4, 1, void, pagecache_write_page_cache_locked,
                 pagecache, pc, void *, buf, range, q, merge, m,
                 rmnode, node)
{
    pagecache_write_page_io_check(bound(pc), (pagecache_page)node, bound(buf), bound(q),
                                  apply_merge(bound(m)));
}

closure_function(5, 1, void, pagecache_write_page_filled,
                 pagecache, pc, pagecache_page, pp, void *, buf, range, q, status_handler, sh,
                 status, s)
{
    pagecache_debug("%s: page %R, status %v\n", __func__, bound(pp)->node.r, s);
    if (!is_ok(s)) {
        apply(bound(sh), timm_up(s, "result", "%s: fill failed", __func__));
    } else {
        pagecache_write_page_io_check(bound(pc), bound(pp), bound(buf), bound(q), bound(sh));
    }
    closure_finish();
}

closure_function(4, 1, void, pagecache_write_gap_cache_locked,
                 pagecache, pc, void *, buf, range, q, merge, m,
                 range, r)
{
    pagecache pc = bound(pc);
    pagecache_debug("%s: buf %p, q %R, r %R\n", __func__, bound(buf), bound(q), r);
    int order = pc->page_order;
    u64 pagesize = U64_FROM_BIT(order);
    u64 start = r.start & ~MASK(order);
    for (u64 offset = start; offset < r.end; offset += pagesize) {
        pagecache_page pp = allocate_pagecache_page_cache_locked(pc, irange(offset, offset + pagesize));
        if (pp == INVALID_ADDRESS) {
            apply(apply_merge(bound(m)), timm("result", "failed to allocate pagecache_page"));
            return;
        }

        /* if this write covers the entire page, don't bother trying to fill it first */
        range i = range_intersection(pp->node.r, bound(q));
        if (i.start == pp->node.r.start && i.end == MIN(pp->node.r.end, pc->length)) {
            spin_lock(&pp->lock);
            pagecache_write_page_internal_page_locked(pc, pp, bound(buf), bound(q), apply_merge(bound(m)));
            spin_unlock(&pp->lock);
        } else {
            pagecache_page_fill_cache_locked(pc, pp, closure(pc->h, pagecache_write_page_filled,
                                                             pc, pp, bound(buf), bound(q),
                                                             apply_merge(bound(m))));
        }
    }
}

closure_function(1, 3, void, pagecache_write,
                 pagecache, pc,
                 void *, buf, range, blocks, status_handler, completion)
{
    pagecache pc = bound(pc);
    pagecache_debug("%s: buf %p, sg %p, blocks %R, completion %p\n", __func__, pc, buf, blocks, completion);
    range q = range_lshift(blocks, pc->block_order);
    merge m = allocate_merge(pc->h, completion);
    status_handler sh = apply_merge(m);

    /* fill gaps and initiate writes (and prerequisite reads) */
    spin_lock(&pc->lock);
    rmnode_handler nh = stack_closure(pagecache_write_page_cache_locked, pc, buf, q, m);
    range_handler rh = stack_closure(pagecache_write_gap_cache_locked, pc, buf, q, m);
    boolean match = rangemap_range_lookup_with_gaps(pc->pages, q, nh, rh);
    spin_unlock(&pc->lock);

    if (!match) {
        apply(sh, timm("result", "%s: no matching pages for range %R", __func__, q));
        return;
    }
    apply(sh, STATUS_OK);
}

pagecache allocate_pagecache(heap general, heap backed,
                             u64 length, u64 pagesize, u64 block_size,
                             block_mapper mapper, block_io read, block_io write)
{
    pagecache pc = allocate(general, sizeof(struct pagecache));
    if (pc == INVALID_ADDRESS)
        return pc;

    pc->pages = allocate_rangemap(general);
    if (pc->pages == INVALID_ADDRESS) {
        deallocate(general, pc->pages, sizeof(struct pagecache));
        return INVALID_ADDRESS;
    }
    list_init(&pc->free);
    list_init(&pc->new);
    list_init(&pc->active);
    list_init(&pc->dirty);
    pc->page_order = find_order(pagesize);
    assert(pagesize == U64_FROM_BIT(pc->page_order));
    pc->block_order = find_order(block_size);
    assert(block_size == U64_FROM_BIT(pc->block_order));
    pc->length = length;
    pc->h = general;
    pc->backed = backed;
    pc->mapper = mapper;
    pc->block_read = read;
    pc->block_write = write;
    pc->sg_read = closure(general, pagecache_read_sg, pc);
    pc->write = closure(general, pagecache_write, pc);
    return pc;
}
