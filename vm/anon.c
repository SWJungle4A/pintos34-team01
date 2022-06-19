/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "include/threads/vaddr.h"
#include "include/lib/kernel/bitmap.h"

static struct bitmap *swap_table; // swap disk에서 사용 가능한 영역과 사용중인 영역을 구분하기 위한 테이블
const size_t SECTORS_PER_PAGE = PGSIZE / DISK_SECTOR_SIZE; // 스왑 영역을 페이지 사이즈 단위로 관리하기 위한 값

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* 필요에 따라 수정 가능한 함수 - 아직까지 수정 안 함. */
	/* TODO: Set up the swap_disk. */
	swap_disk = NULL;
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;
	/* 필요에 따라 수정 가능한 함수 - 일단 true를 반환하도록 수정함. */
	struct anon_page *anon_page = &page->anon;
	// anon_page->type = type;
	anon_page->slot_number = -1;

	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	/* 비트맵을 처음부터 순회해 false 값을 가진 비트를 하나 찾는다.
	   즉, 페이지를 할당받을 수 있는 swap slot을 하나 찾는다. */
	size_t empty_slot = bitmap_scan_and_flip(swap_table, 0, 1, true);

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;



	lock_acquire(&frame_lock);
    list_remove(&page->frame->frame_elem);
    lock_release(&frame_lock);

	free(page->frame);
	return;
}
