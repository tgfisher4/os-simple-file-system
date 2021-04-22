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

bitmap_t inode_table_bitmap;
bitmap_t data_block_bitmap;

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

int fs_walk_inode_table(int from_inumber, struct fs_inode *next_inode){
    // initial setup
    static union fs_block buffer_block;
    static int curr_inumber = 1;
    static int ninodes = -1;
    static bool should_load_block = true;
    if( ninodes < 0 ){
        disk_read(0, buffer_block.data);
        ninodes = buffer_block.super.ninodes;
        buffer_block.inodes[0].size = -1; // flag that buffer_block is invalid
    }

    // handle arg
    if( from_inumber >= ninodes ) return -1; // -1 indicates invalid input
    else if( from_inumber >= 1 ) curr_inumber = from_inumber;
    // check if we have finished traversal
    if( curr_inumber >= ninodes ) return 0; // 0 indicates invalid inode

    int inode_table_idx = curr_inumber / INODES_PER_BLOCK;
    int inode_block_idx = curr_inumber % INODES_PER_BLOCK;
    // if buffer_block is invalid or we have reached a new block, need to perform disk_read
    bool should_load_block = buffer_block.inodes[0].size < 0 || inode_block_idx  == 0;
    if( should_load_block ){
        disk_read(inode_table_idx + INODE_TABLE_START_BLOCK, buffer_block.data);
    }
    // use memcpy because we don't want to assign next_inode to a memory address on the stack
    memcpy(next_inode, &buffer_block.inodes[inode_block_idx], sizeof(struct inode));

    return curr_inumber++;
}

int fs_walk_inode_data(int for_inumber, struct fs_inode *for_inode, char *data){
    // initial setup
    static int curr_block = 0;
    static struct fs_inode inode;
    static union fs_block ind_ptr_blk = {.pointers[0] = -1};

    // initialization
    if( inumber >= 1 || for_inode ){
        curr_block = 0;
        ind_ptr_blk.pointers[0] = -1;
        if( inumber > 0 )   load_inode(inumber, &inode); // err check load_inode?
        else                inode = *for_inode;
    }
    // short-circuit if no more data
    if( DISK_BLOCK_SIZE * curr_block >= inode.size || curr_block >= DATA_POINTERS_PER_INODE + DATA_POINTERS_PER_BLOCK ) return -1;
    // read from direct pointers
    if( curr_block < DATA_POINTERS_PER_INODE ){
        read_from = inode.direct[curr_block];
    }
    // read from indirect pointers
    else{
        if( ind_ptr_blk.pointers[0] < 0 ) disk_read(inode.indirect, ind_ptr_blk.data);
        read_from = ind_ptr_blk.pointers[curr_block - DATA_POINTERS_PER_INODE];
    }
    curr_block++;
    disk_read(read_from, data);
    return read_from;
}

// guidance from: https://stackoverflow.com/questions/10080832/c-i-need-some-guidance-in-how-to-create-dynamic-sized-bitmaps
typedef unsigned char* bitmap_t;
bitmap_t bitmap_create(int n_bits){
    if( n_bits <= 0 ) return NULL;
    // add 7 to round integer division up
    bitmap_t to_return = malloc((sizeof(*to_return) * n_bits + 7) / 8);
    if( !to_return ){
        printf("ERROR Failed to allocate memory. Exiting...\n");
        abort();
    }
    return to_return;
}

void bitmap_delete(bitmap_t bitmap){
    free(bitmap);
}

bool bitmap_test(bitmap_t bitmap, int idx){
    return bitmap[idx / 8] & (1 << (idx % 8));
}

void bitmap_set(bitmap_t bitmap, int idx, int val){
    if( bitmap_test(bitmap, idx) == val ) return;
    bitmap[idx / 8] ^= 1 << (idx % 8);
}

void inode_load(int inumber, struct fs_inode *inode){
    int inode_table_idx = inumber / INODES_PER_BLOCK;
    int inode_block_idx = inumber % INODES_PER_BLOCK;

    union fs_block buffer_block;
    disk_read(inode_table_idx, buffer_block.data);
    memcpy(inode, buffer_block[inode_block_idx]);
}

int fs_mount(){
    union fs_block buffer_block;
    disk_read(0, buffer_block.data);
    if( buffer_block.super.magic != FS_MAGIC ) return 0;

    // true bitmap: unsigned char *inode_table_bitmap = malloc(sizeof(unsigned char) * super.ninodes)
    // bits 0-7 in 0, 8-15 in 1, etc - access via `inode_table_bitmap[inumber / 8] & (1 << inumber % 8)`
    inode_table_bitmap = bitmap_create(buffer_block.super.ninodes);
    disk_block_bitmap = bitmap_create(buffer_block.super.nblocks);

    // initialize data_region_bitmap: mark superblock and inode table blocks as allocated, rest as free
    for( int i = 0; i < super.nblocks + 1; i++ ) bitmap_set(disk_block_bitmap, i, !(i < super.ninodeblocks + 1));

    struct fs_inode inode;
    bitmap_set(inode_table_bitmap, 0, 0); // inode 0 is not available for use
    for( int inumber = fs_walk_inode_table(1, &inode); inumber > 0; inumber = fs_walk_inode_table(-1, &inode) ){
        bitmap_set(inode_table_bitmap, inumber, !inode.isvalid);
        for( int data_block_num = fs_walk_inode_data(0, &inode, buffer_block.data); data_block_num > 0; data_block_num = fs_walk_inode_data(0, NULL, buffer_block.data) ){
            bitmap_set(disk_block_bitmap, data_block_num, 0);
        }
    }

    return 1;
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
