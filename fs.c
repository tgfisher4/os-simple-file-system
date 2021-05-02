#include "fs.h"
#include "disk.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

// unfortunately, must define macros to use in struct definitions whose values are really arbitrary
// can represent them as expressions of other macros, but then have to compute the value on every use
#define FS_MAGIC                0xf0f03410
#define DATA_POINTERS_PER_INODE    5  // truly arbitrary
#define DATA_POINTER_SIZE          4  // just an int
#define INODE_SIZE                32  // = 4 + DATA_POINTERS_PER_INODE * DATA_POINTER_SIZE + 4 + 4;
#define INODES_PER_BLOCK         128  // = DISK_BLOCK_SIZE / INODE_SIZE
#define DATA_POINTERS_PER_BLOCK 1024  // = DISK_BLOCK_SIZE / DATA_POINTER_SIZE
#define INODE_TABLE_START_BLOCK 1     // inode table start immediately after superblock

struct fs_superblock {
    int magic;
    int nblocks;
    int ninodeblocks;
    int ninodes;
};

struct fs_inode {
    int isvalid;
    int size;
    int direct[DATA_POINTERS_PER_INODE];
    int indirect;
};

union fs_block {
    struct fs_superblock super;
    struct fs_inode inodes[INODES_PER_BLOCK];
    int pointers[DATA_POINTERS_PER_BLOCK];
    char data[DISK_BLOCK_SIZE];
};

int fs_format()
{
    return 0;
}

void fs_debug(){
    union fs_block buffer_block;

    // superblock
    disk_read(0, buffer_block.data);
    struct fs_superblock superblock = buffer_block.super;
    printf("superblock:\n");
    printf("    magic number %s valid\n", superblock.magic == FS_MAGIC ? "is" : "is not");
    printf("    %d blocks total on disk\n", superblock.nblocks);
    printf("    %d blocks dedicated to inode table on disk\n", superblock.ninodeblocks);
    printf("    %d total spots in inode table\n", superblock.ninodes);

    // walk inode table
    for( int block = 0; block < superblock.ninodeblocks; ++block ){
        disk_read(block + INODE_TABLE_START_BLOCK, buffer_block.data);
        struct fs_inode *inodes = buffer_block.inodes;
        for( int i = 0; i < INODES_PER_BLOCK; ++i ){
            struct fs_inode inode = inodes[i];
            int inumber = block * INODES_PER_BLOCK + i;
            /*
            if( inumber == 3 ){
                printf("inode 3:\n");
                printf("    valid?: %d\n", inode.isvalid);
                printf("    size: %d\n", inode.size);
            }
            */
            // skip invalid inodes and inode 0
            if( !inode.isvalid || inumber == 0 ) continue;
            printf("inode %d:\n", inumber);
            printf("    size: %d bytes\n", inode.size);

            printf("    direct data blocks:");
            int processed_size = 0;
            for( int d_ptr_idx = 0; processed_size < inode.size && d_ptr_idx < DATA_POINTERS_PER_INODE; ++d_ptr_idx, processed_size += DISK_BLOCK_SIZE ){
                printf(" %d", inode.direct[d_ptr_idx]);
            }
            printf("\n");

            if( processed_size < inode.size ){
                printf("    indirect block: %d\n", inode.indirect);
                printf("    indirect data blocks:");
                disk_read(inode.indirect, buffer_block.data);
                int *id_ptrs = buffer_block.pointers;
                for( int id_ptr_idx = 0; processed_size < inode.size && id_ptr_idx < DATA_POINTERS_PER_BLOCK; ++id_ptr_idx, processed_size += DISK_BLOCK_SIZE ){
                    printf(" %d", id_ptrs[id_ptr_idx]);
                }
                printf("\n");
                if( processed_size < inode.size ){
                    printf("    WARNING: inode exceeds capacity of direct and indirect data blocks\n");
                }
            }
        }
    }
}

int fs_get_inode_block(int inumber){
    return inumber / INODES_PER_BLOCK;
}

int fs_get_inode_offset(int inumber){
    return inumber % INODES_PER_BLOCK;
}

int fs_mount()
{
    return 0;
}

int fs_create()
{
	// Use bitmap to identify a free inode in the inode table block
	union fs_block block_buffer;
	disk_read(0, block_buffer.data);
	int inumber = 0;
	for (int i = 1; i <= block_buffer.super.ninodes; i++) {
		inumber = bitmap_test(inode_table_bitmap, i);
		if (inumber) break;
	}
	if (!inumber) {
		// No free inodes; return zero
		return 0;
	}
	int block_num = INODE_TABLE_START_BLOCK + fs_get_inode_block(inumber);
	int block_offset = fs_get_inode_offset(inumber);

	// Initialize the inode struct
	struct fs_inode new_inode;
	new_inode.isvalid = 1;
	new_inode.size = 0;

	// Read block from disk
	disk_read(block_num, block_buffer.data);

	// Write the inode struct to free inode position
	block_buffer.inodes[block_offset] = new_inode;

	// Write back the block
	disk_write(block_num, block_buffer.data);
    return inumber;
}

int fs_delete( int inumber )
{	
	// Validate valid inumber
	union fs_block block_buffer;
	disk_read(0, block_buffer.data);
	if ((inumber < 1) || (inumber > block_buffer.super.ninodes))
		return 0; // Invalid inumber

	// Read the inode, update the isvald bit, and write back
	int block_num = INODE_TABLE_START_BLOCK + fs_get_inode_block(inumber);
	int block_offset = fs_get_inode_offset(inumber);

	disk_read(block_num, block_buffer.data);
	if (block_buffer.inodes[block_offset].isvalid == 0) {
		// Return 0 -- attempting to delete an inode that's not yet created
		return 0;
	};
	block_buffer.inodes[block_offset].isvalid = 0;
	disk_write(block_num, block_buffer.data);

	// Walk the data blocks, free them, and update the bitmap
	for ( int i = fs_walk_inode_data(inumber, 0, NULL); i > 0; i = fs_walk_inode_data(NULL, NULL, NULL) ) {
		bitmap_set(data_block_bitmap, i, 1);
	}
	
	// Update the inode table bitmap
	bitmap_set(inode_table_bitmap, inumber, 1);
    
	return 1;
}

int fs_getsize( int inumber )
{
    return -1;
}

int fs_read( int inumber, char *data, int length, int offset )
{
    return 0;
}

int fs_write( int inumber, const char *data, int length, int offset )
{
    return 0;
}

// Helper function for fs_debug; Moves a data block to a temporary defragged data region
void move_block( int block_num, int index, union fs_block* defrag_data)
{
	union fs_block block_buffer;
	disk_read(block_num, block_buffer.data)
	memcpy(defrag_data + index, block_buffer.data, sizeof(union fs_block))
}

int fs_defrag()
{

	/*  Create a temporary inode table and data region to hold defragged data */
	union fs_block block_buffer;
	disk_read(0, block_buffer.data);

	int defrag_inumber = 1; // Next available position in defragged inode table
	int defrag_data_index = 0; // Next available position in defragged data region
	int ninodeblocks = block_buffer.super.ninodeblocks;
	int ninodes = block_buffer.super.ninodes;
	int nblocks = block_buffer.super.nblocks;
	
	union fs_block* defrag_inode_table = malloc(sizeof(union fs_block) * ninodeblocks);
	union fs_block* defrag_data = malloc(sizeof(union fs_block) * (nblocks - ninodeblocks - 1));
	
	/* Iterate through inode blocks */
	for (int i = INODE_TABLE_START_BLOCK; i <= ninodeblocks; i++) {
		disk_read(i, block_buffer.data);
		// Iterate through each inode in the block
		for (int j = 0; j < INODES_PER_BLOCK; j++) {

			// If inode is valid and taken, add to defragged table and defrag data region

			if ( (i == INODE_TABLE_START_BLOCK) && (j == 0 ) ) continue;

			int inumber = (i - 1) * INODES_PER_BLOCK + j;
			if ( !bitmap_test(inode_table_bitmap, inumber) ) {

				/*  Add to defragged table */
				int defrag_inode_offset = fs_get_inode_offset(defrag_inumber)
				int inode_offset = fs_get_inode_offset(inumber);
				union fs_block *defrag_block = defrag_inode_table + i - 1;

				defrag_block->inodes[defrag_inode_offset] = block_buffer.inodes[inode_offset];
				defrag_inumber++; // Increment inumber to be next available one
				
				/*  Defrag  data pointers */
				
				// Loop through indirect block's pointers and direct pointers separately
				int num_blocks = (defrag_block->inodes[defrag_inode_offset].size + 4095) / 4096;
				int direct_blocks = 0;
				int indirect_blocks = 0;
				if (num_blocks <= 5) {
					direct_blocks = num_blocks;
				} else {
					direct_blocks = 5;
					indirect_blocks = num_blocks - 5;
				}

				// Direct pointer blocks
				for (int i = 0; i < num_blocks; i++) {
					// Move to temp data region
					int block_num = defrag_block->inodes[defrag_inode_offset].direct[i];
					move_data(block_num, defrag_data_index, defrag_data);

					// Update the inode direct pointers and defrag data index
					defrag_block->inodes[defrag_inode_offset].direct[i] = defrag_data_index;
					defrag_data_index++;
				}
	
				// Indirect pointer blocks
				if (indirect_blocks)
				{
					// Read the indirect block and create new indirect block
					union fs_block defrag_indirect_block;
					disk_read(defrag_block->inodes[defrag_inode_offset].indirect, defrag_indirect_block.data);

					// Direct pointers inside indirect block
					for (int i = 0; i < indirect_blocks; i++) {
						int block_num = block_buffer.pointers[i];
						move_data(block_num, defrag_data_index, defrag_data);

						// Update the inode direct pointers and defrag data index
						defrag_indirect_block.pointers[i] = defrag_data_index;
						defrag_data_index++;
					}

					// Move the indirect block itself into defrag data
					memcpy(defrag_data + defrag_data_index, defrag_indirect_block.data, sizeof(union fs_block))
					defrag_data_index++;
				}
			}
		}
		
		
		
	}
	
	/* Modify the inode bitmap */
	inode_table_bitmap = inode_table_bitmap & 0;
	for (int i = defrag_inumber; i <= ninodes; i++) {
		bitmap_set(inode_table_bitmap, i, 1);
	}

	/* Write inode table to the disk */
	for (int i = 0; i < ninodeblocks; i++) {
		diskwrite(i+INODE_TABLE_START_BLOCK , (defrag_inode_table + i)->data);
	}

	/* Modify the data bitmap */
	disk_block_bitmap = disk_block_bitmap & 0;
	for (int i = defrag_data_index; i < nblocks - ninodeblocks - 1; i++) {
		bitmap_set(disk_block_bitmap, i, 1);
	}

	/* Write data table to the disk */
	for (int i = 0; i < nblocks - ninodeblocks - 1; i++) {
		diskwrite(i + INODE_TABLE_START_BLOCK + ninodeblocks, (defrag_data + i)->data);
	}

	/* Free allocated structures */
	free(defrag_inode_table);
	free(defrag_data);

	return 0;
}
