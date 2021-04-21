#include "fs.h"
#include "disk.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>

// unfortunately, must define macros to use in struct definitions whose values are really arbitrary
// can represent them as expressions of other macros, but then have to compute the value on every use
#define FS_MAGIC                0xf0f03410
#define DATA_POINTERS_PER_INODE    5  // truly arbitrary
#define DATA_POINTER_SIZE          4  // just an int
#define INODE_SIZE                32  // = 4 + DATA_POINTERS_PER_INODE * DATA_POINTER_SIZE + 4 + 4;
#define INODES_PER_BLOCK         128  // = DISK_BLOCK_SIZE / INODE_SIZE
#define DATA_POINTERS_PER_BLOCK 1024  // = DISK_BLOCK_SIZE / DATA_POINTER_SIZE
#define INODE_TABLE_START_BLOCK 1     // inode table start immediately after superblock

// global variables
bool is_mounted = false;

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

int fs_format() {
	// don't format: already mounted
	if (is_mounted)	return 0;

	union fs_block buffer_block;
	
	// read superblock
	disk_read(0, buffer_block.data);
    struct fs_superblock *superblock = &buffer_block.super;

	// set superblock values
	superblock->magic = FS_MAGIC;
	superblock->nblocks = disk_size();
	// compute 10% of blocks for inodes
	int ninodeblocks_temp = superblock->nblocks / 10;
	superblock->ninodeblocks = ninodeblocks_temp;
	superblock->ninodes = superblock->ninodeblocks * INODES_PER_BLOCK;

	// write superblock values
	disk_write(0, buffer_block.data);

	// traverse inode table and invalidate - update with itok()?
    for( int block = 0; block < ninodeblocks_temp; ++block ) {
        disk_read(block + INODE_TABLE_START_BLOCK, buffer_block.data);
        struct fs_inode *inodes = buffer_block.inodes;
        for( int i = 0; i < INODES_PER_BLOCK; ++i ) {
            inodes[i].isvalid = 0;
		}
		disk_write(block + INODE_TABLE_START_BLOCK, buffer_block.data);
	}

    return 1;
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
    return 0;
}

int fs_delete( int inumber )
{
    return 0;
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
