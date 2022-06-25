#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "include/filesys/fat.h"
#include "include/lib/kernel/bitmap.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
 * Must be exactly DISK_SECTOR_SIZE bytes long. */
/* 디스크에 올려져있는 아이노드. 정확히 DISK_SECTOR_SIZE bytes(512byte)여야 한다. */
struct inode_disk {
	disk_sector_t start;                /* First data sector. */
	off_t length;                       /* File size in bytes. */
	unsigned magic;                     /* Magic number. */
	uint32_t unused[125];               /* Not used. */
};

/* Returns the number of sectors to allocate for an inode SIZE
 * bytes long. */
/* 아이노드 크기 바이트 길이에 할당할 섹터 수를 반환 */
static inline size_t
bytes_to_sectors (off_t size) {
	return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode {
	struct list_elem elem;              /* Element in inode list. */
	disk_sector_t sector;               /* Sector number of disk location. */
	int open_cnt;                       /* Number of openers. */
	bool removed;                       /* True if deleted, false otherwise. */
	int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
	struct inode_disk data;             /* Inode content. */
};

/* Returns the disk sector that contains byte offset POS within
 * INODE.
 * Returns -1 if INODE does not contain data for a byte at offset
 * POS. */

/* pos(바이트 수)를 이용하여 해당 inode는 어디서 부터 읽어야하는 디스크섹터 인지 계산해서 반환한다. 
 * 만약 해당되는 데이터가 없다면 -1을 반환한다.*/
static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos) {
	ASSERT (inode != NULL);
	if (pos < inode->data.length)
		return inode->data.start + pos / DISK_SECTOR_SIZE;
	else
		return -1;
}

/* List of open inodes, so that opening a single inode twice
 * returns the same `struct inode'. */
/* 열린 inode 리스트 */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) {
	list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
 * writes the new inode to sector SECTOR on the file system
 * disk.
 * Returns true if successful.
 * Returns false if memory or disk allocation fails. */

/* 데이터의 length bytes로 아이노드를 초기화하고 파일 시스템 디스크의 sector에 새 아이노드를 쓴다.
 * 성공하면 true를, 메모리 혹은 디스크 할당 실패시 false를 반환 */
bool
inode_create (disk_sector_t sector, off_t length) {
	struct inode_disk *disk_inode = NULL;
	bool success = false;

	ASSERT (length >= 0);

	/* If this assertion fails, the inode structure is not exactly
	 * one sector in size, and you should fix that. */
	ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

	disk_inode = calloc (1, sizeof *disk_inode);
	if (disk_inode != NULL) {
		size_t sectors = bytes_to_sectors (length);
		disk_inode->length = length;
		disk_inode->magic = INODE_MAGIC;
		if (free_map_allocate (sectors, &disk_inode->start)) {
			disk_write (filesys_disk, sector, disk_inode);
			if (sectors > 0) {
				static char zeros[DISK_SECTOR_SIZE];
				size_t i;

				for (i = 0; i < sectors; i++) 
					disk_write (filesys_disk, disk_inode->start + i, zeros); 
			}
			success = true; 
		} 
		free (disk_inode);
	}
	return success;
}

/* Reads an inode from SECTOR
 * and returns a `struct inode' that contains it.
 * Returns a null pointer if memory allocation fails. */

/* 섹터에서 아이노드를 읽고 해당 아이노드가 포함된 struct inode를 반환한다.
 * 메모리 할당 실패 시 null 포인터를 반환한다. */
struct inode *
inode_open (disk_sector_t sector) {
	struct list_elem *e;
	struct inode *inode;

	/* Check whether this inode is already open. */
	for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
			e = list_next (e)) {
		inode = list_entry (e, struct inode, elem);
		if (inode->sector == sector) {
			inode_reopen (inode);
			return inode; 
		}
	}

	/* Allocate memory. */
	inode = malloc (sizeof *inode);
	if (inode == NULL)
		return NULL;

	/* Initialize. */
	list_push_front (&open_inodes, &inode->elem);
	inode->sector = sector;
	inode->open_cnt = 1;
	inode->deny_write_cnt = 0;
	inode->removed = false;
	disk_read (filesys_disk, inode->sector, &inode->data);
	return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode) {
	if (inode != NULL)
		inode->open_cnt++;
	return inode;
}

/* Returns INODE's inode number. */
/* 아이노드의 넘버를 반환 */
disk_sector_t
inode_get_inumber (const struct inode *inode) {
	return inode->sector;
}

/* Closes INODE and writes it to disk.
 * If this was the last reference to INODE, frees its memory.
 * If INODE was also a removed inode, frees its blocks. */

/* 아이노드를 닫고 이것을 디스크에 쓴다.
 * 이것이 아이노드에 대한 마지막 참조라면 메모리를 해제한다. - free(inode)
 * 아이노드가 제거된 인자가 온것이라면 블록을 해제한다. */
void
inode_close (struct inode *inode) {
	/* Ignore null pointer. */
	if (inode == NULL)
		return;

	/* Release resources if this was the last opener. */
	if (--inode->open_cnt == 0) {
		/* Remove from inode list and release lock. */
		list_remove (&inode->elem);

		/* Deallocate blocks if removed. */
		if (inode->removed) {
			free_map_release (inode->sector, 1);
			free_map_release (inode->data.start,
					bytes_to_sectors (inode->data.length)); 
		}

		free (inode); 
	}
}

/* Marks INODE to be deleted when it is closed by the last caller who
 * has it open. */

/* 아이노드를 연 마지막 호출자가 해당 아이노드를 닫을 때, 아이노드를 삭제할 수 있도록 mark한다. */
void
inode_remove (struct inode *inode) {
	ASSERT (inode != NULL);
	inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
 * Returns the number of bytes actually read, which may be less
 * than SIZE if an error occurs or end of file is reached. */

/* size 바이트를 inode에서 buffer로 위치 offset부터 읽는다(쓴다).
 * 오류가 발생하거나 파일 끝에 도달한 경우 size보다 작을 수 있는 실제읽은 바이트 수를 반환한다. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) {
	uint8_t *buffer = buffer_;
	off_t bytes_read = 0;
	uint8_t *bounce = NULL;

	while (size > 0) {
		/* Disk sector to read, starting byte offset within sector. */
		disk_sector_t sector_idx = byte_to_sector (inode, offset);
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left = inode_length (inode) - offset;
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually copy out of this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Read full sector directly into caller's buffer. */
			disk_read (filesys_disk, sector_idx, buffer + bytes_read); 
		} else {
			/* Read sector into bounce buffer, then partially copy
			 * into caller's buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}
			disk_read (filesys_disk, sector_idx, bounce);
			memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_read += chunk_size;
	}
	free (bounce);

	return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
 * Returns the number of bytes actually written, which may be
 * less than SIZE if end of file is reached or an error occurs.
 * (Normally a write at end of file would extend the inode, but
 * growth is not yet implemented.) */

/* buffer에서 size 바이트만큼 offset부터 inode에 쓴다.
 * 오류가 발생하거나 파일 끝에 도달한 경우 size보다 작을 수 있는 실제 쓴 바이트 수를 반환한다.
 * (보통 파일 끝에 쓰는 것은 inode를 확장시키지만 growth는 아직 구현되지 않았다.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
		off_t offset) {
	const uint8_t *buffer = buffer_;
	off_t bytes_written = 0;
	uint8_t *bounce = NULL;

	if (inode->deny_write_cnt)
		return 0;
	
	/* Allocate a new block and update data block pointer in inode
	 * Fill the allocated blocks with zero. */

	/* 새 블록을 할당하고 inode에서 데이터 블록 포인터를 업데이트한다.
	 * 할당된 블록을 0으로 채운다. */
	// off_t old_length = inode_length(inode);
	// off_t write_end  = offset + size - 1;

	// if (write_end > old_length -1){
	// 	/* 파일 크기가 업데이트되면 inode를 업데이트한다. */
		
	// }

	while (size > 0) {
		/* Sector to write, starting byte offset within sector. */

		/* sector_idx : offset이 포함된 섹터 인덱스
		 * sector_ofs : 해당 섹터내에서 정확히 어느 지점부터 읽어야 하는지를 계산한 오프셋*/
		disk_sector_t sector_idx = byte_to_sector (inode, offset);
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */

		/* inode_left : offset을 뺀 아이노드 크기 
		 * sector_left : 해당 섹터의 offset을 뺀 남은 섹터 사이즈 
		 * min_left : 둘 중 작은 사이즈를 담는다. */
		off_t inode_left = inode_length (inode) - offset;
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually write into this sector. */
		/* chunk_size는 해당 섹터에 실제로 쓸 바이트 수를 의미한다. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Write full sector directly to disk. */
			/* 해당 섹터내용 전체를 디스크에 직접 쓴다.  */
			disk_write (filesys_disk, sector_idx, buffer + bytes_written); 
		} else {
			/* We need a bounce buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}

			/* If the sector contains data before or after the chunk
			   we're writing, then we need to read in the sector
			   first.  Otherwise we start with a sector of all zeros. */
			
			/* 만약 섹터에 우리가 쓰고 있는 청크의 전후로 데이터가 있다면, 먼저 섹터를 읽어야 한다.
			 * 그렇지 않다면, 모두 제로인 섹터에서 시작해야 한다. */
			if (sector_ofs > 0 || chunk_size < sector_left) 
				disk_read (filesys_disk, sector_idx, bounce);
			else
				memset (bounce, 0, DISK_SECTOR_SIZE);
			memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
			disk_write (filesys_disk, sector_idx, bounce); 
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_written += chunk_size;
	}
	free (bounce);

	return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
	void
inode_deny_write (struct inode *inode) 
{
	inode->deny_write_cnt++;
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
 * Must be called once by each inode opener who has called
 * inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) {
	ASSERT (inode->deny_write_cnt > 0);
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
	inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
/* 아이노드의 사이즈 반환 */
off_t
inode_length (const struct inode *inode) {
	return inode->data.length;
}
