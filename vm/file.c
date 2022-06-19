/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "include/threads/vaddr.h"
#include "include/userprog/process.h"
#include "include/threads/mmu.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;

	file_page->cnt = 0;
	file_page->file = NULL;
	file_page->read_bytes = 0;
	file_page->zero_bytes = 0;
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	list_remove(&page->frame->frame_elem);

}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	struct thread *t = thread_current();

	if(pml4_is_dirty(t->pml4, page->va)){
		file_write_at(file_page->file,page->frame->kva, page->file.read_bytes, page->file.offset);
		pml4_set_dirty(t->pml4, page->va, 0);	
	}
	pml4_clear_page(t->pml4, page->va);
	list_remove(&page->frame->frame_elem);
	free(page->frame);
}

void *do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset) 
{
	struct file *reopen_file = file_reopen(file);
	struct thread *t = thread_current();
	int cnt = length % PGSIZE ? (int)(length/PGSIZE)+1 : (int)(length/PGSIZE);
	// size_t read_bytes = length < PGSIZE ? PGSIZE : file_length(reopen_file);

	void* origin_addr = addr;

	for (int i = 0; i < cnt; i++){
		if (spt_find_page(&t->spt, addr)){
			return NULL;
		}
	}

	for (int i = 0; i < cnt; i++){
		size_t page_read_bytes = length < PGSIZE ? length : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		struct aux_data *aux = calloc(sizeof(struct aux_data), 1);
		aux->cnt = cnt;
		aux->file = reopen_file;
		aux->ofs = offset;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;

		if (!vm_alloc_page_with_initializer(VM_FILE, addr, writable, lazy_load_segment,(void *)aux)){
			free(aux);
			return false;
		}

		length -= page_read_bytes;
		offset += page_read_bytes;
		addr += PGSIZE;
	}
	return origin_addr;
}


/* Do the munmap */
void
do_munmap (void *addr) {
	struct thread* t = thread_current();
	
	struct page *page = spt_find_page(&t->spt, addr);
	struct file *target_file = page->file.file;

	while (1){
		struct page* p = spt_find_page(&t->spt, addr);
		if (p == NULL){
			return NULL;
		}
		// dirty bit가 1일 경우(파일이 수정되었을 경우) 파일에 다시 써주기
		// if (pml4_is_dirty(t->pml4, p->va)){
		// 	file_write_at(target_file, p->frame->kva, p->file.read_bytes, p->file.offset);
		// 	pml4_set_dirty(t->pml4, p->va, 0);	
		// }
		// pml4_clear_page(t->pml4, p->va); // present를 0으로 초기화
		file_backed_destroy(p);

		hash_delete(&t->spt.pages, &p->hash_elem);
		free(p);
		addr +=  PGSIZE;
	}
}