#ifndef DISK_H
#define DISK_H

#define DISK_BLOCK_SIZE 4096

int  disk_init( const char *filename, int nblocks );
int  disk_size();
void disk_read( int blocknum, char *data );
void disk_write( int blocknum, const char *data );
void disk_close();


#endif
