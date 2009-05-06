/*
    Copyright (C) 2005-2008  Ricky Zheng <ricky_gz_zheng@yahoo.co.nz>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/
/**
 * \file uffs_mem.c
 * \brief uffs native memory allocator
 * \author Ricky Zheng, created 23th Feb, 2007
 */

#include <string.h>

#include "uffs/uffs_types.h"
#include "uffs/uffs_public.h"
#include "uffs/uffs_os.h"
#include "uffs/uffs_mem.h"

#if defined(USE_NATIVE_MEMORY_ALLOCATOR)

#define PFX "mem: "


#define HEAP_MAGIC_SIZE	8		/* heap magic size, this block is for memory protection */




/* the 'BEST FIT' arithmetic,
 if not defined, the arithmetic
 will be the 'FIRST FIT' */
#define K_HEAP_ALLOCK_BEST_FIT


/* page size may be: 16,32,64,128... */
#define ALLOC_PAGE_BIT_OFFSET	5
#define ALLOC_PAGE_SIZE			(1 << ALLOC_PAGE_BIT_OFFSET)
#define ALLOC_PAGE_MASK			(ALLOC_PAGE_SIZE - 1)
#define ALLOC_THRESHOLD			(ALLOC_PAGE_SIZE * 1)

/* magic mummbers */
#define HEAP_NODE_FREE			0x123455aa
#define HEAP_NODE_ALLOCED		0xaa551234

#define ALLOC_OFFSET	12

/*  Heap memory node type. */
typedef struct _HEAPNODE {
	int	mark;					/*	alloc mark	*/
    int	size;					/*	Size of this node	*/
	struct _HEAPNODE *prevNode;	/*	private node	*/
    struct _HEAPNODE *prevFree;	/*  Link to prev free node */
    struct _HEAPNODE *nextFree;	/*	Link to next free node */
} HEAPNODE;



/*
		p1	|-----------|
			|prevNode	|	NULL
			|mark		|	HEAP_NODE_ALLOCED
			|size		|	p2 - p1
			|prevFree	|	alloc to user
			|nextFree	|	not used.
			|			|
			|			|
		p2	|-----------|
			|prevNode	|	p1
			|mark		|	HEAP_NODE_FREE
			|size		|	p3 - p2
			|prevFree	|	NULL
			|nextFree	|	p5
			|			|
			|			|
		p3	|-----------|
			|prevNode	|	p2
			|mark		|	HEAP_NODE_ALLOCED
			|size		|	p4 - p3
			|prevFree	|	alloc to user
			|nextFree	|	not used.
			|			|
			|			|
		p4	|-----------|
			|prevNode	|	p3
			|mark		|	HEAP_NODE_ALLOCED
			|size		|	p5 - p4
			|prevFree	|	alloc to user
			|nextFree	|	not used.
			|			|
			|			|
		p5	|-----------|
			|prevNode	|	p4
			|mark		|	HEAP_NODE_FREE
			|size		|	p6 - p5
			|prevFree	|	p2
			|nextFree	|	NULL
			|			|
			|			|
		p6	|-----------|

*/

static HEAPNODE* volatile _k_heapFreeList = NULL;
static HEAPNODE * _k_heapTail_ = NULL; 
static u32 _k_heap_available = 0;
static u32 _minimu_heap_avaiable = 0x0fffffff;
static u32 _kernel_heap_total = 0;

static void HeapDeleteFromFreeList(HEAPNODE *node);
static void HeapChainToFreeList(HEAPNODE *node);
static void *_k_allock_node(HEAPNODE *node, int size);
//static void * _kmalloc_clear(int size);
static int _kfree(void *block);

/*
 *	Delete one node from free list
 *
 */
static void HeapDeleteFromFreeList(HEAPNODE *node)
{
	if(node->nextFree)
		node->nextFree->prevFree = node->prevFree;
	if(node->prevFree)
		node->prevFree->nextFree = node->nextFree;
	if(node == _k_heapFreeList)
		_k_heapFreeList = node->nextFree;
}

/*
 *	Chain the node to free list
 */
static void HeapChainToFreeList(HEAPNODE *node)
{
	node->nextFree = NULL;
	node->prevFree = NULL;
	if(_k_heapFreeList == NULL){
		_k_heapFreeList = node;
		return;
	}
	else{
		_k_heapFreeList->prevFree = node;
		node->nextFree = _k_heapFreeList;
		_k_heapFreeList = node;
	}
}

/*
 * Alloc a block with given node
 * If the node  is larger than the
 * required space plus the space needed for
 * a new node plus a defined threshold, then
 * we split it. The unused portion is put back into
 * the free-list.
 *
 * Attention: Irq is locked when call this routin,
 * so we must unlock irq when return
 */
static void *_k_allock_node(HEAPNODE *node, int size)
{
	HEAPNODE *newNode;

	if(node->size >= size + ALLOC_THRESHOLD){
		/*
		 * we need to split it 
		 */
		newNode = (HEAPNODE *)((u32)node + size);
		newNode->size = node->size - size;
		newNode->mark = HEAP_NODE_FREE;
		newNode->prevNode = node;
		node->size = size;
		/*
		 *	chain the newNode to free list
		 */
		HeapChainToFreeList(newNode);
				
		/*
		 *	fix the next node
		 */
		 ((HEAPNODE *)((u32)newNode + newNode->size))->prevNode = newNode;
	}
		
	/*
	 *	allock this block
	 */
	node->mark = HEAP_NODE_ALLOCED;

	/*
	 *	delete the node from free list
	 */
	HeapDeleteFromFreeList(node);

	_k_heap_available -= node->size;
	if(_minimu_heap_avaiable > _k_heap_available)
		_minimu_heap_avaiable = _k_heap_available;
	
	uffs_CriticalExit();	/* exit critical */
	
	return (void *)((u32)node + ALLOC_OFFSET);
}

/*
 * Allocate a block from heap memory.
 *
 * This functions allocates a memory block of the specified
 * size and returns a pointer to that block.
 *
 * The actual size of the allocated block is larger than the
 * requested size because of space required for maintenance
 * information. This additional information is invisible to
 * the application.
 *
 * The routine looks for the smallest block that will meet
 * the required size and releases it to the caller. If the
 * block being requested is usefully smaller than the smallest
 * free block then the block from which the request is being
 * met is split in two. The unused portion is put back into
 * the free-list.
 *
 * The contents of the allocated block is unspecified.
 * To allocate a block with all bytes set to zero use
 * KHeapAllocClear().
 *
 * \note Interrupts are automatically enabled, when this
 *       function returns.
 *
 * \param size Size of the requested memory block.
 *
 * \return Pointer to the allocated memory block if the
 *         function is successful or NULL if the requested
 *         amount of memory is not _k_heap_available.
 */
static void *_kmalloc(int size)
{
	HEAPNODE *node;
#if defined(K_HEAP_ALLOCK_BEST_FIT)
	HEAPNODE *fit;
#endif
	if(size <= 0)
		return NULL;	/* size is not fit */
		
	/*
	 *	adjust size
	 */
	size += ALLOC_OFFSET;
	if(size & ALLOC_PAGE_MASK){
		size += ALLOC_PAGE_SIZE;
		size &= ~ALLOC_PAGE_MASK;
	}

	uffs_CriticalEnter();	/* enter critical */
	
	node = _k_heapFreeList;
	
#if defined(K_HEAP_ALLOCK_BEST_FIT)
    /*
     * Walk through the linked list of free nodes and find the best fit.
     */
	fit = NULL;
	while(node){
        /*
         * Found a note that fits?
         */
		if(node->size >= size){
            /*
             * If it's an exact match, we don't
             * search any further.
             */
			if(node->size == size){
				fit = node;
				break;
			}
			/*
			 *	We search most fit one
			 */
			if(fit){
				if(node->size < fit->size)
					fit = node;
			}
			else
				fit = node;
		}
		node = node->nextFree;
	}
	
	if(fit){
		if(fit->size >= size)
			return _k_allock_node(fit, size);
	}
#else
	while(node){
		if(node->size >= size)
			return _k_allock_node(node, size);
		node = node->nextFree;
	}
#endif

	uffs_CriticalExit();	/* exit critical */
	
	return NULL;	/*	not found available block	*/

}

#if 0
/* Allocates an array in memory with elements initialized to 0 */
static void *_kcalloc(int num, int size)
{
	return _kmalloc_clear(num * size);
}
#endif

/* Realloc memory.
 * if the size of memblock is small then the new required size, 
 * alloc a new block memory, and copy the contents from the old one,
 * and free the old block.
 * if the size is zero, free the old block, and return NULL. <2004.5.8>
 * if the size of origin block is larger then the new required size,
 * then: 
 *   if the gap is larger then ALLOC_PAGE_SIZE, split the node, and return
 *		the leav memory back to free list.
 *   if the gap is less then ALLOC_PAGE_SIZE, just return current block.
 * If the given block parameter is NULL, _krealloc behaves the same as _kmalloc.
 */
static void *_krealloc(void *block, int size)
{
	HEAPNODE *node;
	HEAPNODE *newNode;
	void *p;	/* return pointer */
	int old_data_size; /* old block data size */

	if(block == NULL){
		return _kmalloc(size);
	}
	
	if(size == 0) {
		_kfree(block);
		return NULL;
	}

	uffs_CriticalEnter();	/* enter critical */
	
	node = (HEAPNODE *)((u32)block - ALLOC_OFFSET);
	old_data_size = node->size - ALLOC_OFFSET;
	if(node->mark != (int)HEAP_NODE_ALLOCED || old_data_size <= 0) {
		uffs_CriticalExit(); /* exit critical */
		return NULL;	/*!!!! at this moment, the heap 
						managment info must be damaged !!!!!*/
	}

	if(old_data_size < size) {
		/* new size is larger then origin block, so need alloc new block */
		p = _kmalloc(size);
		if(!p) {
			uffs_CriticalExit(); /* exit critical */
			return NULL;		/* can't alloc a new block memory, fail... */
		}

		/* alloc a new block, and copy contents from origin block,
		 * and free it finally.
		 */
		memcpy(p, block, old_data_size);
		_kfree(block);
		uffs_CriticalExit(); /* exit critical */
		return p;
	}
	else {
		/* adjust size */
		size += ALLOC_OFFSET;
		if(size & ALLOC_PAGE_MASK) {
			size += ALLOC_PAGE_SIZE;
			size &= ~ALLOC_PAGE_MASK;
		}

		if(node->size - size < ALLOC_PAGE_SIZE) {
			/* the remain memory is too small, so just skip it */
			uffs_CriticalExit(); /* exit critical */
			return block;
		}
		else {
			/* the remain memory is large enough to be splited */
			/* we generate a new 'alloced' node there */
			newNode = (HEAPNODE *)((u32)node + size);
			newNode->prevNode = node;
			newNode->mark = HEAP_NODE_ALLOCED;
			newNode->size = node->size - size;

			/* split into two node now */
			((HEAPNODE *)((u32)node + node->size))->prevNode = newNode;
			node->size = size;

			/* put the newNode into freeList */
			_kfree((void *)((u32)newNode + ALLOC_OFFSET)); 

			uffs_CriticalExit(); /* exit critical */
			return block;
		}
	}
}

#if 0
static void * _kmalloc_clear(int size)
{
	void *p;
	
	p = _kmalloc(size);
	if(p)
		memset(p, 0, size);
	return p;
}
#endif

/*!
 * \brief Return a block to heap memory.
 *
 * An application calls this function, when a previously
 * allocated memory block is no longer needed.
 *
 * The heap manager checks, if the released block adjoins any
 * other free regions. If it does, then the adjacent free regions
 * are joined together to form one larger region.
 *
 * \note Interrupts are automatically enabled, when this
 *       function returns.
 *
 * \param block Points to a memory block previously allocated
 *              through a call to _kmalloc().
 *
 * \return 0 on success, -1 if the caller tried to free
 *         a block which had been previously released.
 */
static int _kfree(void *block)
{
	HEAPNODE *node;
	HEAPNODE *prev;
	HEAPNODE *next;
	if (block == NULL) {
		return -1;	//the pointer of the memory is invalid.
	}
	uffs_CriticalEnter();	/* enter critical */
	
	node = (HEAPNODE *)((u32)block - ALLOC_OFFSET);
	if(node->mark != (int)HEAP_NODE_ALLOCED || node->size <= ALLOC_OFFSET) {
		uffs_CriticalExit();/* exit critical */
		return -1;	/*!!!! at this moment, the heap 
						management info must be damaged !!!!!*/
	}
	_k_heap_available += node->size;
	
	prev = node->prevNode;
	next = (HEAPNODE *)((u32)node + node->size);

	if(prev->mark == HEAP_NODE_FREE){
        /*
         * If there' s a free node in front of us, merge it.
         */
		prev->size += node->size;
		next->prevNode = prev;
		HeapDeleteFromFreeList(prev);
		node = prev;
	}

	if(next->mark == HEAP_NODE_FREE){
        /*
         * If there' s a free node following us, merge it.
         */
		node->size += next->size;
		((HEAPNODE *)((u32)next + next->size))->prevNode = node;
		HeapDeleteFromFreeList(next);
	}

	/*
	 *	now, we just chain the node to free list head.
	 */
	node->mark = HEAP_NODE_FREE;
	HeapChainToFreeList(node);
	uffs_CriticalExit();	/* exit critical */
	
	return 0;
}


/*!
 * \brief
 * Add a new memory region to the free heap.
 *
 * This function is called during
 * initialization.
 *
 * Applications typically do not call this function.
 *
 * \param addr Start address of the memory region.
 * \param size Number of bytes of the memory region.
 */
void uffs_InitHeapMemory(void *addr, int size)
{
	HEAPNODE *np;
	
	
	if(!((u32)addr & 3)){
		addr = (void *)(((u32)addr) + 4);
		addr = (void *)(((u32)addr) & ~3);
	}
	size &= ~ALLOC_PAGE_MASK;
	if(size < ALLOC_PAGE_SIZE * 3) return;

	uffs_CriticalEnter();
	
	/* pre alloc header node, size is ALLOC_PAGE_SIZE */
	np = (HEAPNODE *)addr;
	np->size = ALLOC_PAGE_SIZE;
	np->mark = HEAP_NODE_ALLOCED;
	np->prevNode = NULL;

	/* pre alloc tail node, size is -1 */
    np = (HEAPNODE *)((u32)addr + size - ALLOC_PAGE_SIZE);
	np->mark = HEAP_NODE_ALLOCED;
	np->size = -1;
	np->prevNode = (HEAPNODE *)((u32)addr + ALLOC_PAGE_SIZE);
	_k_heapTail_ = np;

	/* Free list head */
    np = (HEAPNODE *)((u32)addr + ALLOC_PAGE_SIZE);
    np->mark = HEAP_NODE_FREE;
    np->prevNode = (HEAPNODE *)addr;
    np->size = size - 2 * ALLOC_PAGE_SIZE;
    np->nextFree = NULL;
    np->prevFree = NULL;
	_k_heapFreeList = np;
	_k_heap_available = np->size;
	_minimu_heap_avaiable = _k_heap_available;
	
	_kernel_heap_total += size;

	uffs_CriticalExit();
}

/******************************************************************************************/


static void *__umalloc(uffs_memAllocator *mem, unsigned int size, HASHTBL * heapHashTbl);
static void *__ucalloc(uffs_memAllocator *mem, unsigned int num, unsigned int size, HASHTBL *heapHashTbl);
static void *__urealloc(uffs_memAllocator *mem, void *block, unsigned int size, HASHTBL *heapHashTbl);
static int __ufree(uffs_memAllocator *mem, void *p, HASHTBL * heapHashTbl);


///* init malloc/free system */
//static HASHTBL * InitHeapMM(void)
//{
//	HASHTBL * heapHashTbl;
//	heapHashTbl = (HASHTBL *)_kmalloc(sizeof(HEAP_MM*) * HEAP_HASH_SIZE);
//	if(heapHashTbl != NULL){
//		memset(heapHashTbl, 0, sizeof(HEAP_MM*) * HEAP_HASH_SIZE);
//		return heapHashTbl;
//	}
//	else{
//		return NULL;
//	}
//}

/* release all alloced memory from hash table,
 * return alloced pointer nummber.
 */
static int ReleaseHeap(uffs_memAllocator *mem, HASHTBL *heapHashTbl)
{
	int i;
	int count = 0;
	HEAP_MM volatile * node;

	if(heapHashTbl == NULL) return -1;
	for(i = 0; i < HEAP_HASH_SIZE; i++){
		while((node = heapHashTbl[i]) != NULL){
			__ufree(mem, node->p, heapHashTbl);
			count++;
		}
	}
	_kfree(heapHashTbl);
	return count;
}

static void *uffs_malloc(struct uffs_DeviceSt *dev, unsigned int size)
{
	HASHTBL * heapHashTbl;

	if((int)size < 0) return NULL;
	heapHashTbl = dev->mem.tbl;
	if(heapHashTbl){
		return __umalloc(&dev->mem, size, heapHashTbl);
	}
	else{
		return NULL;
	}
}


/* alloc one block with given size, return the block pointer */
static void *__umalloc(uffs_memAllocator *mem, unsigned int size, HASHTBL *heapHashTbl)
{
	void *p;
	HEAP_MM *node;
	int idx;
	
	/* calling kernel routin allocate bigger size memory block */
	p = _kmalloc(HEAP_MAGIC_SIZE + size + HEAP_MAGIC_SIZE);
	
	if(p){
		node = (HEAP_MM *)_kmalloc(sizeof(HEAP_MM));
		if(node == NULL){
			_kfree(p);
			return NULL;
		}
		p = (void *)((u32)p + HEAP_MAGIC_SIZE);	/* adjust pointer first */
		node->p = p;
		node->size = size;
		mem->count += size;
		if (mem->maxused < mem->count) mem->maxused = mem->count;
		node->task_id = uffs_OSGetTaskId();	/* get task id */
		
		uffs_CriticalEnter();
		
		/* insert node to hash table */
		idx = GET_HASH_INDEX(p);
		node->next = heapHashTbl[idx];
		heapHashTbl[idx] = node;
		
		uffs_CriticalExit();
		return p;	/* ok, return the pointer */
	}
	return NULL;
}

/* Allocates an array in memory with elements initialized to 0 */
static void *__ucalloc(uffs_memAllocator *mem, unsigned int num, unsigned int size, HASHTBL *heapHashTbl)
{
	return __umalloc(mem, num * size, heapHashTbl);
}


/* realloc one block with given size, return the block pointer */
static void *__urealloc(uffs_memAllocator *mem, void *block, unsigned int size, HASHTBL *heapHashTbl)
{
	void *p, *pNew;
	HEAP_MM *prev, *node;
	int idx;

	if(block == NULL) {
		return __umalloc(mem, size, heapHashTbl);
	}

	if(size == 0) {
		__ufree(mem, block, heapHashTbl);
		return NULL;
	}

	/* calculate hash idx */
	idx = GET_HASH_INDEX(block);

	/* check whether block pointer is alloc from this heap... */
	uffs_CriticalEnter();
	node = heapHashTbl[idx];
	prev = NULL;
	while(node){
		if(node->p == block){
			break; /* got it! */
		}
		prev = node;
		node = node->next;	/* search for next node */
	}

	if(!node) {
		/* not my duty :-) */
		uffs_CriticalExit();
		return NULL;
	}

	/* ok, begin call kernel API to realloc memory */

	p = (void *)((u32)block - HEAP_MAGIC_SIZE);	/* get real pointer which kernel need */
	pNew = _krealloc(p, HEAP_MAGIC_SIZE + size + HEAP_MAGIC_SIZE);

	if(pNew == NULL) {	/* realloc fail */
		uffs_CriticalExit();
		return NULL;
	}

	if(pNew == p) {
		/* new block is the same as the old block */
		uffs_CriticalExit();
		return block;
	}

	/* new block is difference with old block, we need to change hash table ... */
	if(prev){
		/* prev is not the first */
		prev->next = node->next;
	}
	else{
		/* this node is the first, so.. */
		heapHashTbl[idx] = node->next;
	}
	uffs_CriticalExit();

	node->p = (void *)((u32)pNew + HEAP_MAGIC_SIZE);
	node->size = size;
	node->task_id = uffs_OSGetTaskId();

	/* insert node into hash table */
	idx = GET_HASH_INDEX(node->p);
	uffs_CriticalEnter();
	node->next = heapHashTbl[idx];
	heapHashTbl[idx] = node;
	uffs_CriticalExit();

	return node->p;
	
}


/* free the block, if the pointer(parameter 'p') is 
 * not valid(allocated by this allocate system) or error occur, return -1,
 * else return 0
 */
static int __ufree(uffs_memAllocator *mem, void *p, HASHTBL *heapHashTbl)
{
	HEAP_MM *node, *prev;
	
	if(p){	/* check the pointer */
		uffs_CriticalEnter();
		node = heapHashTbl[GET_HASH_INDEX(p)];
		prev = NULL;
		while(node){
			if(node->p == p) {
				/* we find the node, so begin to release */
				if(prev){
					/* this node is not the first */
					prev->next = node->next;
				}
				else{
					/* this node is the first node of hash channel */
					heapHashTbl[GET_HASH_INDEX(p)] = node->next;
				}

				mem->count -= node->size;
				
				uffs_CriticalExit();
				if(_kfree(node) == -1)	/* calling kernel routine release node */
					return -1;			/* fail, return -1 */
				
				/* calling kernel routine and return */
				return _kfree((void *)((u32)p - HEAP_MAGIC_SIZE)); 
			}
			prev = node;
			node = node->next;	/* search for next node */
		}
		uffs_CriticalExit();
	}
	return -1;
}

static URET uffs_free(struct uffs_DeviceSt *dev, void *block)
{
	HASHTBL *heapHashTbl;
	heapHashTbl = dev->mem.tbl;
	if(heapHashTbl){
		if (__ufree(&dev->mem, block, heapHashTbl) < 0) {
			uffs_Perror(UFFS_ERR_SERIOUS, PFX"Try to free unmanaged memory ?\n");
			return U_FAIL;
		}
	}
	return U_SUCC;
}

URET uffs_initNativeMemAllocator(uffs_Device *dev)
{
	uffs_memAllocator *mem = &dev->mem;

	memset(mem->tbl, 0, sizeof(mem->tbl));
	mem->malloc = uffs_malloc;
	mem->free = uffs_free;
	mem->blockinfo_buffer_size = 0;
	mem->page_buffer_size = 0;
	mem->tree_buffer_size = 0;
	mem->one_page_buffer_size = 0;

	return U_SUCC;
}


URET uffs_releaseNativeMemAllocator(uffs_Device *dev)
{
	int count;
	URET ret = U_SUCC;
	if (dev) {
		count = ReleaseHeap(&dev->mem, dev->mem.tbl);
		if (count < 0) {
			uffs_Perror(UFFS_ERR_SERIOUS, PFX"Release native memory allocator fail!\n");
			ret = U_FAIL;
		}
		else if (count > 0) {
			uffs_Perror(UFFS_ERR_NORMAL, PFX"Find %d block memory leak!\n", count);
		}
	}
	return ret;
}

/**
 * \brief Setup the memory allocator to native memory allocator
 *
 * \param allocator memory allocator to be setup
 */
void uffs_SetupNativeMemoryAllocator(uffs_memAllocator *allocator)
{
	memset(allocator, 0, sizeof(uffs_memAllocator));
	allocator->init = uffs_initNativeMemAllocator;
	allocator->release = uffs_releaseNativeMemAllocator;
}

#endif //USE_NATIVE_MEMORY_ALLOCATOR

