#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

/* ================= FILESYSTEM CONSTANTS ================= */

#define BLOCK_SIZE        4096U
#define INODE_SIZE         128U
#define DIRECT_POINTERS      8U
#define NAME_LEN            28

#define FS_MAGIC        0x56534653U
#define JOURNAL_MAGIC   0x4A524E4C

#define JOURNAL_BLOCK_IDX  1U
#define JOURNAL_BLOCKS    16U

/* ================= STRUCTURES ================= */

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
};

struct inode {
    uint16_t type;   // 0 = free, 1 = file, 2 = dir
    uint16_t links;
    uint32_t size;
    uint32_t direct[DIRECT_POINTERS];
    uint32_t ctime;
    uint32_t mtime;
};

struct dirent {
    uint32_t inode;
    char name[NAME_LEN];
};

/* ================= JOURNAL STRUCTURES ================= */

struct journal_header {
    uint32_t magic;
    uint32_t nbytes_used;
};

enum { REC_DATA = 0, REC_COMMIT = 1 };

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

/* ================= HELPERS ================= */

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

/* ================= COMMAND: CREATE ================= */

void cmd_create(int fd, const char *name) {
    struct superblock sb;
    read_block(fd, 0, &sb);

    /* Read inode bitmap */
    uint8_t inode_bmap[BLOCK_SIZE];
    read_block(fd, sb.inode_bitmap, inode_bmap);

    int free_ino = -1;
    for (uint32_t i = 1; i < sb.inode_count; i++) {
        if (!test_bit(inode_bmap, i)) {
            free_ino = i;
            break;
        }
    }
    if (free_ino < 0) die("no free inode");

    /* Read inode 0 (root) */
    uint8_t inode_block[BLOCK_SIZE];
    read_block(fd, sb.inode_start, inode_block);
    struct inode *inodes = (struct inode *)inode_block;
    struct inode root = inodes[0];

    uint32_t root_dir_block = root.direct[0];

    /* Read root directory */
    uint8_t root_data[BLOCK_SIZE];
    read_block(fd, root_dir_block, root_data);
    struct dirent *ents = (struct dirent *)root_data;

    int free_slot = -1;
    for (int i = 0; i < BLOCK_SIZE / sizeof(struct dirent); i++) {
        if (ents[i].inode == 0 && ents[i].name[0] == '\0') {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) die("root directory full");

    /* ---- PREPARE METADATA IN MEMORY ---- */

    set_bit(inode_bmap, free_ino);

    ents[free_slot].inode = free_ino;
    strncpy(ents[free_slot].name, name, NAME_LEN);

    inodes[0].size += sizeof(struct dirent);

    uint32_t new_inode_block = sb.inode_start +
        (free_ino * INODE_SIZE) / BLOCK_SIZE;
    uint32_t offset = (free_ino * INODE_SIZE) % BLOCK_SIZE;

    uint8_t new_inode_blk[BLOCK_SIZE];
    if (new_inode_block == sb.inode_start)
        memcpy(new_inode_blk, inode_block, BLOCK_SIZE);
    else
        read_block(fd, new_inode_block, new_inode_blk);

    struct inode *new_ino =
        (struct inode *)(new_inode_blk + offset);
    memset(new_ino, 0, sizeof(struct inode));
    new_ino->type = 1;
    new_ino->links = 1;
    new_ino->ctime = new_ino->mtime = time(NULL);

    /* ---- JOURNAL WRITE ---- */

    uint32_t jstart = JOURNAL_BLOCK_IDX * BLOCK_SIZE;
    struct journal_header jh;

    if (pread(fd, &jh, sizeof(jh), jstart) != sizeof(jh))
        die("read journal header");

    if (jh.magic != JOURNAL_MAGIC) {
        jh.magic = JOURNAL_MAGIC;
        jh.nbytes_used = sizeof(jh);
    }

    uint32_t off = jh.nbytes_used;

#define APPEND(ptr, sz) do { \
    if (pwrite(fd, ptr, sz, jstart + off) != sz) die("journal write"); \
    off += sz; \
} while (0)

    struct data_record r;

    r.hdr.type = REC_DATA;
    r.hdr.size = sizeof(r);
    r.block_no = sb.inode_bitmap;
    memcpy(r.data, inode_bmap, BLOCK_SIZE);
    APPEND(&r, sizeof(r));

    r.block_no = root_dir_block;
    memcpy(r.data, root_data, BLOCK_SIZE);
    APPEND(&r, sizeof(r));

    r.block_no = new_inode_block;
    memcpy(r.data, new_inode_blk, BLOCK_SIZE);
    APPEND(&r, sizeof(r));

    struct commit_record c;
    c.hdr.type = REC_COMMIT;
    c.hdr.size = sizeof(c);
    APPEND(&c, sizeof(c));

    jh.nbytes_used = off;
    if (pwrite(fd, &jh, sizeof(jh), jstart) != sizeof(jh))
        die("write journal header");

    printf("Created '%s' (journaled only). Run install.\n", name);
}

/* ================= COMMAND: INSTALL ================= */

void cmd_install(int fd) {
    uint32_t jstart = JOURNAL_BLOCK_IDX * BLOCK_SIZE;
    struct journal_header jh;

    if (pread(fd, &jh, sizeof(jh), jstart) != sizeof(jh))
        die("read journal header");

    if (jh.magic != JOURNAL_MAGIC || jh.nbytes_used == sizeof(jh)) {
        printf("Journal empty.\n");
        return;
    }

    uint8_t *buf = malloc(jh.nbytes_used);
    if (!buf) die("malloc");

    if (pread(fd, buf, jh.nbytes_used, jstart) != (ssize_t)jh.nbytes_used)
        die("read journal");

    uint32_t ptr = sizeof(jh);
    int applied = 0;

    while (ptr < jh.nbytes_used) {
        struct rec_header *rh = (struct rec_header *)(buf + ptr);
        if (rh->type == REC_DATA) {
            struct data_record *dr = (struct data_record *)rh;
            write_block(fd, dr->block_no, dr->data);
        }
        if (rh->type == REC_COMMIT)
            applied++;
        ptr += rh->size;
    }

    jh.nbytes_used = sizeof(jh);
    if (pwrite(fd, &jh, sizeof(jh), jstart) != sizeof(jh))
        die("clear journal");

    free(buf);
    printf("Installed %d transaction(s).\n", applied);
}

/* ================= MAIN ================= */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s create <name>\n", argv[0]);
        printf("  %s install\n", argv[0]);
        return 1;
    }

    int fd = open("vsfs.img", O_RDWR);
    if (fd < 0) die("open vsfs.img");

    if (!strcmp(argv[1], "create") && argc == 3)
        cmd_create(fd, argv[2]);
    else if (!strcmp(argv[1], "install"))
        cmd_install(fd);
    else
        die("invalid command");

    close(fd);
    return 0;
}
