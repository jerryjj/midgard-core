#define fsync _commit
#ifndef ftruncate
#define ftruncate chsize
#endif
/* For flock() emulation */

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

extern int flock(int fd, int op);
