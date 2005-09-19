/* ruby-fuse
 *
 * A Ruby module to interact with the FUSE userland filesystem in
 * a Rubyish way.
 */

// #include "RubyFuse.h"

#define FUSE_USE_VERSION 22
#define _FILE_OFFSET_BITS 64

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <ruby.h>

#include "fusefs_fuse.h"

/* init_time
 *
 * All files will have a modified time equal to this. */
time_t init_time;

/* opened_file
 *
 * FuseFS uses the opened_file list to keep files that are written to in
 * memory until they are closed before passing it to FuseRoot.write_to,
 * and file contents returned by FuseRoot.read_file until FUSE informs
 * us it is safe to close.
 */
typedef struct __opened_file_ {
  char   *path;
  char   *value;
  long   writesize;
  long   size;
  long   zero_offset;
  struct __opened_file_ *next;
} opened_file;

static opened_file *head = NULL;

static int
file_openedP(const char *path) {
  opened_file *ptr;
  for (ptr = head;ptr; ptr = ptr->next)
    if (!strcmp(path,ptr->path)) return 1;
  return 0;
}

/* When a file is being written to, its value starts with this much
 * allocated and grows by this much when necessary. */
#define FILE_GROW_SIZE  1024

/* When a file is created, the OS will first mknod it, then attempt to
 *   fstat it immediately. We get around this by using a static path name
 *   for the most recently mknodd'd path. */
static char *created_file = NULL;

/* Ruby Constants constants */
VALUE cFuseFS      = Qnil; /* FuseFS class */
VALUE cFSException = Qnil; /* Our Exception. */
VALUE FuseRoot     = Qnil; /* The root object we call */

/* IDs for calling methods on objects. */
ID id_dir_contents;
ID id_read_file;
ID id_write_to;
ID id_remove;
ID id_mkdir;
ID id_rmdir;

ID is_directory;
ID is_file;
ID is_writable;
ID is_removable;
ID can_mkdir;
ID can_rmdir;

ID id_dup;

typedef unsigned long int (*rbfunc)();

/* I'm too lazy to turn everything into a VALUE array
 * for passing to rf_protect, so I'm just doing it
 * this way.
 *
 * Faster, too. */

static VALUE rb_path;
static ID to_call;

/* rf_protected and rf_call
 *
 * Used for: protection.
 *
 * This is called by rb_protect, and will make a call using
 * the above rb_path and to_call ID to call the method safely
 * on FuseRoot.
 *
 * We call rf_call(path,method_id), and rf_call will use rb_protect
 *   to call rf_protected, which makes the call on FuseRoot and returns
 *   whatever the call returns.
 */
static VALUE
rf_protected(VALUE arg) {
  if (arg == Qnil) {
    return rb_funcall(FuseRoot,to_call,1,rb_path);
  } else {
    return rb_funcall(FuseRoot,to_call,2,rb_path,arg);
  }
}

static VALUE
rf_call(const char *path, ID method, VALUE arg) {
  int error;
  VALUE result;

  if (!rb_respond_to(FuseRoot,method)) {
    return Qnil;
  }
  /* Set up the call and make it. */
  rb_path = rb_str_new2(path);
  to_call = method;
  result = rb_protect(rf_protected, arg, &error);
 
  /* Did it error? */
  if (error) return Qnil;

  return result;
}

/* rf_getattr
 *
 * Used when: 'ls', and before opening a file.
 *
 * FuseFS will call: directory? and file? on FuseRoot
 *   to determine if the path in question is pointing
 *   at a directory or file. The permissions attributes
 *   will be 777 (dirs) and 666 (files) xor'd with FuseFS.umask
 */
static int
rf_getattr(const char *path, struct stat *stbuf) {
  /* If it doesn't exist, it doesn't exist. Simple as that. */
  VALUE retval;
  char *value;
  size_t len;

  /* Zero out the stat buffer */
  memset(stbuf, 0, sizeof(struct stat));

  /* printf("In getattr for %s!\n", path ); */
  /* printf("created_file is %s!\n", created_file ); */

  /* "/" is automatically a dir. */
  if (strcmp(path,"/") == 0) {
    stbuf->st_mode = S_IFDIR | 0555;
    stbuf->st_size = 4096;
    stbuf->st_nlink = 1;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_mtime = init_time;
    stbuf->st_atime = init_time;
    stbuf->st_ctime = init_time;
    return 0;
  }

  /* If we created it with mknod, then it "exists" */
  if (created_file && (strcmp(created_file,path) == 0)) {
    /* It's created */
    stbuf->st_mode = S_IFREG | 0444;
    stbuf->st_nlink = 1 + file_openedP(path);
    stbuf->st_size = 0;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_mtime = init_time;
    stbuf->st_atime = init_time;
    stbuf->st_ctime = init_time;
    return 0;
  }

  /* If FuseRoot says the path is a directory, we set it 0555.
   * If FuseRoot says the path is a file, it's 0444.
   *
   * Otherwise, -ENOENT */
  if (RTEST(rf_call(path, is_directory,Qnil))) {
    if (RTEST(rf_call(path,is_writable,Qnil))) {
      stbuf->st_mode = S_IFDIR | 0777;
    } else {
      stbuf->st_mode = S_IFDIR | 0555;
    }
    stbuf->st_nlink = 1;
    stbuf->st_size = 4096;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_mtime = init_time;
    stbuf->st_atime = init_time;
    stbuf->st_ctime = init_time;
    return 0;
  } else if (RTEST(rf_call(path, is_file,Qnil))) {
    if (RTEST(rf_call(path,is_writable,Qnil))) {
      stbuf->st_mode = S_IFREG | 0666;
    } else {
      stbuf->st_mode = S_IFREG | 0444;
    }
    stbuf->st_nlink = 1 + file_openedP(path);
    stbuf->st_size = 0;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_mtime = init_time;
    stbuf->st_atime = init_time;
    stbuf->st_ctime = init_time;
    return 0;
  }
  return -ENOENT;
}

/* rf_readdir
 *
 * Used when: 'ls'
 *
 * FuseFS will call: 'directory?' on FuseRoot with the given path
 *   as an argument. If the return value is true, then it will in turn
 *   call 'contents' and expects to receive an array of file contents.
 *
 * '.' and '..' are automatically added, so the programmer does not
 *   need to worry about those.
 */
static int
rf_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
           off_t offset, struct fuse_file_info *fi) {
  VALUE contents;
  VALUE cur_entry;
  VALUE retval;

  /* printf( "In rf_readdir!\n" ); */

  /* This is what fuse does to turn off 'unused' warnings. */
  (void) offset;
  (void) fi;

  /* FuseRoot must exist */
  if (FuseRoot == Qnil) {
    if (!strcmp(path,"/")) {
      filler(buf,".", NULL, 0);
      filler(buf,"..", NULL, 0);
      return 0;
    }
    return -ENOENT;
  }

  if (strcmp(path,"/") != 0) {
    retval = rf_call(path, is_directory,Qnil);

    if (!RTEST(retval)) {
      return -ENOENT;
    }
  }
 
  /* These two are Always in a directory */
  filler(buf,".", NULL, 0);
  filler(buf,"..", NULL, 0);

  retval = rf_call(path, id_dir_contents,Qnil);
  if (!RTEST(retval)) {
    return 0;
  }
  if (TYPE(retval) != T_ARRAY) {
    return 0;
  }

  /* Duplicate the array, just in case. */
  /* TODO: Do this better! */
  retval = rb_funcall(retval,id_dup,0);

  while ((cur_entry = rb_ary_shift(retval)) != Qnil) {

    if (TYPE(cur_entry) != T_STRING)
      continue;

    filler(buf,STR2CSTR(cur_entry),NULL,0);
  }
  return 0;
}

/* rf_mknod
 *
 * Used when: This is called when a file is created.
 *
 * Note that this is actually almost useless to FuseFS, so all we do is check
 *   if a path is writable? and if so, return true. The open() will do the
 *   actual work of creating the file.
 */
static int
rf_mknod(const char *path, mode_t umode, dev_t rdev) {
  opened_file *ptr;

  /* Make sure it's not already open. */
  for (ptr = head;ptr != NULL;ptr = ptr->next)
    if (strcmp(ptr->path,path) == 0) break;
  if (ptr)
    return -EACCES;

  /* printf("File isn't open\n"); */

  /* We ONLY permit regular files. No blocks, characters, fifos, etc. */
  if (!S_ISREG(umode))
    return -EACCES;

  /* printf("It's an IFREG file\n"); */

  if (RTEST(rf_call(path, is_file,Qnil))) {
    return -EEXIST;
  }

  /* printf( "It doesn't exist\n" ); */

  /* Is this writable to */
  if (!RTEST(rf_call(path,is_writable,Qnil))) {
    return -EACCES;
  }
  /* printf( "It's writable to ...\n"); */

  if (created_file)
    free(created_file);

  created_file = strdup(path);

  return 0;
}

/* rf_open
 *
 * Used when: A file is opened for read or write.
 *
 * If called to open a file for reading, then FuseFS will call "read_file" on
 *   FuseRoot, and store the results into the linked list of "opened_file"
 *   structures, so as to provide the same file for mmap, all excutes of
 *   read(), and preventing more than one call to FuseRoot.
 *
 * If called on a file opened for writing, FuseFS will first double check
 *   if the file is writable to by calling "writable?" on FuseRoot, passing
 *   the path. If the return value is a truth value, it will create an entry
 *   into the opened_file list, flagged as for writing.
 *
 * If called with any other set of flags, this will return -ENOPERM, since
 *   FuseFS does not (currently) need to support anything other than direct
 *   read and write.
 */
static int
rf_open(const char *path, struct fuse_file_info *fi) {
  VALUE body;
  char *value;
  size_t len;
  opened_file *newfile;

  /* printf("Open called for %s!\n", path); */

  /* Make sure it's not already open. */
  if (file_openedP(path))
    return -EACCES;

  if ((fi->flags & 3) == O_RDONLY) {
    /* Open for read. */
    /* Make sure it exists. */
    if (!RTEST(rf_call(path,is_file,Qnil))) {
      return -ENOENT;
    }

    body = rf_call(path, id_read_file,Qnil);

    /* I don't wanna deal with non-strings :D. */
    if (TYPE(body) != T_STRING) {
      return -ENOENT;
    }

    /* We have the body, now save it the entire contents to our
     * opened_file lists. */
    newfile = malloc(sizeof(opened_file));
    newfile->value = strdup(rb_str2cstr(body,&newfile->size));
    newfile->writesize = 0;
    newfile->zero_offset = 0;
    newfile->path  = strdup(path);

    newfile->next = head;
    head = newfile;
    return 0;

  } else if (((fi->flags & 3) == O_RDWR) ||
             (((fi->flags & 3) == O_WRONLY) && (fi->flags & O_APPEND))) {
    /* Can we write to it? */
    if (!RTEST(rf_call(path,is_writable,Qnil))) {
      return -EACCES;
    }

    /* Make sure it exists. */
    if (RTEST(rf_call(path,is_file,Qnil))) {
      body = rf_call(path, id_read_file,Qnil);

      /* I don't wanna deal with non-strings :D. */
      if (TYPE(body) != T_STRING) {
        return -ENOENT;
      }

      /* We have the body, now save it the entire contents to our
       * opened_file lists. */
      newfile = malloc(sizeof(opened_file));
      newfile->value = strdup(rb_str2cstr(body,&newfile->size));
      newfile->writesize = newfile->size+1;
      newfile->path  = strdup(path);
      newfile->zero_offset = 0;
    } else {
      newfile = malloc(sizeof(opened_file));
      newfile->writesize = FILE_GROW_SIZE;
      newfile->value = malloc(newfile->writesize);
      newfile->path  = strdup(path);
      newfile->size  = 0;
      newfile->zero_offset = 0;
      *(newfile->value) = '\0';
    }

    if (fi->flags & O_APPEND) {
      newfile->zero_offset = newfile->size;
    }

    newfile->next = head;
    head = newfile;
    return 0;
  } else if ((fi->flags & 3) == O_WRONLY) {
    /* printf("Opening for write!\n"); */
    /* Open for write. */
    /* Can we write to it? */
    if (!RTEST(rf_call(path,is_writable,Qnil))) {
      return -EACCES;
    }

    /* We can write to it. Create an opened_write_file entry and initialize
     * it to a small size. */
    newfile = malloc(sizeof(opened_file));
    newfile->writesize = FILE_GROW_SIZE;
    newfile->value = malloc(newfile->writesize);
    newfile->path  = strdup(path);
    newfile->size  = 0;
    newfile->zero_offset = 0;
    *(newfile->value) = '\0';

    newfile->next = head;
    head = newfile;

    if (created_file && (strcasecmp(created_file,path) == 0)) {
      free(created_file);
      created_file = NULL;
    }
    return 0;
  } else {
    return -EACCES;
  }
}

/* rf_release
 *
 * Used when: A file is no longer being read or written to.
 *
 * If release is called on a written file, FuseFS will call 'write_to' on
 *   FuseRoot, passing the path and contents of the file. It will then
 *   clear the file information from the in-memory file storage that
 *   FuseFS uses to prevent FuseRoot from receiving incomplete files.
 *
 * If called on a file opened for reading, FuseFS will just clear the
 *   in-memory copy of the return value from rf_open.
 */
static int
rf_release(const char *path, struct fuse_file_info *fi) {

  opened_file *ptr,*prev;

  /* printf("In release for %s!\n", path); */

  /* Find the opened file. */
  for (ptr = head, prev=NULL;ptr;prev = ptr,ptr = ptr->next)
    if (strcmp(ptr->path,path) == 0) break;

  /* printf("Looking for opened file ...\n"); */
  
  /* If we don't have this open, it doesn't exist. */
  if (ptr == NULL)
    return -ENOENT;

  /* printf("Found it!\n"); */

  /* Is this a file that was open for write?
   *
   * If so, call write_to. */
  if (ptr->writesize != 0) {
    /* printf("Write size is nonzero.\n"); */
    /* printf("size is %d.\n", ptr->size); */
    /* printf("value is %s.\n", ptr->value); */
    rf_call(path,id_write_to,rb_str_new(ptr->value,ptr->size));
  }

  /* printf("freeing\n"); */
  
  /* Free the file contents. */
  if (prev == NULL) {
    head = ptr->next;
  } else {
    prev->next = ptr->next;
  }
  free(ptr->value);
  free(ptr->path);
  free(ptr);

  return 0;
}

/* rf_rename
 *
 * Used when: a file is renamed.
 *
 * When FuseFS receives a rename command, it really just removes the old file
 *   and creates the new file with the same contents.
 */
static int
rf_rename(const char *path, const char *dest) {
  /* Does it exist to be edited? */
  if (!RTEST(rf_call(path,is_file,Qnil)))
    return -ENOENT;

  /* Can we remove the old one? */
  if (!RTEST(rf_call(path,is_removable,Qnil)))
    return -EACCES;
 
  /* Can we create the new one? */
  if (!RTEST(rf_call(dest,is_writable,Qnil)))
    return -EACCES;

  /* Copy it over and then remove. */
  VALUE body = rf_call(path,id_read_file,Qnil);
  if (TYPE(body) != T_STRING) {
    /* We just write a null file, then. Ah well. */
    VALUE newstr = rb_str_new2("");
    rf_call(path,id_write_to,newstr);
  } else {
    rf_call(path,id_write_to,body);
  }
  return 0;
}

/* rf_unlink
 *
 * Used when: a file is removed.
 *
 * This calls can_remove? and remove() on FuseRoot.
 */
static int
rf_unlink(const char *path) {
  /* printf( "In rf_unlink for %s!\n", path ); */

  /* Does it exist to be removed? */
  if (!RTEST(rf_call(path,is_file,Qnil)))
    return -ENOENT;

  /* Can we remove it? */
  if (!RTEST(rf_call(path,is_removable,Qnil)))
    return -EACCES;
 
  /* Ok, remove it! */
  rf_call(path,id_remove,Qnil);
  return 0;
}

/* rf_truncate
 *
 * Used when: a file is truncated.
 *
 * If this is an existing file?, that is writable? to, then FuseFS will
 *   read the file, truncate it, and call write_to with the new value.
 */
static int
rf_truncate(const char *path, off_t offset) {
  /* printf( "rf_truncate(\"%s\",%d)\n", path, offset ); */

  /* Does it exist to be truncated? */
  if (!RTEST(rf_call(path,is_file,Qnil)))
    return -ENOENT;

  /* Can we write to it? */
  if (!RTEST(rf_call(path,is_removable,Qnil)))
    return -EACCES;
 
  /* If offset is 0, then we just overwrite it with an empty file. */
  if (offset > 0) {
    VALUE newstr = rb_str_new2("");
    rf_call(path,id_write_to,newstr);
  } else {
    VALUE body = rf_call(path,id_read_file,Qnil);
    if (TYPE(body) != T_STRING) {
      /* We just write a null file, then. Ah well. */
      VALUE newstr = rb_str_new2("");
      rf_call(path,id_write_to,newstr);
    } else {
      long size;
      char *str = rb_str2cstr(body,&size);

      /* Just in case offset is bigger than the file. */
      if (offset >= size) return 0;

      str[offset] = '\0';
      rf_call(path,id_write_to,rb_str_new2(str));
    }
  }
  return 0;
}

/* rf_mkdir
 *
 * Used when: A user calls 'mkdir'
 *
 * This calls can_mkdir? and mkdir() on FuseRoot.
 */
static int
rf_mkdir(const char *path, mode_t mode) {
  /* Does it exist? */
  if (RTEST(rf_call(path,is_directory,Qnil)))
    return -EEXIST;

  if (RTEST(rf_call(path,is_file,Qnil)))
    return -EEXIST;

  /* Can we mkdir it? */
  if (!RTEST(rf_call(path,can_mkdir,Qnil)))
    return -EACCES;
 
  /* Ok, mkdir it! */
  rf_call(path,id_mkdir,Qnil);
  return 0;
}

/* rf_rmdir
 *
 * Used when: A user calls 'rmdir'
 *
 * This calls can_rmdir? and rmdir() on FuseRoot.
 */
static int
rf_rmdir(const char *path, mode_t mode) {
  /* Does it exist? */
  if (!RTEST(rf_call(path,is_directory,Qnil)))
    return -ENOENT;

  if (!RTEST(rf_call(path,is_file,Qnil)))
    return -ENOTDIR;

  /* Can we rmdir it? */
  if (!RTEST(rf_call(path,can_rmdir,Qnil)))
    return -EACCES;
 
  /* Ok, rmdir it! */
  rf_call(path,id_rmdir,Qnil);
  return 0;
}

/* rf_write
 *
 * Used when: a file is written to by the user.
 *
 * This does not access FuseRoot at all. Instead, it appends the written
 *   data to the opened_file entry, growing its memory usage if necessary.
 */
static int
rf_write(const char *path, const char *buf, size_t size, off_t offset,
         struct fuse_file_info *fi) {

  opened_file *ptr;

  /* printf( "In rf_write for %s, write '%s'\n", path, buf ); */
  /* printf( "Offset is %d\n", offset ); */

  /* Find the opened file. */
  for (ptr = head;ptr;ptr = ptr->next)
    if (strcmp(ptr->path,path) == 0) break;

  /* If we don't have this open, we can't write to it. */
  if (ptr == NULL)
    return 0;

  /* printf("File %s is open!\n", path); */

  /* Make sure it's open for read ... */
  if (ptr->writesize == 0) {
    return 0;
  }

  /* We have it, so now we need to write to it. */
  offset += ptr->zero_offset;

  /* Grow memory if necessary. */
  if ((offset + size + 1) > ptr->writesize) {
    size_t newsize;
    newsize = offset + size + 1 + FILE_GROW_SIZE;
    newsize -= newsize % FILE_GROW_SIZE;
    ptr->writesize = newsize;
    ptr->value = realloc(ptr->value, newsize);
  }

  memcpy(ptr->value + offset, buf, size);

  /* I really don't know if a null bit is required, but this
   * also functions as a size bit I can pass to rb_string_new2
   * to allow binary data */
  if (offset+size > ptr->size)
    ptr->size = offset+size;
  ptr->value[ptr->size] = '\0';

  return size;
}

/* rf_read
 *
 * Used when: A file opened by rf_open is read.
 *
 * This does not access FuseRoot at all. It merely reads from the already-read
 *   'file' that is saved in the opened_file list.
 */
static int
rf_read(const char *path, char *buf, size_t size, off_t offset,
        struct fuse_file_info *fi) {

  opened_file *ptr;

  /* printf( "In rf_read!\n" ); */
  /* Find the opened file. */
  for (ptr = head;ptr;ptr = ptr->next)
    if (strcmp(ptr->path,path) == 0) break;

  /* If we don't have this open, it doesn't exist. */
  if (ptr == NULL)
    return -ENOENT;

  /* Is there anything left to read? */
  if (offset < ptr->size) {
    if (offset + size > ptr->size)
      size = ptr->size - offset;
    memcpy(buf, ptr->value + offset, size);
    return size;
  }

  return 0;
}

/* rf_oper
 *
 * Used for: FUSE utilizes this to call operations at the appropriate time.
 *
 * This is utilized by rf_mount
 */
static struct fuse_operations rf_oper = {
    .getattr   = rf_getattr,
    .readdir   = rf_readdir,
    .mknod     = rf_mknod,
    .unlink    = rf_unlink,
    .mkdir     = rf_mkdir,
    .truncate  = rf_truncate,
    .rename    = rf_rename,
    .open      = rf_open,
    .release   = rf_release,
    .read      = rf_read,
    .write     = rf_write,
};

/* rf_set_root
 *
 * Used by: FuseFS.set_root
 *
 * This defines FuseRoot, which is the crux of FuseFS. It is required to
 *   have the methods "directory?" "file?" "contents" "writable?" "read_file"
 *   and "write_to"
 */
VALUE
rf_set_root(VALUE self, VALUE rootval) {
  if (self != cFuseFS) {
    rb_raise(cFSException,"Error: 'set_root' called outside of FuseFS?!");
    return Qnil;
  }

  rb_iv_set(cFuseFS,"@mountpoint",rootval);
  FuseRoot = rootval;
  return Qtrue;
}

/* rf_mount_to
 *
 * Used by: FuseFS.mount_to(dir)
 *
 * FuseFS.mount_to(dir) calls FUSE to mount FuseFS under the given directory.
 */
VALUE
rf_mount_to(VALUE self, VALUE mountpoint) {
  if (self != cFuseFS) {
    rb_raise(cFSException,"Error: 'mount_to' called outside of FuseFS?!");
    return Qnil;
  }

  Check_Type(mountpoint, T_STRING); 

  rb_iv_set(cFuseFS,"@mountpoint",mountpoint);
  fusefs_setup(STR2CSTR(mountpoint), &rf_oper);
  return Qtrue;
}

/* rf_fd
 *
 * Used by: FuseFS.fuse_fd(dir)
 *
 * FuseFS.fuse_fd returns the file descriptor of the open handle on the
 *   /dev/fuse object that is utilized by FUSE. This is crucial for letting
 *   ruby keep control of the script, as it can now use IO.select, rather
 *   than turning control over to fuse_main.
 */
VALUE
rf_fd(VALUE self) {
  int fd = fusefs_fd();
  if (fd < 0)
    return Qnil;
  return INT2NUM(fd);
}

/* rf_process
 *
 * Used for: FuseFS.process
 *
 * rf_process, which calls fusefs_process, is the other crucial portion to
 *   keeping ruby in control of the script. fusefs_process will read and
 *   process exactly one command from the fuse_fd. If this is called when
 *   there is no incoming data waiting, it *will* hang until it receives a
 *   command on the fuse_fd
 */
VALUE
rf_process(VALUE self) {
  fusefs_process();
}


/* rf_uid and rf_gid
 *
 * Used by: FuseFS.reader_uid and FuseFS.reader_gid
 *
 * These return the UID and GID of the processes that are causing the
 *   separate Fuse methods to be called. This can be used for permissions
 *   checking, returning a different file for different users, etc.
 */
VALUE
rf_uid(VALUE self) {
  int fd = fusefs_uid();
  if (fd < 0)
    return Qnil;
  return INT2NUM(fd);
}

VALUE
rf_gid(VALUE self) {
  int fd = fusefs_gid();
  if (fd < 0)
    return Qnil;
  return INT2NUM(fd);
}

/* Init_fusefs_lib()
 *
 * Used by: Ruby, to initialize FuseFS.
 *
 * This is just stuff to set up and establish the Ruby module FuseFS and
 *   its methods.
 */
void
Init_fusefs_lib() {
  head = NULL;
  init_time = time(NULL);

  /* module FuseFS */
  cFuseFS = rb_define_module("FuseFS");

  /* Our exception */
  cFSException = rb_define_class_under(cFuseFS,"FuseFSException",rb_eStandardError);

  /* def Fuse.run */
  rb_define_singleton_method(cFuseFS,"fuse_fd",     (rbfunc) rf_fd, 0);
  rb_define_singleton_method(cFuseFS,"reader_uid",  (rbfunc) rf_uid, 0);
  rb_define_singleton_method(cFuseFS,"reader_gid",  (rbfunc) rf_gid, 0);
  rb_define_singleton_method(cFuseFS,"process",     (rbfunc) rf_process, 0);
  rb_define_singleton_method(cFuseFS,"mount_to",    (rbfunc) rf_mount_to, 1);
  rb_define_singleton_method(cFuseFS,"mount_under", (rbfunc) rf_mount_to, 1);
  rb_define_singleton_method(cFuseFS,"set_root",    (rbfunc) rf_set_root, 1);

  id_dir_contents = rb_intern("contents");
  id_read_file    = rb_intern("read_file");
  id_write_to     = rb_intern("write_to");
  id_remove       = rb_intern("delete");
  id_mkdir        = rb_intern("mkdir");
  id_rmdir        = rb_intern("rmdir");

  is_directory = rb_intern("directory?");
  is_file      = rb_intern("file?");
  is_writable  = rb_intern("can_write?");
  is_removable = rb_intern("can_delete?");
  can_mkdir    = rb_intern("can_mkdir?");
  can_rmdir    = rb_intern("can_rmdir?");

  id_dup       = rb_intern("dup");
}
