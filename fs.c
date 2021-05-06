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

int      min(int first, int second);

void     load_inode(int inumber, struct fs_inode *inode);
int      walk_inode_table(int from_inumber, struct fs_inode* inode);
int      walk_inode_data(int for_inumber, struct fs_inode* for_inode, char *data);
void     move_block(int block_num, int index, union fs_block* defrag_data);


/* globals */
bitmap_t inode_table_bitmap;
bitmap_t disk_block_bitmap;
bool     is_mounted = false;


/* function definitions */
// guidance from: https://stackoverflow.com/questions/10080832/c-i-need-some-guidance-in-how-to-create-dynamic-sized-bitmaps
bitmap_t bitmap_create(int n_bits){
    if( n_bits < 0 ) return NULL;
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

int min(int first, int second) {
    return first < second ? first : second;
}

void load_inode(int inumber, struct fs_inode *inode){
    int inode_table_idx = inumber / INODES_PER_BLOCK;
    int inode_block_idx = inumber % INODES_PER_BLOCK;

    union fs_block buffer_block;
    disk_read(inode_table_idx + INODE_TABLE_START_BLOCK, buffer_block.data);
    memcpy(inode, &buffer_block.inodes[inode_block_idx], sizeof(struct fs_inode));
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

int fs_format() {
    // don't format: already mounted
    if (is_mounted) return 0;

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
        for( int block_num = walk_inode_data(0, &inode, NULL), i = 0; block_num > 0; block_num = walk_inode_data(0, NULL, NULL),i++ ){
            if( i == DATA_POINTERS_PER_INODE ){
                printf("\n    indirect block: %d", inode.indirect);
                printf("\n    indirect data blocks:");
            }
            printf(" %d", block_num);
        }
        printf("\n");
    }
}

int fs_mount(){
    if( is_mounted ) return 0;
    union fs_block buffer_block;
    disk_read(0, buffer_block.data);
    if( buffer_block.super.magic != FS_MAGIC ) return 0;

    inode_table_bitmap = bitmap_create(buffer_block.super.ninodes);
    disk_block_bitmap = bitmap_create(buffer_block.super.nblocks);

    // initialize data_region_bitmap: mark superblock and inode table blocks as allocated, rest as free
    //for( int i = 0; i < buffer_block.super.nblocks + 1; i++ ) bitmap_set(disk_block_bitmap, i, !(i < buffer_block.super.ninodeblocks + 1)); // why does this have +1 in for condition?
    for( int i = 0; i < buffer_block.super.nblocks; i++ ) bitmap_set(disk_block_bitmap, i, !(i < buffer_block.super.ninodeblocks + 1)); // +1 to include superblock

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
    is_mounted = true;

    return 1;
}

int fs_create(){
    if( !is_mounted ) return 0;
    // Use bitmap to identify a free inode in the inode table block
    union fs_block block_buffer;
    disk_read(0, block_buffer.data);
    int inumber = 0;
    for (int i = 1; i < block_buffer.super.ninodes; i++) {
        if(bitmap_test(inode_table_bitmap, i)){
            inumber = i;
            break;
        }
    }
    if (!inumber) {
        // No free inodes; return zero
        return 0;
    }
    int block_num = INODE_TABLE_START_BLOCK + (inumber / INODES_PER_BLOCK);
    int block_offset = inumber % INODES_PER_BLOCK;

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

    bitmap_set(inode_table_bitmap, inumber, 0);
    return inumber;
}

int fs_delete( int inumber ){
    if( !is_mounted ) return 0;
    // Validate valid inumber
    union fs_block block_buffer;
    disk_read(0, block_buffer.data);
    if ((inumber < 1) || (inumber >= block_buffer.super.ninodes))
        return 0; // Invalid inumber

    // Read the inode, update the isvald bit, and write back
    int block_num = INODE_TABLE_START_BLOCK + (inumber / INODES_PER_BLOCK);
    int block_offset = inumber % INODES_PER_BLOCK;


    disk_read(block_num, block_buffer.data);
    if (block_buffer.inodes[block_offset].isvalid == 0) {
        // Return 0 -- attempting to delete an inode that's not yet created
        return 0;
    }
    block_buffer.inodes[block_offset].isvalid = 0;
    disk_write(block_num, block_buffer.data);

    // Walk the data blocks, free them, and update the bitmap
    for ( int i = walk_inode_data(0, &block_buffer.inodes[block_offset], NULL); i > 0; i = walk_inode_data(0, NULL, NULL) ) {
        bitmap_set(disk_block_bitmap, i, 1);
    }

    // Update the inode table bitmap
    bitmap_set(inode_table_bitmap, inumber, 1);

    return 1;
}

int fs_getsize( int inumber ){
    if( !is_mounted ) return -1; // Not 0, because 0 can still be a valid inode size

    union fs_block buffer_block;

    // reading superblock to see if inumber is valid
    disk_read(0, buffer_block.data);
    if (inumber <= 0 || inumber >= buffer_block.super.ninodes) return -1;

    struct fs_inode inode_to_check = buffer_block.inodes[inumber];

    // if inode is not valid, then return -1
    if (!inode_to_check.isvalid) return -1;

    return inode_to_check.size;
}

int fs_read( int inumber, char *data, int length, int offset ) {
    if( !is_mounted ) return 0;
    union fs_block buffer_block;
    int bytes_read = 0;

    // read superblock and check validity of inumber
    disk_read(0, buffer_block.data);
    if ( inumber <= 0 || inumber >= buffer_block.super.ninodes ) return 0;

    // read inode's corresponding block from disk
    int block = inumber / INODES_PER_BLOCK;
    disk_read(block + INODE_TABLE_START_BLOCK, buffer_block.data);

    // choose correct inode from block and verify validity
    struct fs_inode inode = buffer_block.inodes[inumber % INODES_PER_BLOCK];
    if ( !inode.isvalid || offset > inode.size )   return 0;

    // read data byte-by-byte from direct pointers unless and until end 
    //int direct_blocks = min(DATA_POINTERS_PER_INODE, 1 + (inode.size / DATA_POINTERS_PER_BLOCK));
    int distance = min(length, inode.size - offset);
    int i = offset / DISK_BLOCK_SIZE;
    int j = offset % DISK_BLOCK_SIZE;
    for ( ; i < DATA_POINTERS_PER_INODE && bytes_read < distance; ++i, j=0 ) {
        disk_read(inode.direct[i], buffer_block.data);
        for ( ; j < DISK_BLOCK_SIZE && bytes_read < distance; ++j ) {
            data[bytes_read++] = buffer_block.data[j];
        }
    }

    // read data from indirect block (if necessary)
    disk_read(inode.indirect, buffer_block.data);
    int indirected_pointers[DATA_POINTERS_PER_BLOCK];
    memcpy(indirected_pointers, buffer_block.pointers, DATA_POINTERS_PER_BLOCK);    
    for ( i -= DATA_POINTERS_PER_INODE; i < DATA_POINTERS_PER_BLOCK && bytes_read < distance; ++i, j=0 ) {
        disk_read(indirected_pointers[i], buffer_block.data);
        for ( ; j < DISK_BLOCK_SIZE && bytes_read < distance; ++j ) {
            data[bytes_read++] = buffer_block.data[j];
        }
    }

    return bytes_read;
}

bool alloc_block( int *pointer, int nblocks ) {
    int k = 0;
    for ( ; k < nblocks && !bitmap_test(disk_block_bitmap, k); ++k );

    if ( k == nblocks )     return false; // out of space cuh

    bitmap_set(disk_block_bitmap, k, 0);
    *pointer = k;

    return true;
}

int fs_write( int inumber, const char *data, int length, int offset ) { // option: make read/write one funtion
    union fs_block buffer_block;
    int bytes_written = 0;

    // read superblock and check validity of inumber
    disk_read(0, buffer_block.data);
    if ( inumber <= 0 || inumber >= buffer_block.super.ninodes )    return 0;

    int nblocks = buffer_block.super.nblocks; // stash nblocks to search bitmap for free blocks

    // read inode's corresponding block from disk
    union fs_block inode_block;
    int block = inumber / INODES_PER_BLOCK;
    disk_read(block + INODE_TABLE_START_BLOCK, inode_block.data);

    // choose correct inode from block and verify validity
    // use address so that we can update this fs_block and write it back later
    struct fs_inode *inode = &(inode_block.inodes[inumber % INODES_PER_BLOCK]);
    if ( !inode->isvalid || offset > inode->size ) return 0; // accept big offset?

    // compute number of pointers already allocated
    int num_pointers = ((inode->size % DISK_BLOCK_SIZE) > 0) + (inode->size / DISK_BLOCK_SIZE);

    // write data byte-by-byte to direct pointers
    int i = offset / DISK_BLOCK_SIZE;
    int j = offset % DISK_BLOCK_SIZE;
    for ( ; i < DATA_POINTERS_PER_INODE && bytes_written < length; ++i, j=0 ) {
        // allocate new block
        if ( i >= num_pointers ) {
            if ( !alloc_block(&(inode->direct[i]), nblocks) ) {
                // return sequence (could be functionized)
                inode->size = -min( -inode->size, -(offset + bytes_written) );
                disk_write( inumber/INODES_PER_BLOCK + INODE_TABLE_START_BLOCK, inode_block.data );
                return bytes_written;
            }
        }

        // descriptive comment here
        disk_read(inode->direct[i], buffer_block.data);
        for ( ; j < DISK_BLOCK_SIZE && bytes_written < length; ++j ) {
            buffer_block.data[j] = data[bytes_written++];
        }
        disk_write(inode->direct[i], buffer_block.data);
    }

    // allocate new indirect block (if necessary)
    union fs_block indirect_block;
    int *indirected_pointers;
    if ( inode->size < DATA_POINTERS_PER_INODE * DISK_BLOCK_SIZE && bytes_written < length ) {
        if ( !alloc_block(&(inode->indirect), nblocks) ) {
            // return sequence
            inode->size = -min( -inode->size, -(offset + bytes_written) );
            disk_write( inumber/INODES_PER_BLOCK + INODE_TABLE_START_BLOCK, inode_block.data );
            return bytes_written;
        }
    } 
    if ( bytes_written < length ) {
        disk_read(inode->indirect, indirect_block.data);
        indirected_pointers = indirect_block.pointers;
    }
    bool indirect_writeback = false;
    // write data to indirect block (if necessary)

    for ( i -= DATA_POINTERS_PER_INODE, num_pointers -= DATA_POINTERS_PER_INODE; i < DATA_POINTERS_PER_BLOCK && bytes_written < length; ++i, j=0 ) {    
        // allocate new block
        if ( i >= num_pointers ) {
            if ( !alloc_block(&(indirected_pointers[i]), nblocks) ) {
                // return sequence
                if (indirect_writeback) {
                    disk_write( inode->indirect, indirect_block.data );
                }
                inode->size = -min( -inode->size, -(offset + bytes_written) );
                disk_write( inumber/INODES_PER_BLOCK + INODE_TABLE_START_BLOCK, inode_block.data );
                return bytes_written;
            }
            indirect_writeback = true;
        }

        disk_read(indirected_pointers[i], buffer_block.data);
        for ( ; j < DISK_BLOCK_SIZE && bytes_written < length; ++j ) {
            buffer_block.data[j] = data[bytes_written++];
        }
        disk_write(indirected_pointers[i], buffer_block.data);
    }

    // return sequence when it spills over to indirect block
    if (indirect_writeback) {
        disk_write( inode->indirect, indirect_block.data );
    }
    // return sequence
    inode->size = -min( -inode->size, -(offset + bytes_written) );
    disk_write( inumber/INODES_PER_BLOCK + INODE_TABLE_START_BLOCK, inode_block.data );
    return bytes_written;
}

// Helper function for fs_debug; Moves a data block to a temporary defragged data region
void move_block(int block_num, int index, union fs_block* defrag_data){
    union fs_block block_buffer;
    disk_read(block_num, block_buffer.data);
    memcpy(defrag_data + index, block_buffer.data, sizeof(union fs_block));
}

int fs_defrag(){

    /*  Create a temporary inode table and data region to hold defragged data */
    union fs_block block_buffer;
    disk_read(0, block_buffer.data);

    int defrag_inumber = 1; // Next available position in defragged inode table
    int defrag_data_index = 0; // Next available position in defragged data region
    int ninodeblocks = block_buffer.super.ninodeblocks;
    int ninodes = block_buffer.super.ninodes;
    int nblocks = block_buffer.super.nblocks;

    union fs_block* defrag_inode_table = calloc(ninodeblocks, sizeof(union fs_block)); // overkill, but sets all inodes in new table to invalid
    union fs_block* defrag_data = malloc(sizeof(union fs_block) * (nblocks - ninodeblocks - 1));
    if( !(defrag_inode_table && defrag_data) ){
        printf("[ERROR] Failed to allocate memory. Exiting...\n");
        abort();
    }

    /* Iterate through inode blocks */
    for (int i = INODE_TABLE_START_BLOCK; i < INODE_TABLE_START_BLOCK + ninodeblocks; i++) {
        disk_read(i, block_buffer.data);
        // Iterate through each inode in the block
        for (int j = 0; j < INODES_PER_BLOCK; j++) {

            // If inode is valid and taken, add to defragged table and defrag data region

            if ( (i == INODE_TABLE_START_BLOCK) && (j == 0 ) ) continue;

            int inumber = (i - INODE_TABLE_START_BLOCK) * INODES_PER_BLOCK + j;
            if ( !bitmap_test(inode_table_bitmap, inumber) ) {

                /*  Add to defragged table */
                int defrag_inode_offset = defrag_inumber % INODES_PER_BLOCK;
                int inode_offset = inumber % INODES_PER_BLOCK; // = j
                //union fs_block *defrag_block = defrag_inode_table + i - INODE_TABLE_START_BLOCK; // should this be defrag_inumber / INODES_PER_BLOCK?
                union fs_block *defrag_block = defrag_inode_table + defrag_inumber / INODES_PER_BLOCK;

                defrag_block->inodes[defrag_inode_offset] = block_buffer.inodes[inode_offset];
                defrag_inumber++; // Increment inumber to be next available one

                /*  Defrag  data pointers */

                // Loop through indirect block's pointers and direct pointers separately
                //int num_blocks = (defrag_block->inodes[defrag_inode_offset].size + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE;
                int num_blocks = (block_buffer.inodes[inode_offset].size + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE;
                int direct_blocks = 0;
                int indirect_blocks = 0;
                if (num_blocks <= 5) {
                    direct_blocks = num_blocks;
                } else {
                    direct_blocks = 5;
                    indirect_blocks = num_blocks - 5;
                }

                // Direct pointer blocks
                for (int k = 0; k < direct_blocks; k++) {
                    // Move to temp data region
                    //int block_num = defrag_block->inodes[defrag_inode_offset].direct[k];
                    int block_num = block_buffer.inodes[inode_offset].direct[k];
                    move_block(block_num, defrag_data_index, defrag_data);

                    // Update the inode direct pointers and defrag data index
                    defrag_block->inodes[defrag_inode_offset].direct[k] = defrag_data_index + INODE_TABLE_START_BLOCK + ninodeblocks;
                    defrag_data_index++;
                }

                // Indirect pointer blocks
                if (indirect_blocks)
                {
                    // Read the indirect block and create new indirect block
                    union fs_block defrag_indirect_block;
                    //union fs_block current_indirect_block;
                    //disk_read(block_buffer.inodes[inode_offset].indirect, current_indirect_block.data);
                    disk_read(block_buffer.inodes[inode_offset].indirect, defrag_indirect_block.data);
                    // disk_read(defrag_block->inodes[defrag_inode_offset].indirect, defrag_indirect_block.data);

                    // Direct pointers inside indirect block
                    for (int k = 0; k < indirect_blocks; k++) {
                        int block_num = defrag_indirect_block.pointers[k];
                        move_block(block_num, defrag_data_index, defrag_data);

                        // Update the inode direct pointers and defrag data index
                        defrag_indirect_block.pointers[k] = defrag_data_index + INODE_TABLE_START_BLOCK + ninodeblocks;
                        defrag_data_index++;
                    }

                    // Move the indirect block itself into defrag data
                    memcpy(defrag_data + defrag_data_index, defrag_indirect_block.data, sizeof(union fs_block));
                    defrag_block->inodes[defrag_inode_offset].indirect = defrag_data_index + INODE_TABLE_START_BLOCK + ninodeblocks;
                    defrag_data_index++;
                }
            }
        }



    }

    /* Modify the inode bitmap */
    for (int i = 1; i < defrag_inumber; i++) {
        //inode_table_bitmap[i / 8] ^= 1 << (i % 8);
        bitmap_set(inode_table_bitmap, i, 0);
    }
    for (int i = defrag_inumber; i < ninodes; i++) {
        bitmap_set(inode_table_bitmap, i, 1);
    }

    /* Write inode table to the disk */
    for (int i = 0; i < ninodeblocks; i++) {
        disk_write(i+INODE_TABLE_START_BLOCK , (defrag_inode_table + i)->data);
    }

    /* Modify the data bitmap */
    for (int i = 0; i < defrag_data_index + INODE_TABLE_START_BLOCK + ninodeblocks; i++) {
        //disk_block_bitmap[i / 8] ^= 1 << (i % 8);
        bitmap_set(disk_block_bitmap, i, 0);
    }
    for (int i = defrag_data_index + INODE_TABLE_START_BLOCK + ninodeblocks; i < nblocks - ninodeblocks - 1; i++) {
        bitmap_set(disk_block_bitmap, i, 1);
    }

    /* Write data table to the disk */
    for (int i = 0; i < nblocks - ninodeblocks - 1; i++) {
        disk_write(i + INODE_TABLE_START_BLOCK + ninodeblocks, (defrag_data + i)->data);
    }

    /* Free allocated structures */
    free(defrag_inode_table);
    free(defrag_data);

    return 1;
}
