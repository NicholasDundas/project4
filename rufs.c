/*
 *  Copyright (C) 2024 CS416/CS518 Rutgers CS
 *	Tiny File System
 *	File:	rufs.c
 *
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>

#include "block.h"
#include "rufs.h"

char diskfile_path[PATH_MAX];

// Declare your in-memory data structures here
struct superblock sb; // stores superblock metadata read during init
bitmap_t bmp; // bitmap of size BLOCK_SIZE used with bio_read/write operations
/* 
 * Get available inode number from bitmap
 */
int get_avail_ino() {

	// Step 1: Read inode bitmap from disk
	bio_read(sb.i_bitmap_blk,&bmp);
	int ino = 0; // stores next availaible inode number, if none exist it is 0
	// Step 2: Traverse inode bitmap to find an available slot
	for(int i = 0; i < sb.max_inum; i++)
		if(get_bitmap(bmp,i)) {
			ino = i;
		}
	// Step 3: Update inode bitmap and write to disk 
	if(ino != 0) {
		set_bitmap(bmp,ino);		
		bio_write(sb.i_bitmap_blk,bmp);
	}
	return ino;
}

/* 
 * Get available data block number from bitmap
 */
int get_avail_blkno() {

	// Step 1: Read data block bitmap from disk
	bio_read(sb.d_bitmap_blk,&bmp);
	int blkno = 0; // stores next availaible data block, if none exist it is 0	
	// Step 2: Traverse data block bitmap to find an available slot
	for(int i = 0; i < sb.max_dnum; i++)
		if(get_bitmap(bmp,i)) {
			blkno = i;
		}
	// Step 3: Update data block bitmap and write to disk 
	if(blkno != 0) {
		set_bitmap(bmp,blkno);
		bio_write(sb.i_bitmap_blk,bmp);	
	}
	return blkno;
}

/* 
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {
	if(ino > sb.max_inum)
		return -1;
  // Step 1: Get the inode's on-disk block number
	const unsigned int blkno = (ino * sizeof(struct inode)) / BLOCK_SIZE;
  // Step 2: Get offset of the inode in the inode on-disk block
	const unsigned int offset = (ino * sizeof(struct inode)) % BLOCK_SIZE;
  // Step 3: Read the block from disk and then copy into inode structure
	bio_read(blkno + sb.i_start_blk,bmp);
	memcpy(inode,bmp[offset],sizeof(struct inode));
	return 0;
}

int writei(uint16_t ino, struct inode *inode) {
	if(ino > sb.max_inum)
		return -1;
	// Step 1: Get the block number where this inode resides on disk
	const unsigned int blkno = (ino * sizeof(struct inode)) / BLOCK_SIZE;
	// Step 2: Get the offset in the block where this inode resides on disk
	const unsigned int offset = (ino * sizeof(struct inode)) % BLOCK_SIZE;
	// Step 3: Write inode to disk 
	bio_read(blkno + sb.i_start_blk,bmp);
	memcpy(&bmp[offset],inode,sizeof(struct inode));
	bio_write(blkno + sb.i_start_blk,bmp);
	return 0;
}


/* 
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {

	// Step 1: Call readi() to get the inode using ino (inode number of current directory)
	struct inode dir_inode;
	if (readi(ino, &dir_inode) != 0)
	{
		return -ENOENT; // error: failed to read inode
	}

	
	// iterate through all direct pointers
	for (int i = 0; i < 16; i++)
	{
		int data_block_idx = dir_inode.direct_ptr[i];
		if (data_block_idx == 0)
		{
			break; //no data blocks left to search
		}

		// Step 2: Read directory's data block and check each directory entry.
		char block_data[BLOCK_SIZE];
		if (bio_read(data_block_idx, block_data) <= 0)
		{
			return -EIO; // error: failed to read directory data block
		}

		// iterate through directory entries in the data block
		int offset = 0;
		while (offset < BLOCK_SIZE)
		{
			struct dirent *dir_entry = (struct dirent *)(block_data + offset);
			if (dir_entry->valid && strncmp(dir_entry->name, fname, name_len) == 0)
			{
				
				memcpy(dirent, dir_entry, sizeof(struct dirent));
				return 0; // return success, found a matching directory entry
			}
			offset += sizeof(struct dirent);
		}
	}

	return -ENOENT; // if code reaches here, no directory/file was found so return error
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
	
	// Step 2: Check if fname (directory name) is already used in other entries

	// Step 3: Add directory entry in dir_inode's data block and write to disk

	// Allocate a new data block for this directory if it does not exist

	// Update directory inode

	// Write directory entry

	return 0;
}

// Required for 518
int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode
	
	// Step 2: Check if fname exist

	// Step 3: If exist, then remove it from dir_inode's data block and write to disk

	return 0;
}

/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way

	return 0;
}

/* 
 * Make file system
 */
int rufs_mkfs() {

	// Call dev_init() to initialize (Create) Diskfile
	dev_init(diskfile_path);
	// write superblock information
	const unsigned int inum_block_count = (MAX_INUM * sizeof(struct inode)) / BLOCK_SIZE; // num of blocks needed for inodes
	struct superblock new_sb = { 
	.magic_num = MAGIC_NUM, 
	.max_inum = MAX_INUM,
	.max_dnum = MAX_DNUM - (inum_block_count + 3), // 3 represents the bitmaps and superblock stored before inodes
	.i_bitmap_blk = 1, //0 is superblock, followed by inode bitmap
	.d_start_blk = 2, //then datablock bitmap
	.i_start_blk = 3, //then the inodes themselves
	.d_start_blk = inum_block_count + 3 }; //finally by the datablocks

	sb = new_sb;
	memcpy(&sb,bmp,sizeof(struct superblock));
	bio_write(0,&sb);

	// initialize inode bitmap
	memset(bmp,0,BLOCK_SIZE);
	bio_write(sb.i_bitmap_blk,bmp);
	// initialize data block bitmap
	for(int i = 0; i < inum_block_count + sb.i_start_blk; i++)	
		set_bitmap(bmp,i); //Mark these data blocks as reserved for filesystem metadata (superblock, bitmaps, inodes)
	bio_write(sb.d_bitmap_blk,bmp);
	// update bitmap information for root directory

	// update inode for root directory
	return 0;
}


/* 
 * FUSE file operations
 */
static void *rufs_init(struct fuse_conn_info *conn) {
	bmp = calloc(0,BLOCK_SIZE);
	// Step 1a: If disk file is not found, call mkfs
	if(dev_open(diskfile_path) != 0)
		rufs_mkfs();
  // Step 1b: If disk file is found, just initialize in-memory data structures
  // and read superblock from disk
	else {
		bio_read(0,bmp);
		memcpy(&sb,bmp,sizeof(struct superblock));
	}
	return NULL;
}

static void rufs_destroy(void *userdata) {

	// Step 1: De-allocate in-memory data structures
	free(bmp);
	// Step 2: Close diskfile
	dev_close();
}

static int rufs_getattr(const char *path, struct stat *stbuf) {

	// Step 1: call get_node_by_path() to get inode from path

	// Step 2: fill attribute of file into stbuf from inode

		stbuf->st_mode   = S_ISDIR | 0755;
		stbuf->st_nlink  = 2;
		time(&stbuf->st_mtime);

	return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: If not find, return -1

    return 0;
}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: Read directory entries from its data blocks, and copy them to filler

	return 0;
}


static int rufs_mkdir(const char *path, mode_t mode) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of parent directory

	// Step 3: Call get_avail_ino() to get an available inode number

	// Step 4: Call dir_add() to add directory entry of target directory to parent directory

	// Step 5: Update inode for target directory

	// Step 6: Call writei() to write inode to disk
	

	return 0;
}

// Required for 518
static int rufs_rmdir(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of target directory

	// Step 3: Clear data block bitmap of target directory

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory

	return 0;
}

static int rufs_releasedir(const char *path, struct fuse_file_info *fi) {
// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of parent directory

	// Step 3: Call get_avail_ino() to get an available inode number

	// Step 4: Call dir_add() to add directory entry of target file to parent directory

	// Step 5: Update inode for target file

	// Step 6: Call writei() to write inode to disk

	return 0;
}

static int rufs_open(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: If not find, return -1

	return 0;
}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: copy the correct amount of data from offset to buffer

	// Note: this function should return the amount of bytes you copied to buffer
	return 0;
}

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: Write the correct amount of data from offset to disk

	// Step 4: Update the inode info and write it to disk

	// Note: this function should return the amount of bytes you write to disk
	return size;
}

// Required for 518

static int rufs_unlink(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of target file

	// Step 3: Clear data block bitmap of target file

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target file in its parent directory

	return 0;
}

static int rufs_truncate(const char *path, off_t size) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_release(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_flush(const char * path, struct fuse_file_info * fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_utimens(const char *path, const struct timespec tv[2]) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}


static struct fuse_operations rufs_ope = {
	.init		= rufs_init,
	.destroy	= rufs_destroy,

	.getattr	= rufs_getattr,
	.readdir	= rufs_readdir,
	.opendir	= rufs_opendir,
	.releasedir	= rufs_releasedir,
	.mkdir		= rufs_mkdir,
	.rmdir		= rufs_rmdir,

	.create		= rufs_create,
	.open		= rufs_open,
	.read 		= rufs_read,
	.write		= rufs_write,
	.unlink		= rufs_unlink,

	.truncate   = rufs_truncate,
	.flush      = rufs_flush,
	.utimens    = rufs_utimens,
	.release	= rufs_release
};


int main(int argc, char *argv[]) {
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);

	return fuse_stat;
}

