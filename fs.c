#include "fs.h"
#include "disk.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>

/* macros */
// unfortunately, must define macros to use in struct definitions whose values are really arbitrary
// can represent them as expressions of other macros, but then have to compute the value on every use
#define FS_MAGIC                0xf0f03410
#define DATA_POINTERS_PER_INODE    5  // truly arbitrary
#define DATA_POINTER_SIZE          4  // just an int
#define INODE_SIZE                32  // = 4 + DATA_POINTERS_PER_INODE * DATA_POINTER_SIZE + 4 + 4;
#define INODES_PER_BLOCK         128  // = DISK_BLOCK_SIZE / INODE_SIZE
#define DATA_POINTERS_PER_BLOCK 1024  // = DISK_BLOCK_SIZE / DATA_POINTER_SIZE
#define INODE_TABLE_START_BLOCK    1  // inode table start immediately after superblock


/* types */
typedef unsigned char* bitmap_t;
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


/* helper function prototypes */
// true bitmap: one bit per entry
// bits 0-7 in 0, 8-15 in 1, etc - access via `bitmap[bit / 8] & (1 << bit % 8)`
bitmap_t bitmap_create(int n_bits);
void     bitmap_delete(bitmap_t bitmap);
bool     bitmap_test(bitmap_t bitmap, int idx);
void     bitmap_set(bitmap_t bitmap, int idx, bool val);
void     bitmap_print(bitmap_t bitmap, int n_bits);

void     load_inode(int inumber, struct fs_inode *inode);
int      walk_inode_table(int from_inumber, struct fs_inode* inode);
int      walk_inode_data(int for_inumber, struct fs_inode* for_inode, char *data);


/* globals */
bitmap_t inode_table_bitmap;
bitmap_t disk_block_bitmap;
bool     is_mounted;


/* function definitions */
// guidance from: https://stackoverflow.com/questions/10080832/c-i-need-some-guidance-in-how-to-create-dynamic-sized-bitmaps
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

void bitmap_set(bitmap_t bitmap, int idx, bool val){
    if( bitmap_test(bitmap, idx) == val ) return;
    bitmap[idx / 8] ^= 1 << (idx % 8);
}

void bitmap_print(bitmap_t bitmap, int n_bits){
    int width = 1;
    for( int place_value = 1; n_bits > 10 * place_value; width += 1, place_value *= 10 );
    printf("BITMAP START\n");
    for( int byte = 0; byte < (n_bits + 7)/8; ++byte ){
        int start = byte * 8;
        printf("  %*d - %*d: ", width, start, width, start + 7);
        for( int bit = 0; bit < 8; ++bit ){
            printf("%d", bitmap_test(bitmap, start + bit));
        }
        printf("\n");
    }
    printf("BITMAP END\n");
}

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
    struct fs_inode inode;
    for( int inumber = walk_inode_table(1, &inode); inumber > 0; inumber = walk_inode_table(-1, &inode) ){
        if( !inode.isvalid || inumber == 0 ) continue;
        printf("inode %d:\n", inumber);
        printf("    size: %d bytes\n", inode.size);
        printf("    direct data blocks:");
        // walk data blocks for this inode
        for( int block_num = walk_inode_data(0, &inode, NULL); block_num > 0; block_num = walk_inode_data(0, NULL, NULL) ){
            if( block_num == DATA_POINTERS_PER_INODE ){
                printf("\n    indirect block: %d", inode.indirect);
                printf("\n    indirect data blocks:");
            }
            printf(" %d", block_num);
        }
        printf("\n");
    }
}

int walk_inode_table(int from_inumber, struct fs_inode *next_inode){
    // initial setup
    static union fs_block buffer_block;
    static int curr_inumber = 1;
    static int ninodes = -1;
    //static bool should_load_block = true;
    if( ninodes < 0 ){
        disk_read(0, buffer_block.data);
        ninodes = buffer_block.super.ninodes;
        buffer_block.inodes[0].size = -1; // flag that buffer_block is invalid
    }

    // handle arg
    if( from_inumber >= ninodes ) return -1; // -1 indicates invalid input
    else if( from_inumber >= 1 ){
        int curr_blk = curr_inumber / INODES_PER_BLOCK;
        int from_blk = from_inumber / INODES_PER_BLOCK;
        if( curr_blk != from_blk ) buffer_block.inodes[0].size = -1;
        curr_inumber = from_inumber;
    }
    // check if we have finished traversal
    if( curr_inumber >= ninodes ) return 0; // 0 indicates invalid inode

    int block_of_inode_table = curr_inumber / INODES_PER_BLOCK;
    int inode_of_block = curr_inumber % INODES_PER_BLOCK;
    // if buffer_block is invalid or we have reached a new block, need to perform disk_read
    bool should_load_block = buffer_block.inodes[0].size < 0 || inode_of_block == 0;
    if( should_load_block ){
        disk_read(block_of_inode_table + INODE_TABLE_START_BLOCK, buffer_block.data);
    }
    // use memcpy because we don't want to assign next_inode to a memory address on the stack
    memcpy(next_inode, &buffer_block.inodes[inode_of_block], sizeof(struct fs_inode));

    return curr_inumber++;
}

int walk_inode_data(int for_inumber, struct fs_inode *for_inode, char *data){
    // initial setup
    static int curr_block = 0;
    static struct fs_inode inode;
    static union fs_block ind_ptr_blk = {.pointers[0] = -1};

    // initialization
    if( for_inumber >= 1 || for_inode ){
        curr_block = 0;
        ind_ptr_blk.pointers[0] = -1;
        if( for_inumber > 0 )   load_inode(for_inumber, &inode);
        else                    inode = *for_inode; // this implicitly copies, so we don't have to worry about for_inode being modified later
    }
    // short-circuit if no more data
    if( DISK_BLOCK_SIZE * curr_block >= inode.size || curr_block >= DATA_POINTERS_PER_INODE + DATA_POINTERS_PER_BLOCK ) return -1;

    int read_from;
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
    if( data ) disk_read(read_from, data); // allow data to be null
    return read_from;
}


void load_inode(int inumber, struct fs_inode *inode){
    int inode_table_idx = inumber / INODES_PER_BLOCK;
    int inode_block_idx = inumber % INODES_PER_BLOCK;

    union fs_block buffer_block;
    disk_read(inode_table_idx, buffer_block.data);
    memcpy(inode, &buffer_block.inodes[inode_block_idx], DISK_BLOCK_SIZE);
}

int fs_mount(){
    union fs_block buffer_block;
    disk_read(0, buffer_block.data);
    if( buffer_block.super.magic != FS_MAGIC ) return 0;

    inode_table_bitmap = bitmap_create(buffer_block.super.ninodes);
    disk_block_bitmap = bitmap_create(buffer_block.super.nblocks);

    // initialize data_region_bitmap: mark superblock and inode table blocks as allocated, rest as free
    for( int i = 0; i < buffer_block.super.nblocks + 1; i++ ) bitmap_set(disk_block_bitmap, i, !(i < buffer_block.super.ninodeblocks + 1));

    struct fs_inode inode;
    bitmap_set(inode_table_bitmap, 0, 0); // inode 0 is not available for use
    for( int inumber = walk_inode_table(1, &inode); inumber > 0; inumber = walk_inode_table(-1, &inode) ){
        bitmap_set(inode_table_bitmap, inumber, !inode.isvalid);
        if( !inode.isvalid ) continue;
        for( int data_block_num = walk_inode_data(0, &inode, NULL); data_block_num > 0; data_block_num = walk_inode_data(0, NULL, NULL) ){
            if( data_block_num == DATA_POINTERS_PER_INODE ) bitmap_set(disk_block_bitmap, inode.indirect, 0);
            bitmap_set(disk_block_bitmap, data_block_num, 0);
        }
    }

    // bitmap_print(inode_table_bitmap, buffer_block.super.ninodes);
    // bitmap_print(disk_block_bitmap, buffer_block.super.nblocks);

    return 1;
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
