#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
struct aux_data{
    struct file* file;
    off_t ofs;
    size_t read_bytes;
    size_t zero_bytes;
    int cnt;
};
tid_t process_create_initd(const char *file_name);
tid_t process_fork(const char *name, struct intr_frame *if_);
bool lazy_load_segment(struct page *page, void *aux);
int process_exec(void *f_name);
int process_wait(tid_t);
void process_exit(void);
void process_activate(struct thread *next);
void argument_stack(char **parse, int count, void **rsp);

/* pid를 입력하여 자식프로세스인지 확인하여 맞다면 thread 구조체 반환 */
struct thread *get_child_process(int pid);

#endif /* userprog/process.h */