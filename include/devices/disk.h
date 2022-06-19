#ifndef DEVICES_DISK_H
#define DEVICES_DISK_H

#include <inttypes.h>
#include <stdint.h>

/* Size of a disk sector in bytes. */
#define DISK_SECTOR_SIZE 512

/* Index of a disk sector within a disk.
 * Good enough for disks up to 2 TB. */
typedef uint32_t disk_sector_t;

/* Format specifier for printf(), e.g.:
 * printf ("sector=%"PRDSNu"\n", sector); */
#define PRDSNu PRIu32

void disk_init (void); // 디스크 할당
void disk_print_stats (void);

struct disk *disk_get (int chan_no, int dev_no); // 디스크 영역이 시작되는 주소 리턴
disk_sector_t disk_size (struct disk *); // 디스크 스왑영역의 전체 크기를 알 수 있음
void disk_read (struct disk *, disk_sector_t, void *); // swap in 할때 디스크에 있는
void disk_write (struct disk *, disk_sector_t, const void *); // 4096 / 512

void 	register_disk_inspect_intr ();
#endif /* devices/disk.h */
