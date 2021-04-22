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
	inumber = 0;
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

	union fs_block block_buffer;
	disk_read(block_num, block_buffer.data);
	if (block_buffer.inodes[block_offset].isvalid == 0) {
		// Return 0 -- attempting to delete an inode that's not yet created
		return 0;
	};
	block_buffer.inodes[block_offset].isvalid = 0;
	disk_write(block_num, block_buffer.data);

	// Walk the data blocks, free them, and update the bitmap
	for ( int data_block_num = fs_walk_inode_data(inumber, 0, NULL); data_block_num > 0; data_block_num = fs_walk_inode_data(NULL, NULL, NULL) ) {
		bitmap_set(data_block_bitmap, data_block_num, 1);
	}
	
	// Update the inode table bitmap
	bitmap_set(inode_table_bitmap, inumber, 1)
    
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
