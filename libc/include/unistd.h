#ifndef _UNISTD_H
#define _UNISTD_H

// @TODO: Incorrect include.
#include <sys/types.h>

// @TODO: void in arg list
pid_t getpid();

ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

off_t lseek(int fd, off_t offset, int whence);

#endif
