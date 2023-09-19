// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"
#include "helpers.h"

block_meta *global_base = NULL; /*Heap base*/

/**
 * @brief Find a free block of memory with the required size
 * 
 * @param size The size of the block
 * @return block_meta* The block found or NULL if no block was found
 */
static block_meta *find_best_fit(size_t size)
{
	/*Find the best fit block which means going through the whole list and returning the block
	with the size closest to the required one*/
	block_meta *current = global_base;
	block_meta *best_fit = NULL;
	while (current)
	{
		if (current->status == STATUS_FREE && current->size >= size)
		{
			if (!best_fit || current->size < best_fit->size)
			{
				best_fit = current;
			}
		}
		current = current->next;
	}
	return best_fit;
}


/**
 * @brief Get a new block of memory with the required size,
 * used by os_malloc
 * 
 * @param size The size of the block
 * @return block_meta* The newly allocated block
 */
static block_meta *request_space_malloc(size_t size)
{
	block_meta *block;
	if (size < MMAP_THRESHOLD) /*For sizes smaller than MMAP_THRESHOLD use sbrk*/
	{
		block = sbrk(0);
		block = sbrk(size);
		DIE(block == (void *)-1, "sbrk");
		block->size = size;
		block->status = STATUS_ALLOC;
		block->next = NULL;
	}
	else /*Else we use mmap*/
	{
		block = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		DIE(block == MAP_FAILED, "mmap");
		block->size = size;
		block->status = STATUS_MAPPED;
		block->next = NULL;
	}

	return block;
}


/**
 * @brief Get a new block of memory with the required size,
 * used by os_calloc
 * 
 * @param size The size of the block
 * @return block_meta* The newly allocated block
 */
static block_meta *request_space_calloc(size_t size)
{
	size_t page_size = sysconf(_SC_PAGESIZE);
	block_meta *block;
	if (size < page_size) /*For sizes smaller than the memory page size use sbrk*/
	{
		block = sbrk(0);
		block = sbrk(size);
		DIE(block == (void *)-1, "sbrk");
		block->size = size;
		block->status = STATUS_ALLOC;
		block->next = NULL;
	}
	else /*Else we use mmap*/
	{
		block = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		DIE(block == MAP_FAILED, "mmap");
		block->size = size;
		block->status = STATUS_MAPPED;
		block->next = NULL;
	}

	return block;
}


/**
 * @brief Split a block of memory into two blocks
 * 
 * @param block The block to be split
 * @param size The size of the first block
 */
static void split_block(block_meta *block, size_t size)
{
	block_meta *new_block = (void *)block + size;
	new_block->size = block->size - size;
	new_block->status = STATUS_FREE;
	new_block->next = block->next;
	block->size = size;
	block->next = new_block;

}


/**
 * @brief Coalesce all the free blocks in the list
 * 
 * @return block_meta* The last block in the list
 */
static block_meta *coalesce_blocks()
{
	block_meta *current = global_base;
	block_meta *prev = NULL;
	while (current)
	{
		if (current->status == STATUS_FREE)
		{
			if (prev && prev->status == STATUS_FREE)
			{
				prev->size += current->size;
				prev->next = current->next;
				current = prev;
			}
			if (current->next && current->next->status == STATUS_FREE)
			{
				current->size += current->next->size;
				current->next = current->next->next;
			}
		}
		prev = current;
		current = current->next;
	}

	return prev;
}


/**
 * @brief Preallocates memory on the heap for the first time
 * to reduce the number of brk system calls. Preallocated
 * memory is 128KB
 * 
 */
static void first_time_prealloc()
{
	global_base = sbrk(0);
	global_base = sbrk(MMAP_THRESHOLD);
	DIE(global_base == (void *)-1, "sbrk");
	global_base->size = MMAP_THRESHOLD;
	global_base->status = STATUS_FREE;
	global_base->next = NULL;

}


/**
 * @brief Expand the size of a block of memory. Checks if all the
 * blocks after the current one are free and if they are, it expands
 * the current block to the required size. 
 * 
 * @param block The block to be expanded
 * @param size The new size of the block
 * @return block_meta* The new block or NULL if the expansion failed
 */
static block_meta *realloc_expand(block_meta *block, size_t size)
{
	block_meta *next = block->next;
	while (next && next->status == STATUS_FREE)
	{
		block->size += next->size;
		block->next = next->next;
		next = block->next;
		
	}

	if (block->size >= size)
	{
		return block;
	}
	else
	{
		return NULL;
	}
}

/**
 * @brief Get the block pointer from the data pointer
 * 
 * @param ptr The data pointer
 * @return block_meta* The block pointer
 */
static block_meta *get_block_ptr(void *ptr)
{
	return (block_meta *)(ptr - ALIGN(sizeof(block_meta)));
}

void *os_malloc(size_t size)
{
	/* TODO: Implement os_malloc */
	if (size == 0)
	{
		return NULL;
	}
	//Calculate the aligned size
	int aligned_size = ALIGN(sizeof(block_meta)) + ALIGN(size);

	if (!global_base && aligned_size < MMAP_THRESHOLD)
	{
		first_time_prealloc(); /*Preallocate memory for the first time*/
	}

	/*Before searching for a free block we coalesce all the free blocks
	Also save the last block in the list because we will need it later*/
	block_meta *last = coalesce_blocks();
	
	//Find the best fit block
	block_meta *block = find_best_fit(aligned_size);
	if (block)
	{
		//If the block is bigger than the required size we split it
		if (block->size >= aligned_size + ALIGN(sizeof(block_meta) + ALIGN(1)))
		{
			split_block(block, aligned_size);
		}
		block->status = STATUS_ALLOC;
		return (void *)block + ALIGN(sizeof(block_meta));
	}
	else
	{
		if (last && last->status == STATUS_FREE) /*If the last block is free we expand it*/
		{
			block = request_space_malloc(aligned_size - last->size);
			if (!block)
			{
				return NULL;
			}
			last->size += block->size;
			block = last;
			if (block->size >= aligned_size + ALIGN(sizeof(block_meta) + ALIGN(1)))
			{
				split_block(block, aligned_size);
			}
			block->status = STATUS_ALLOC;
			return (void *)block + ALIGN(sizeof(block_meta));
		}
		
		//If no block was found we request a new one
		block = request_space_malloc(aligned_size);
		if (!block)
		{
			return NULL;
		}
		if (block->status == STATUS_ALLOC)
		{
			//If the block is allocated with brk we add it to the end of the list
			last->next = block;
			if (last->status == STATUS_FREE)
			{
				//If the last block is free we coalesce it with the new block
				last->size += block->size;
				last->next = block->next;
			}
		}
		return (void *)block + ALIGN(sizeof(block_meta));
	}
	
}

void os_free(void *ptr)
{
	/* TODO: Implement os_free */
	if (!ptr)
	{
		return;
	}

	block_meta *block = get_block_ptr(ptr);
	if (block->status == STATUS_ALLOC)
	{
		//If the block is allocated with brk we mark it as free
		block->status = STATUS_FREE;
	}
	else
	{
		//If the block is allocated with mmap we free it
		int ret = munmap(block, block->size);
		DIE(ret == -1, "munmap");
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	/* TODO: Implement os_calloc */
	if (nmemb == 0 || size == 0)
	{
		return NULL;
	}

	size *= nmemb;

	//Calculate the aligned size
	int aligned_size = ALIGN(sizeof(block_meta)) + ALIGN(size);

	if (!global_base && aligned_size < sysconf(_SC_PAGESIZE))
	{
		first_time_prealloc(); /*Preallocate memory for the first time*/
	}

	/*Before searching for a free block we coalesce all the free blocks
	Also save the last block in the list because we will need it later*/
	block_meta *last = coalesce_blocks();
	
	//Find the best fit block
	block_meta *block = find_best_fit(aligned_size);
	if (block)
	{
		//If the block is bigger than the required size we split it
		if (block->size >= aligned_size + ALIGN(sizeof(block_meta) + ALIGN(1)))
		{
			split_block(block, aligned_size);
		}
		block->status = STATUS_ALLOC;
		//Set the memory to 0
		memset((void *)block + ALIGN(sizeof(block_meta)), 0, size);
		return (void *)block + ALIGN(sizeof(block_meta));
	}
	else
	{
		if (last && last->status == STATUS_FREE) /*If the last block is free we expand it*/
		{
			block = request_space_calloc(aligned_size - last->size);
			if (!block)
			{
				return NULL;
			}
			last->size += block->size;
			block = last;
			if (block->size >= aligned_size + ALIGN(sizeof(block_meta) + ALIGN(1)))
			{
				split_block(block, aligned_size);
			}
			block->status = STATUS_ALLOC;
			//Set the memory to 0
			memset((void *)block + ALIGN(sizeof(block_meta)), 0, size);
			return (void *)block + ALIGN(sizeof(block_meta));
		}
		
		//If no block was found we request a new one
		block = request_space_calloc(aligned_size);
		if (!block)
		{
			return NULL;
		}
		if (block->status == STATUS_ALLOC)
		{
			//If the block is allocated with brk we add it to the end of the list
			last->next = block;
			if (last->status == STATUS_FREE)
			{
				//If the last block is free we coalesce it with the new block
				last->size += block->size;
				last->next = block->next;
			}
		}
		//Set the memory to 0
		memset((void *)block + ALIGN(sizeof(block_meta)), 0, size);
		return (void *)block + ALIGN(sizeof(block_meta));
	}

}

void *os_realloc(void *ptr, size_t size)
{
	/* TODO: Implement os_realloc */
	size_t aligned_size = ALIGN(size) + ALIGN(sizeof(block_meta));
	if (!ptr)
	{
		return os_malloc(size);
	}
	if (size == 0)
	{
		os_free(ptr);
		return NULL;
	}


	block_meta *block = get_block_ptr(ptr);
	//Return NULL if the block is not allocated
	if (block->status == STATUS_FREE)
	{
		return NULL;
	}
	//Do nothing if the size is the same
	if (block->size == aligned_size)
	{
		return ptr;
	}


	//If the block is allocated with mmap we can't expand it
	if (block->status == STATUS_MAPPED)
	{
		void *new_ptr = os_malloc(size);
		block_meta *new_block = get_block_ptr(new_ptr);
		if (!new_ptr)
		{
			return NULL;
		}
		memcpy(new_ptr, ptr, new_block->size - ALIGN(sizeof(block_meta)));
		os_free(ptr);
		return new_ptr;
	}

	//If the new size is smaller than the old one we truncate the block
	if (block->size >= aligned_size)
	{	
		//If the block is bigger than the required size we split it
		if (block->size >= aligned_size + ALIGN(sizeof(block_meta) + ALIGN(1)))
		{
			split_block(block, aligned_size);
			return ptr;
		}
	}

	coalesce_blocks();
	
	//Try to expand the block
	block = realloc_expand(block, aligned_size);
	if (block)
	{
		//If the block is bigger than the required size we split it
		if (block->size >= aligned_size + ALIGN(sizeof(block_meta) + ALIGN(1)))
		{
			split_block(block, aligned_size);
		}
		return ptr;
	}
	else
	{
		//If the block can't be expanded we allocate a new one and copy the data
		void *new_ptr = os_malloc(size);
		block_meta *new_block = get_block_ptr(new_ptr);
		if (!new_ptr)
		{
			return NULL;
		}
		memcpy(new_ptr, ptr, new_block->size - ALIGN(sizeof(block_meta)));
		os_free(ptr);
		return new_ptr;
	}
}
