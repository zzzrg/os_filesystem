#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "disk_emu.h"
#include "sfs_api.h"

#define BLOCK_SIZE 1024
#define SFS_SIZE 4000
#define MAXFILES 100
#define MAXFILESIZE BLOCK_SIZE * ((BLOCK_SIZE/sizeof(int)) + 12)
// #define MAXFILENAME 16   /* Assume at most 16 characters (16.3) */
#define DISKNAME "CCdisk.disk"

static bool free_bitmap[SFS_SIZE];
// for looping through nextfile in the directory
static int curr_file = 0;
// global indirect block to be used across all methods

int find_avail_block(){
    int i;
    for(i=0; i<SFS_SIZE; i++){
        if(!free_bitmap[i]){
            // set it to taken
            free_bitmap[i] = true;
            // return the index
            return i;
        }
    }
    // out of blocks
    return -1;
}

typedef struct Inode{
    int mode;
    int link_count;
    int uid;
    int gid;
    int size;
    int block_pointers[12];
    int indirect;
} Inode;

// initialize an inode table
Inode inodes[MAXFILES];

void init_inode_table(){
    int i, j;
    for(i=0; i<MAXFILES; i++){
        Inode *inode = &inodes[i];
        inode->link_count = 0;
        inode->size = 0;
        for(j=0; j<12; j++){
            // unused set to -1
            inode->block_pointers[j] = -1;
        }
        inode->indirect = -1;
    }
}

typedef struct DirEntry{
    int inode_index;
    char filename[MAXFILENAME+1];
} DirEntry;

// initialize a directory table
DirEntry directory[MAXFILES];

void init_root_directory(){
    int i, j;
    for(i=0; i<MAXFILES; i++){
        // -1 means unused slot
        DirEntry *entry = &directory[i];
        entry->inode_index = -1;
        for(j=0; j<MAXFILENAME; j++){
            entry->filename[j] = '\0';
        }
    }
}

typedef struct Superblock{
    int magic_num;
    int block_size;
    int sys_size;
    int inode_table_blocks;
    int root_dir;
} Superblock;

Superblock sb;

typedef struct FileDesc{
    int inode_num;
    int read_pt;
    int write_pt;
} FileDesc;

FileDesc filedesc_table[MAXFILES];
void init_fd_table(){
    int i;
    for(i=0; i<MAXFILES; i++){
        // -1 means unused slot
        FileDesc *fd = &filedesc_table[i];
        fd->inode_num = -1;
        fd->read_pt = 0;
        fd->write_pt = 0;
    }
}

void mksfs(int fresh){
    if(fresh){
        init_fresh_disk(DISKNAME, BLOCK_SIZE, SFS_SIZE);
        // initialize the super block
        init_fd_table();
        init_root_directory();
        init_inode_table();

        int inode_blocks_needed = sizeof(inodes)/BLOCK_SIZE;
        if(sizeof(inodes)%BLOCK_SIZE != 0){
            // add one more block for overflow
            inode_blocks_needed++;
        }


        sb.magic_num = (0xACBD0005);
        sb.block_size = BLOCK_SIZE;
        sb.sys_size = SFS_SIZE;
        sb.inode_table_blocks = inode_blocks_needed; 
        sb.root_dir = 0;
        // write superblock to disk
        // allocate block
        free_bitmap[0] = true;
        write_blocks(0, 1, &sb);
        
        // write inode blocks to disk
        int i;
        for(i=0; i<inode_blocks_needed; i++){
            int avail_block = find_avail_block();
            free_bitmap[avail_block] = true;
        }
        write_blocks(1, inode_blocks_needed, &inodes);

        // write directory in blocks and write it to disk
        int dir_blocks_needed = sizeof(directory)/BLOCK_SIZE;
        if(sizeof(directory)%BLOCK_SIZE != 0){
            // add one more block for overflow
            dir_blocks_needed++;
        }

        for(i=0; i<dir_blocks_needed; i++){
            int avail_block = find_avail_block();
            free_bitmap[avail_block] = true;
        }

        write_blocks(inode_blocks_needed+1, dir_blocks_needed, &directory);

        // write free bitmap
        write_blocks(3995, 4, &free_bitmap);

    }else{
        init_disk(DISKNAME, BLOCK_SIZE, SFS_SIZE);
        read_blocks(0, 1, &sb);

        int inode_blocks_needed = sizeof(inodes)/BLOCK_SIZE;
        if(sizeof(inodes)%BLOCK_SIZE != 0){
            // add one more block for overflow
            inode_blocks_needed++;
        }

        read_blocks(1, inode_blocks_needed, &inodes);

        int dir_blocks_needed = sizeof(directory)/BLOCK_SIZE;
        if(sizeof(directory)%BLOCK_SIZE != 0){
            // add one more block for overflow
            dir_blocks_needed++;
        }
        read_blocks(inode_blocks_needed+1, dir_blocks_needed, &directory);
        read_blocks(3995, 4, &free_bitmap);
        init_fd_table();
    }
}

int sfs_getnextfilename(char *fname){
    // directory file looper 
    // returns current position in the directory, returns 0 when all files have been returned once
    while(1){
        DirEntry *entry = &directory[curr_file];
        if(entry->filename != NULL){
            // copy the filename and return current index of file in directory
            memcpy(fname, entry->filename, MAXFILENAME);
            return curr_file;
        } 
        // increment the current file index, while checking if we have gone through the entire directory
        curr_file = (curr_file+1)%100;
        if(curr_file == 0){
            break;
        }
    }
    return 0;
}

int sfs_getfilesize(const char* path){
    // returns size of a file
    int i;
    // go through the entire directory to see if the file is present
    for(i=0; i<MAXFILES; i++){
        DirEntry *entry = &directory[i];
        // if filename of the directory entry corresponds to the path
        if(entry->inode_index != -1){
            if(strcmp(entry->filename, path) == 0){
                // return size
                return inodes[entry->inode_index].size;
            }
        }

    }
        
    // else the file doesn't exist
    return -1;
}


int sfs_fopen(char *name){
    // preliminary check to see if name respects restrictions
    if(strlen(name) > 16){
        return -1;
    }
    int i;
    for(i=0; i<100; i++){
        // first find the file in the directory if it exists
        DirEntry *entry = &directory[i];
        if(entry->inode_index != -1){
            if(strcmp(entry->filename, name) == 0){
                // once it is found, check if the file is already open in file descriptor table
                // to do this we loop thru the filedesc table to see if there's a fd corresponding to the file
                int j;
                for(j=0; j<100; j++){
                    FileDesc *fd = &filedesc_table[j];
                    if(fd->inode_num == entry->inode_index){
                        return j;
                    }
                }
                // otherwise insert a new entry in the filedescriptor table
                // we find an empty slot
                for(j=0; j<100; j++){
                    FileDesc *fd = &filedesc_table[j];
                    if(fd->inode_num == -1){
                        fd->inode_num = entry->inode_index;
                        fd->read_pt=0;
                        fd->write_pt=inodes[entry->inode_index].size;

                        return j;
                    }
                }
            }
        }
    }

    // if we don't find the file in the directory, we must create it
    // 1. allocate and initialize inode, we find where the link count is 0 i.e. not linked to any file
    for(i=0; i<MAXFILES; i++){
        Inode *node = &inodes[i];
        if(node->link_count == 0){
            node->link_count++;
            node->mode = 0;
            // 2. initialize a directory entry in the directory table
            int j;
            for(j=0; j<MAXFILES; j++){
                // slot is available if inode is un-initialized (i.e. 0)
                DirEntry *entry = &directory[i];
                if(entry->inode_index == -1){
                    // assign inode and filename to the directory entry
                    entry->inode_index = i;
                    memcpy(entry->filename, name, MAXFILENAME);
                    // 3. insert filedescriptor into filedescriptor table
                    int k;
                    for(k=0; k<MAXFILES; k++){
                        FileDesc *fd = &filedesc_table[k];
                        // we find an empty slot
                        if(fd->inode_num == -1){
                            fd->inode_num = i;
                            // set the pointers appropriately
                            fd->read_pt=0;
                            fd->write_pt=node->size;
                            // printf("%d for the file %s\n", k, name);
                                // gotta write to disk
                            int inode_blocks_needed = sizeof(inodes)/BLOCK_SIZE;
                            if(sizeof(inodes)%BLOCK_SIZE != 0){
                                // add one more block for overflow
                                inode_blocks_needed++;
                            }
                            write_blocks(1, inode_blocks_needed, &inodes);

                            int dir_blocks_needed = sizeof(directory)/BLOCK_SIZE;
                            if(sizeof(directory)%BLOCK_SIZE != 0){
                                // add one more block for overflow
                                dir_blocks_needed++;
                            }
                            write_blocks(inode_blocks_needed+1, dir_blocks_needed, &directory);

                            write_blocks(3995, 4, &free_bitmap);
                            return k;
                        }
                    }
                }
            }
        }
    }
    // error opening a file
    return -1;
}

int sfs_fclose(int fileID){
    // closes the file with the file descriptor fileID
    FileDesc *fd = &filedesc_table[fileID];
    if(fd->inode_num == -1){
        // error finding the file descriptor to close i.e. file is not opened
        return -1;
    }

    fd->inode_num = -1;
    fd->read_pt = 0;
    fd->write_pt = 0;

    return 0;
}

int sfs_frseek(int fileID,int loc){
    // find the file descriptor using fileID
    FileDesc *fd = &filedesc_table[fileID];
    // check if the file descriptor is closed
    if(fd->inode_num == -1){
        return -1;
    }
    // check if the seek respects the size
    Inode *node = &inodes[fd->inode_num];
    if(node->size < loc){
        // trying to seek further than the size of the file
        return -1;
    }

    fd->read_pt = loc;
    return 0;
}

int sfs_fwseek(int fileID, int loc){
    // find the file descriptor using fileID
    FileDesc *fd = &filedesc_table[fileID];
    // check if the file descriptor is closed
    if(fd->inode_num == -1){
        return -1;
    }
    // check if the seek respects the size
    Inode *node = &inodes[fd->inode_num];
    if(node->size < loc){
        // trying to seek further than the size of the file
        return -1;
    }
    fd->write_pt = loc;
    return 0;
}

int sfs_fwrite(int fileID, const char *buf, int length){
    int i;
    // find the file descriptor
    FileDesc *fd = &filedesc_table[fileID];
    if(fd->inode_num == -1){
        // trying to write to a closed file no bueno
        return 0;
    }
    int bytes_towrite = length;
    // retrieve the inode associated to this file
    Inode *f_inode = &inodes[fd->inode_num];
    // figure out how many blocks are being used for this file
    int current_blocks = f_inode->size/1024;
    if(f_inode->size%1024!=0){
        current_blocks++;
    }
    // figure out if we need more blocks
    int updated_bytes = fd->write_pt + length;
    if(updated_bytes > MAXFILESIZE){
        updated_bytes = MAXFILESIZE;
        bytes_towrite = MAXFILESIZE - fd->write_pt;
    }

    int total_blocks_towrite = updated_bytes/BLOCK_SIZE;
    if(updated_bytes%BLOCK_SIZE != 0){
        // need one more block for overflow
        total_blocks_towrite++;
    }
    // additional blocks needed
    int additional_blocks = total_blocks_towrite - current_blocks;

    // if this file has an indirect block already, retrieve the indirect block from the disk
    int indirect_block_pts[BLOCK_SIZE];

    if(current_blocks > 12){
        read_blocks(f_inode->indirect, 1, indirect_block_pts);
        // memcpy(&indirect_block_pts, indir_block, BLOCK_SIZE);

    }else if(total_blocks_towrite > 12){
        // if this file didn't have indirect blocks, but by writing we need one, allocate a block for indirect
        int indir_pt = find_avail_block();
        if(indir_pt < 0){
            return 0;
        }
        f_inode->indirect=indir_pt;
    }
    // allocate blocks in the bitmap, then have the pointers in the file's inode point to these datablocks

    // see if we need more blocks than we already have
    if(additional_blocks > 0){
        // we need to allocate more blocks to this file
        // start the index from the last block allocated
        for(i=current_blocks; i<total_blocks_towrite; i++){
            int free_block = find_avail_block();
            if(free_block == -1){
                return 0;
            }

            if(i<12){
                // point the direct pointers
                f_inode->block_pointers[i] = free_block;
            }else{
                // use the indirect block to point to each allocated block
                indirect_block_pts[i-12] = free_block;
            }
        }
    }

    // start writing
    int start = fd->write_pt/BLOCK_SIZE;
    int start_offset = fd->write_pt % BLOCK_SIZE;

    // read the original file from the disk first
    // allocate enough space needed for the entire file

    int to_allocate = BLOCK_SIZE * total_blocks_towrite;
    void *file_buffer = malloc(to_allocate);


    for(i=start; i<current_blocks; i++){
        if(i<12){
            read_blocks(f_inode->block_pointers[i], 1, (file_buffer + (i-start)*BLOCK_SIZE));
        }else{
            read_blocks(indirect_block_pts[i-12], 1, (file_buffer + (i-start)*BLOCK_SIZE));
        }
    }

    // copy into the sequence the string we want to append and write
    memcpy((file_buffer + start_offset), buf, bytes_towrite);

    // write the buffer back to the disk
    for(i=start; i<total_blocks_towrite; i++){
        if(i<12){
            write_blocks(f_inode->block_pointers[i], 1, (file_buffer + (i-start)*BLOCK_SIZE));
        }else{
            write_blocks(indirect_block_pts[i-12], 1, (file_buffer + (i-start)*BLOCK_SIZE));
        }
    }

    // now just update the metadata on the disk
    // update the size information in the inode
    // ORIGINAL SIZE == 0

    if(f_inode->size < updated_bytes){
        f_inode->size = updated_bytes;
    }
    
    // update the file descriptor pointer
    fd->write_pt = updated_bytes;

    // write the indirect block to the disk
    if(total_blocks_towrite > 12){
        write_blocks(f_inode->indirect, 1, &indirect_block_pts);
    }

    // write everything else to the disk
    int inode_blocks_needed = sizeof(inodes)/BLOCK_SIZE;
    if(sizeof(inodes)%BLOCK_SIZE != 0){
        // add one more block for overflow
        inode_blocks_needed++;
    }
    write_blocks(1, inode_blocks_needed, &inodes);
    write_blocks(3995, 4, &free_bitmap);

    free(file_buffer);
    return bytes_towrite;
}

int sfs_fread(int fileID,char *buf, int length){
    if(length < 0){
        return 0;
    }
    int i;
    // fetch the file descriptor
    FileDesc *fd = &filedesc_table[fileID];
    if(fd->inode_num == -1){
        return 0;
    }

    // fetch inode associated to file
    Inode *f_inode = &inodes[fd->inode_num];
    // no file, nothing to read
    if(f_inode->size == 0){
        return 0;
    }

    int bytes_toread, last_block;

    // if the length overshoots the size of the file, we need to roll it back
    if(fd->read_pt + length > f_inode->size){
        bytes_toread = f_inode->size - fd->read_pt;
        last_block = f_inode->size/BLOCK_SIZE;
        if(f_inode->size%BLOCK_SIZE != 0){
            last_block++;
        }
    }else{
        bytes_toread = length;
        last_block = (fd->read_pt + length)/BLOCK_SIZE;
        if((fd->read_pt + length)%BLOCK_SIZE != 0){
            last_block++;
        }
    }

    // find the index of first block where the file starts
    int start_block_index = fd->read_pt/BLOCK_SIZE;
    // find the offset
    int start_offset = fd->read_pt%BLOCK_SIZE;

    // see if the file to be read is using indirect pointers
    // void *indirect_buff = malloc(BLOCK_SIZE);
    int used_blocks = f_inode->size/1024;
    if(f_inode->size%1024!=0){
        used_blocks++;
    }
    int indirect_block_pts[BLOCK_SIZE];

    if(used_blocks>12){
        read_blocks(f_inode->indirect, 1, indirect_block_pts);
        // memcpy(&indirect_block_pts, indirect_buff, BLOCK_SIZE);
    }
    
    // allocate a buffer to hold the file
    void *file_buff = malloc(BLOCK_SIZE * last_block);

    for(i=start_block_index; i<last_block; i++){
        if(i<12){
            read_blocks(f_inode->block_pointers[i], 1, (file_buff + (i-start_block_index)*BLOCK_SIZE));
        }else{
            read_blocks(indirect_block_pts[i-12], 1, (file_buff + (i-start_block_index)*BLOCK_SIZE));
        }
    }

    memcpy(buf, (file_buff + start_offset), bytes_toread);
    // reassign the read pointer
    fd->read_pt += bytes_toread;

    // free(indirect_buff);
    free(file_buff);

    return bytes_toread;
}

int sfs_remove(char *file){
    // find the directory entry of the file
    int i, j;
    int node_index = -1;

    for(i=0; i<MAXFILES; i++){
        DirEntry *entry = &directory[i];
        if(entry->inode_index != -1){
            if(strcmp(file, entry->filename) == 0){
                node_index = entry->inode_index;

                // as well as remove the entry from the directory
                entry->inode_index = -1;
                for(j=0; j<MAXFILENAME; j++){
                    entry->filename[j] = '\0';
                }
                break;
            }
        }
    }

    if(node_index < 0){
        // file doesn't exist
        return -1;
    }else{
        Inode *inode = &inodes[node_index];
        int blocks_held = inode->size/1024;
        if(inode->size%1024!=0){
            blocks_held++;
        }

        // overwriting all the data with 0s

        char emptying[BLOCK_SIZE] = {0}; 
        int indirect_block[BLOCK_SIZE];
        if(inode->indirect != -1){
            read_blocks(inode->indirect, 1, indirect_block);
        }
        for(i=0; i<blocks_held; i++){
            if(i<12){
                write_blocks(inode->block_pointers[i], 1, emptying);
                free_bitmap[inode->block_pointers[i]] = false;
            }else{
                write_blocks(indirect_block[i-12], 1, emptying);
                free_bitmap[indirect_block[i-12]] = false;
            }
        }

        free_bitmap[inode->indirect] = false;

        // TODO filesize
        inode->link_count = 0;
        inode->size = 0;
        for(j=0; j<12; j++){
            // unused set to -1
            inode->block_pointers[j] = -1;
        }
        inode->indirect = -1;

        // write everything else to the disk
        int inode_blocks_needed = sizeof(inodes)/BLOCK_SIZE;
        if(sizeof(inodes)%BLOCK_SIZE != 0){
            // add one more block for overflow
            inode_blocks_needed++;
        }
        write_blocks(1, inode_blocks_needed, &inodes);

        int dir_blocks_needed = sizeof(directory)/BLOCK_SIZE;
        if(sizeof(directory)%BLOCK_SIZE != 0){
            // add one more block for overflow
            dir_blocks_needed++;
        }
        write_blocks(3995, 4, &free_bitmap);


    }

    return 0;
}