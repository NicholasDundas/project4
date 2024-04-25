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
 * Returns -1 if none found
 */
int get_avail_ino() {

	// Step 1: Read inode bitmap from disk
	if(bio_read(sb.i_bitmap_blk,bmp) <= 0)
		return -2;
	int ino = -1; // stores next availaible inode number, if none exist it is 0
	// Step 2: Traverse inode bitmap to find an available slot
	for(int i = 0; i < sb.max_inum; i++)
		if(!get_bitmap(bmp,i)) {
			ino = i;
			break;
		}
	// Step 3: Update inode bitmap and write to disk 
	if(ino != -1) {
		set_bitmap(bmp,ino);		
		if(bio_write(sb.i_bitmap_blk,bmp) <= 0)
			return -2;
	}
	return ino;
}

/* 
 * Get available data block number from bitmap
 * Returns -1 if none found
 */
int get_avail_blkno() {

	// Step 1: Read data block bitmap from disk
	if(bio_read(sb.d_bitmap_blk,bmp) <= 0)
		return -2;
	int blkno = -1; // stores next availaible data block, if none exist it is 0	
	// Step 2: Traverse data block bitmap to find an available slot
	for(int i = 0; i < sb.max_dnum; i++)
		if(!get_bitmap(bmp,i)) {
			blkno = i;
			break;
		}
	// Step 3: Update data block bitmap and write to disk 
	if(blkno != -1) {
		set_bitmap(bmp,blkno);
		if(bio_write(sb.d_bitmap_blk,bmp) <= 0)
			return -2;
	}
	return blkno;
}

/* 
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {
	if(ino > sb.max_inum)
		return 1;
  // Step 1: Get the inode's on-disk block number
	const unsigned int blkno = (ino * sizeof(struct inode)) / BLOCK_SIZE + sb.i_start_blk;
  // Step 2: Get offset of the inode in the inode on-disk block
	const unsigned int offset = (ino * sizeof(struct inode)) % BLOCK_SIZE;
  // Step 3: Read the block from disk and then copy into inode structure
	if(bio_read(blkno,bmp) <= 0)
		return EIO;
	memcpy(inode,bmp + offset,sizeof(struct inode));
	return 0;
}

int writei(uint16_t ino, struct inode *inode) {
	if(ino > sb.max_inum)
		return 1;
	// Step 1: Get the block number where this inode resides on disk
	const unsigned int blkno = (ino * sizeof(struct inode)) / BLOCK_SIZE + sb.i_start_blk;
	// Step 2: Get the offset in the block where this inode resides on disk
	const unsigned int offset = (ino * sizeof(struct inode)) % BLOCK_SIZE;
	// Step 3: Write inode to disk 
	if(bio_read(blkno,bmp) <= 0)
		return EIO;
	memcpy(bmp + offset,inode,sizeof(struct inode));
	if(bio_write(blkno,bmp) <= 0)
		return EIO;
	return 0;
}


/* 
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {

	// Step 1: Call readi() to get the inode using ino (inode number of current directory)
	struct inode dir_inode;
	if (readi(ino, &dir_inode) != 0)
		return EIO; // error: failed to read inode
	
	// iterate through all direct pointers
	for (int i = 0; i < 16; i++) {
		int data_block_idx = dir_inode.direct_ptr[i];
		if (data_block_idx == 0)
			continue; //empty direct_ptr, do not search

		// Step 2: Read directory's data block and check each directory entry.
		if (bio_read(data_block_idx, bmp) <= 0)
			return EIO; // error: failed to read directory data block

		// iterate through directory entries in the data block
		int offset = 0;
		while (offset < BLOCK_SIZE) {
			struct dirent *dir_entry = (struct dirent *)(bmp + offset);
			if (dir_entry->valid && strncmp(dir_entry->name, fname, name_len) == 0) {
				memcpy(dirent, dir_entry, sizeof(struct dirent));
				return 0; // return success, found a matching directory entry
			}
			offset += sizeof(struct dirent);
		}
	}
	return ENOENT; // if code reaches here, no directory/file was found so return error
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {
	int need_alloc = 0; //whether we need to allocate a new block
	int empty_dptr = -1; //first empty direct entry block
	int empty_dir_ent = -1; //first empty direct entry
	struct dirent *dir_entry;
	for (int i = 0; i < 16; i++) {
		int data_block_idx = dir_inode.direct_ptr[i];
		if (data_block_idx == 0) {
			if(empty_dir_ent == -1 && empty_dptr == -1)  {
				empty_dptr = i;
				need_alloc = 1;
			}
			continue; //empty direct_ptr, do not search
		}
		// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
		if (bio_read(data_block_idx, bmp) <= 0)
			return EIO; // error: failed to read directory data block

		// iterate through directory entries in the data block
		int offset = 0;
		while (offset < BLOCK_SIZE) {
			dir_entry = (struct dirent *)(bmp + offset);
			// Step 2: Check if fname (directory name) is already used in other entries
			if (!dir_entry->valid && empty_dir_ent == -1) { //empty directory entry found, save for later...
				empty_dptr = i;
				empty_dir_ent = offset;
				need_alloc = 0; //we dont need to allocate any direct pointers (implied in a block)
			}	
			if (dir_entry->valid && strncmp(dir_entry->name, fname, name_len) == 0) { //found duplicate, copy over it
				empty_dptr = i;
				empty_dir_ent = offset;
				need_alloc = 0;
				goto WRITE_DIRENT;
			}
			offset += sizeof(struct dirent);
		}
	}
	if(empty_dir_ent == -1 && empty_dptr == -1)
		return ENOMEM; //no place to add dirent
	// Step 3: Add directory entry in dir_inode's data block and write to disk
	// Allocate a new data block for this directory if it does not exist
WRITE_DIRENT:
	if(need_alloc) { //Allocate new datablock for directory
		dir_inode.direct_ptr[empty_dptr] = get_avail_blkno();
		memset(bmp,0,BLOCK_SIZE);
		empty_dptr = dir_inode.direct_ptr[empty_dptr];
		empty_dir_ent = 0;
	} else {
		bio_read(dir_inode.direct_ptr[empty_dptr],bmp);
	}
	dir_entry = (struct dirent *)(bmp + empty_dir_ent);
	dir_entry->ino = f_ino;
	dir_entry->len = name_len;
	dir_entry->valid = 1;
	strncpy(dir_entry->name,fname,name_len);
	bio_write(dir_inode.direct_ptr[empty_dptr],bmp); // Write temp block to file
	// Update directory inode
	dir_inode.vstat.st_mtime = time(NULL);
	writei(dir_inode.ino,&dir_inode);
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
	
	// tokenize the path with "/" delimiter
    char *token;
    char *path_copy = malloc(strlen(path) + 1);
	if(!path_copy)
		exit(EXIT_FAILURE);
    strcpy(path_copy, path);
    token = strtok(path_copy, "/");

	// traverse the path
    while (token != NULL) {
        // find the directory entry for the current token
        struct dirent dirent;
        int dir_find_result = dir_find(ino, token, strlen(token), &dirent);
        
        // check if the directory entry was found
        if (dir_find_result != 0) {
            free(path_copy);
			return ENOENT;
        }
        
        // read the inode corresponding to the directory entry
        if (readi(dirent.ino, inode) != 0) {
			free(path_copy);
			return EIO;
        }
        
        // move to the next token
        token = strtok(NULL, "/");
        
        // if token is not NULL and inode is not a directory, return error.
		//  this means that we aren't at the end of the path(token != null), but the inode 
		//  is not a directory, so we can't traverse further
        if (token != NULL && !S_ISDIR(inode->vstat.st_mode)) { //S_ISDIR vs S_IFDIR?
            // Next token is not NULL but current inode is not a directory
            free(path_copy);
			return ENOTDIR;
        }
        
        // update ino to the inode of the current directory entry
        ino = dirent.ino;
    }
    
    // read the inode of the terminal point to struct inode *inode
    if (readi(ino, inode) != 0) {
		free(path_copy);
        return EIO; //return error if unsuccessful
    }
	free(path_copy);
	return 0;
}


/* 
 * Make file system
 */
int rufs_mkfs() {
	printf("mkfs called\n");
	// Call dev_init() to initialize (Create) Diskfile
	dev_init(diskfile_path);
	// write superblock information
	const unsigned int inum_block_count = (MAX_INUM * sizeof(struct inode)) / BLOCK_SIZE; // num of blocks needed for inodes
	struct superblock new_sb = { 
		.magic_num = MAGIC_NUM, 
		.max_inum = MAX_INUM,
		.max_dnum = MAX_DNUM - (inum_block_count + 3), // 3 represents the bitmaps and superblock stored before inodes
		.i_bitmap_blk = 1, //0 is superblock, followed by inode bitmap
		.d_bitmap_blk = 2, //then datablock bitmap
		.i_start_blk = 3, //then the inodes themselves
		.d_start_blk = inum_block_count + 3 //finally by the datablocks
	}; 
	printf("creating superblock\n");
	sb = new_sb;
	*(struct superblock*)bmp = new_sb;
	if(bio_write(0,bmp) <= 0)
		return 1;
	printf("superblock written\n");
	// initialize inode bitmap
	memset(bmp,0,BLOCK_SIZE);
	if(bio_write(sb.i_bitmap_blk,bmp) <= 0)
		return 1;
	printf("inode bitmap written\n");
	// initialize data block bitmap
	for(int i = 0; i < inum_block_count + sb.i_start_blk; i++)	
		set_bitmap(bmp,i); //Mark these data blocks as reserved for filesystem metadata (superblock, bitmaps, inodes)
	if(bio_write(sb.d_bitmap_blk,bmp) <= 0)
		return 1;
	printf("datablock bitmap written\n");
	// update inode for root directory
	struct inode root = { 0 };
	root.ino = get_avail_ino();
	root.direct_ptr[0] = get_avail_blkno();
	printf("root.ino == %d, root.direct_ptr[0] == %d\n",root.ino,root.direct_ptr[0]);
	if(root.ino == -1 || root.direct_ptr[0] == -1)
		return 1;
	root.type = S_IFDIR;
	root.vstat.st_mode = S_IFDIR | 0755;
	root.vstat.st_mtime = time(NULL);
	root.size = BLOCK_SIZE;
	root.valid = 1;
	int err = writei(root.ino,&root);
	if(err)
		return err;
	printf("inode root directory created\n");
	memset(bmp,0,BLOCK_SIZE);
	if(bio_write(root.direct_ptr[0],bmp) <= 0)
		return 1;
	printf("inode root directory datablock created\n");
	return 0;
}


/* 
 * FUSE file operations
 */
static void *rufs_init(struct fuse_conn_info *conn) {
	printf("rufs init called\n");
	bmp = calloc(0,BLOCK_SIZE);
	if(!bmp)
		exit(EXIT_FAILURE);
	printf("bitmap allocated for block io\n"); 
	// Step 1a: If disk file is not found, call mkfs
	if(dev_open(diskfile_path) != 0) {
		printf("disk file not found, creating\n");
		int err = rufs_mkfs();
		if(err)
			exit(err); //error making file system, exit
	} else { 
		printf("disk file found, reading\n");
		// Step 1b: If disk file is found, just initialize in-memory data structures
		if(bio_read(0,bmp) <= 0) // and read superblock from disk
			exit(EXIT_FAILURE); // error reading, just EXIT
		sb = *(struct superblock*)bmp;
		printf("superblock read");
	}
	return NULL;
}

static void rufs_destroy(void *userdata) {
	printf("rufs destroy called\n");
	// Step 1: De-allocate in-memory data structures
	free(bmp);
	// Step 2: Close diskfile
	printf("closing diskfile\n");
	dev_close();
	printf("diskfile closed\n");
}

static int rufs_getattr(const char *path, struct stat *stbuf) {

	// Step 1: call get_node_by_path() to get inode from path

	// Step 2: fill attribute of file into stbuf from inode

		stbuf->st_mode   = S_IFDIR | 0755;
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
	printf("calling fuse main\n");
	fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);
	printf("fuse main done\n");
	return fuse_stat;
}
