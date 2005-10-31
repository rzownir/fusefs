/* fusefs_fuse.h */

/* This is rewriting most of the things that occur
 * in fuse_main up through fuse_loop */

#ifndef __FUSEFS_FUSE_H_
#define __FUSEFS_FUSE_H_

int fusefs_fd();
int fusefs_unmount();
int fusefs_ehandler();
int fusefs_setup(char *mountpoint, const struct fuse_operations *op, char *opts);
int fusefs_process();
int fusefs_uid();
int fusefs_gid();

#endif
