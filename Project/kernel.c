
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

#define BLOCK_SIZE        4096U
#define INODE_SIZE         128U
#define DIRECT_POINTERS      8U
#define NAME_LEN            28

#define FS_MAGIC        0x56534653U
#define JOURNAL_MAGIC   0x4A524E4C

#define JOURNAL_BLOCK_IDX  1U
#define JOURNAL_BLOCKS    16U
#define JOURNAL_SIZE     (JOURNAL_BLOCKS * BLOCK_SIZE)  // 65536 bytes

// Record types
enum { REC_DATA = 1, REC_COMMIT = 2 };  // Changed from 0,1 to 1,2

// Structures from spec
struct superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t journal_block;
    uint32_t inode_bitmap;
    uint32_t data_bitmap;
    uint32_t inode_start;
    uint32_t data_start;
    uint8_t _pad[128 - 9 * 4];
};

struct inode {
    uint16_t type;
    uint16_t links;
    uint32_t size;
    uint32_t direct[DIRECT_POINTERS];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t _pad[128 - (2 + 2 + 4 + DIRECT_POINTERS * 4 + 4 + 4)];
};

struct dirent {
    uint32_t inode;
    char name[NAME_LEN];
};

// Journal structures from spec
struct journal_header {
    uint32_t magic;
    uint32_t nbytes_used;
};

struct rec_header {
    uint16_t type;
    uint16_t size;
};

struct data_record {
    struct rec_header hdr;
    uint32_t block_no;
    uint8_t data[BLOCK_SIZE];
};

struct commit_record {
    struct rec_header hdr;
};

// Helper functions
void die(const char *msg) {
    perror(msg);
    exit(1);
}

void read_block(int fd, uint32_t block, void *buf) {
    if (pread(fd, buf, BLOCK_SIZE, block * BLOCK_SIZE) != BLOCK_SIZE)
        die("read_block");
}

void write_block(int fd, uint32_t block, void *buf) {
    if (pwrite(fd, buf, BLOCK_SIZE, block * BLOCK_SIZE) != BLOCK_SIZE)
        die("write_block");
}

int test_bit(uint8_t *bitmap, int idx) {
    return (bitmap[idx / 8] >> (idx % 8)) & 1;
}

void set_bit(uint8_t *bitmap, int idx) {
    bitmap[idx / 8] |= (1 << (idx % 8));
}

// Get absolute offset in journal
uint32_t journal_absolute_offset(uint32_t journal_relative_offset) {
    return JOURNAL_BLOCK_IDX * BLOCK_SIZE + journal_relative_offset;
}

// Write to journal handling block boundaries
void journal_write(int fd, uint32_t journal_offset, const void *data, size_t size) {
    uint32_t abs_offset = journal_absolute_offset(journal_offset);
    size_t written = 0;
    
    while (written < size) {
        size_t to_write = size - written;
        if (to_write > BLOCK_SIZE - (abs_offset % BLOCK_SIZE)) {
            to_write = BLOCK_SIZE - (abs_offset % BLOCK_SIZE);
        }
        
        if (pwrite(fd, (uint8_t*)data + written, to_write, abs_offset) != (ssize_t)to_write) {
            die("journal_write");
        }
        
        written += to_write;
        abs_offset += to_write;
    }
}

// Read from journal handling block boundaries
void journal_read(int fd, uint32_t journal_offset, void *data, size_t size) {
    uint32_t abs_offset = journal_absolute_offset(journal_offset);
    size_t read_bytes = 0;
    
    while (read_bytes < size) {
        size_t to_read = size - read_bytes;
        if (to_read > BLOCK_SIZE - (abs_offset % BLOCK_SIZE)) {
            to_read = BLOCK_SIZE - (abs_offset % BLOCK_SIZE);
        }
        
        if (pread(fd, (uint8_t*)data + read_bytes, to_read, abs_offset) != (ssize_t)to_read) {
            die("journal_read");
        }
        
        read_bytes += to_read;
        abs_offset += to_read;
    }
}

// Check if journal has space
int journal_has_space(struct journal_header *jh, size_t needed) {
    return (jh->nbytes_used + needed) <= JOURNAL_SIZE;
}

// Initialize journal properly
void init_journal(int fd, struct journal_header *jh) {
    jh->magic = JOURNAL_MAGIC;
    jh->nbytes_used = sizeof(struct journal_header);
    journal_write(fd, 0, jh, sizeof(*jh));
}

void cmd_create(int fd, const char *name) {
    // 1. Read superblock
    struct superblock sb;
    read_block(fd, 0, &sb);
    
    if (sb.magic != FS_MAGIC) {
        fprintf(stderr, "Not a valid VSFS filesystem\n");
        exit(1);
    }
    
    // 2. Read and check journal
    struct journal_header jh;
    journal_read(fd, 0, &jh, sizeof(jh));
    
    if (jh.magic != JOURNAL_MAGIC) {
        init_journal(fd, &jh);
    }
    
    // 3. Find free inode
    uint8_t inode_bmap[BLOCK_SIZE];
    read_block(fd, sb.inode_bitmap, inode_bmap);
    
    uint32_t free_ino = 0;
    for (uint32_t i = 1; i < sb.inode_count; i++) {  // Start from 1 (0 is root)
        if (!test_bit(inode_bmap, i)) {
            free_ino = i;
            break;
        }
    }
    
    if (free_ino == 0) {
        fprintf(stderr, "No free inodes\n");
        exit(1);
    }
    
    // 4. Read root directory to find free slot
    uint8_t inode_block[BLOCK_SIZE];
    read_block(fd, sb.inode_start, inode_block);
    struct inode *inodes = (struct inode *)inode_block;
    
    if (inodes[0].type != 2) {
        fprintf(stderr, "Root inode is not a directory\n");
        exit(1);
    }
    
    uint32_t root_dir_block = inodes[0].direct[0];
    uint8_t root_data[BLOCK_SIZE];
    read_block(fd, root_dir_block, root_data);
    
    struct dirent *ents = (struct dirent *)root_data;
    int free_slot = -1;
    for (int i = 0; i < BLOCK_SIZE / sizeof(struct dirent); i++) {
        if (ents[i].inode == 0) {
            free_slot = i;
            break;
        }
    }
    
    if (free_slot < 0) {
        fprintf(stderr, "Root directory full\n");
        exit(1);
    }
    
    // 5. Check filename length
    if (strlen(name) >= NAME_LEN) {
        fprintf(stderr, "Filename too long (max %d chars)\n", NAME_LEN - 1);
        exit(1);
    }
    
    // 6. Prepare updated blocks in memory
    // a. Update inode bitmap
    uint8_t updated_inode_bmap[BLOCK_SIZE];
    memcpy(updated_inode_bmap, inode_bmap, BLOCK_SIZE);
    set_bit(updated_inode_bmap, free_ino);
    
    // b. Update root directory
    uint8_t updated_root_data[BLOCK_SIZE];
    memcpy(updated_root_data, root_data, BLOCK_SIZE);
    ents = (struct dirent *)updated_root_data;
    ents[free_slot].inode = free_ino;  // Inode number
    strncpy(ents[free_slot].name, name, NAME_LEN - 1);
    ents[free_slot].name[NAME_LEN - 1] = '\0';
    
    // Update root directory size
    struct inode *root_inode = (struct inode *)inode_block;
    root_inode->size += sizeof(struct dirent);
    
    // c. Create new inode
    uint32_t inode_block_idx = sb.inode_start + (free_ino * INODE_SIZE) / BLOCK_SIZE;
    uint32_t inode_offset = (free_ino * INODE_SIZE) % BLOCK_SIZE;
    
    uint8_t updated_inode_block[BLOCK_SIZE];
    if (inode_block_idx == sb.inode_start) {
        memcpy(updated_inode_block, inode_block, BLOCK_SIZE);
    } else {
        read_block(fd, inode_block_idx, updated_inode_block);
    }
    
    struct inode *new_inode = (struct inode *)(updated_inode_block + inode_offset);
    memset(new_inode, 0, sizeof(struct inode));
    new_inode->type = 1;  // Regular file
    new_inode->links = 1;
    new_inode->ctime = time(NULL);
    new_inode->mtime = new_inode->ctime;
    
    // 7. Calculate space needed and check
    size_t data_record_size = sizeof(struct data_record);
    size_t commit_record_size = sizeof(struct commit_record);
    size_t transaction_size = data_record_size * 3 + commit_record_size;
    
    if (!journal_has_space(&jh, transaction_size)) {
        fprintf(stderr, "Journal full. Run 'install' first.\n");
        exit(1);
    }
    
    // 8. Write transaction to journal
    uint32_t current_offset = jh.nbytes_used;
    
    // Data record 1: Inode bitmap
    struct data_record dr;
    dr.hdr.type = REC_DATA;
    dr.hdr.size = data_record_size;
    dr.block_no = sb.inode_bitmap;
    memcpy(dr.data, updated_inode_bmap, BLOCK_SIZE);
    journal_write(fd, current_offset, &dr, data_record_size);
    current_offset += data_record_size;
    
    // Data record 2: Root directory
    dr.block_no = root_dir_block;
    memcpy(dr.data, updated_root_data, BLOCK_SIZE);
    journal_write(fd, current_offset, &dr, data_record_size);
    current_offset += data_record_size;
    
    // Data record 3: Inode table block
    dr.block_no = inode_block_idx;
    memcpy(dr.data, updated_inode_block, BLOCK_SIZE);
    journal_write(fd, current_offset, &dr, data_record_size);
    current_offset += data_record_size;
    
    // Commit record
    struct commit_record cr;
    cr.hdr.type = REC_COMMIT;
    cr.hdr.size = commit_record_size;
    journal_write(fd, current_offset, &cr, commit_record_size);
    current_offset += commit_record_size;
    
    // 9. Update journal header
    jh.nbytes_used = current_offset;
    journal_write(fd, 0, &jh, sizeof(jh));
    
    printf("Created journal entry for '%s' (inode %u). Run './journal install' to apply.\n", 
           name, free_ino);
}

void cmd_install(int fd) {
    // 1. Read journal header
    struct journal_header jh;
    journal_read(fd, 0, &jh, sizeof(jh));
    
    if (jh.magic != JOURNAL_MAGIC) {
        fprintf(stderr, "Journal does not exist or is corrupted\n");
        exit(1);
    }
    
    if (jh.nbytes_used == sizeof(struct journal_header)) {
        printf("Journal is empty\n");
        return;
    }
    
    printf("Installing journaled changes...\n");
    
    // 2. Parse journal
    uint32_t offset = sizeof(struct journal_header);
    int transactions_applied = 0;
    int in_transaction = 0;
    
    while (offset < jh.nbytes_used) {
        // Read record header
        struct rec_header rh;
        journal_read(fd, offset, &rh, sizeof(rh));
        
        if (rh.type == REC_DATA) {
            // Read full data record
            struct data_record dr;
            journal_read(fd, offset, &dr, rh.size);
            
            // Only write if we're in a valid transaction
            if (in_transaction) {
                write_block(fd, dr.block_no, dr.data);
            }
            
            offset += rh.size;
            in_transaction = 1;  // We've seen DATA, transaction started
            
        } else if (rh.type == REC_COMMIT) {
            if (in_transaction) {
                transactions_applied++;
            }
            in_transaction = 0;  // Transaction complete
            offset += rh.size;
            
        } else {
            // Invalid record type
            fprintf(stderr, "Invalid record type %u at offset %u\n", rh.type, offset);
            break;
        }
    }
    
    // 3. Clear journal (checkpoint)
    jh.nbytes_used = sizeof(struct journal_header);
    journal_write(fd, 0, &jh, sizeof(jh));
    
    // Optionally zero out the rest of journal
    uint8_t zero[BLOCK_SIZE] = {0};
    for (int i = 1; i < JOURNAL_BLOCKS; i++) {
        write_block(fd, JOURNAL_BLOCK_IDX + i, zero);
    }
    
    printf("Applied %d transaction(s). Journal cleared.\n", transactions_applied);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage:\n  %s create <filename>\n  %s install\n", argv[0], argv[0]);
        return 1;
    }
    
    int fd = open("vsfs.img", O_RDWR);
    if (fd < 0) die("open vsfs.img");
    
    if (strcmp(argv[1], "create") == 0 && argc == 3) {
        cmd_create(fd, argv[2]);
    } else if (strcmp(argv[1], "install") == 0 && argc == 2) {
        cmd_install(fd);
    } else {
        fprintf(stderr, "Invalid command\n");
        close(fd);
        return 1;
    }
    
    close(fd);
    return 0;
}