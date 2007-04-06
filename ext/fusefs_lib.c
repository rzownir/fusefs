/* ruby-fuse
 *
 * A Ruby module to interact with the FUSE userland filesystem in
 * a Rubyish way.
 */

// #define DEBUG

#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64

#include <fuse.h>
#include <fuse/fuse_lowlevel.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
// #include <sys/stat.h>
#include <fcntl.h>
#include <ruby.h>

#ifdef DEBUG
#include <stdarg.h>
#endif

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
  int    modified;
  long   writesize;
  long   size;
  long   zero_offset;
  int    raw;
  struct __opened_file_ *next;
} opened_file;

typedef opened_file editor_file;

static opened_file *opened_head = NULL;
static editor_file *editor_head = NULL;

static int
file_openedP(const char *path) {
  opened_file *ptr;
  for (ptr = opened_head;ptr; ptr = ptr->next)
    if (!strcmp(path,ptr->path)) return 1;
  return 0;
}

/* When a file is being written to, its value starts with this much
 * allocated and grows by this much when necessary. */
#define FILE_GROW_SIZE  1024

/* When a file is created, the OS will first mknod it, then attempt to
 *   fstat it immediately. We get around this by using a static path name
 *   for the most recently mknodd'd path. */
static char   *created_file = NULL;
static time_t  created_time = 0;

/* Ruby Constants constants */
VALUE cFuseFS      = Qnil; /* FuseFS class */
VALUE cFSException = Qnil; /* Our Exception. */
VALUE FuseRoot     = Qnil; /* The root object we call */

/* IDs for calling methods on objects. */

#define RMETHOD(name,cstr) \
  char *c_ ## name = cstr; \
  ID name;

RMETHOD(id_dir_contents,"contents");
RMETHOD(id_read_file,"read_file");
RMETHOD(id_write_to,"write_to");
RMETHOD(id_delete,"delete");
RMETHOD(id_mkdir,"mkdir");
RMETHOD(id_rmdir,"rmdir");
RMETHOD(id_touch,"touch");
RMETHOD(id_chmod,"chmod");
RMETHOD(id_size,"size");

RMETHOD(id_mtime,"mtime");
RMETHOD(id_ctime,"ctime");
RMETHOD(id_atime,"atime");

RMETHOD(is_directory,"directory?");
RMETHOD(is_file,"file?");
RMETHOD(is_executable,"executable?");
RMETHOD(can_write,"can_write?");
RMETHOD(can_delete,"can_delete?");
RMETHOD(can_mkdir,"can_mkdir?");
RMETHOD(can_rmdir,"can_rmdir?");

RMETHOD(id_raw_open,"raw_open");
RMETHOD(id_raw_close,"raw_close");
RMETHOD(id_raw_read,"raw_read");
RMETHOD(id_raw_write,"raw_write");
RMETHOD(id_raw_rename,"raw_rename");

RMETHOD(id_dup,"dup");
RMETHOD(id_to_i,"to_i");

typedef unsigned long int (*rbfunc)();

/* debug()
 *
 * If #define DEBUG is enabled, then this acts as a printf to stderr
 */
#ifdef DEBUG
static void
debug(char *msg,...) {
  va_list ap;
  va_start(ap,msg);
  vfprintf(stderr,msg,ap);
}
#else
// Make debug just comment out what's after it.
#define debug // debug
#endif

/* catch_editor_files
 *
 * If this is a true value, then FuseFS will attempt to capture
 * editor swap files and handle them itself, so the ruby filesystem
 * is not passed swap files it doesn't care about.
 */

int handle_editor = 1;
int which_editor  = 0;
#define EDITOR_VIM    1
#define EDITOR_EMACS  2

/* editor_fileP
 *
 * Passed a path, editor_fileP will return if it is likely to be a file
 * belonging to an editor.
 *
 * vim: /path/to/.somename.ext.sw*
 * emacs: /path/to/#somename.ext#
 */
static int
editor_fileP(const char *path) {
  char *filename;
  editor_file *ptr;

  if (!handle_editor)
    return 0;
  
  /* Already created one */
  for (ptr = editor_head ; ptr ; ptr = ptr->next) {
    if (strcasecmp(ptr->path,path) == 0) {
      return 2;
    }
  }

  /* Basic checks */
  filename = strrchr(path,'/');
  if (!filename) return 0; // No /.
  filename++;
  if (!*filename) return 0; // / is the last.

  /* vim */
  do {
    // vim uses: .filename.sw?
    char *ptr = filename;
    int len;
    if (*ptr != '.') break;

    // ends with .sw? 
    ptr = strrchr(ptr,'.');
    len = strlen(ptr);
    // .swp or .swpx
    if (len != 4 && len != 5) break;
    if (strncmp(ptr,".sw",3) == 0) {
      debug("  (%s is a vim file).\n", path);
      which_editor = EDITOR_VIM;
      return 1; // It's a vim file.
    }
  } while (0);

  /* emacs */
  do {
    char *ptr = filename;
    // Begins with a #
    if (*ptr != '#') break;

    // Ends with a #
    ptr = strrchr(ptr,'#');
    if (!ptr) break;
    // the # must be the end of the filename.
    ptr++;
    if (*ptr) break;
    debug("  (%s is an emacs file).\n", path);
    which_editor = EDITOR_EMACS;
    return 1;
  } while (0);
  return 0;
}

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
rf_protected(VALUE args) {
  ID to_call = SYM2ID(rb_ary_shift(args));
  return rb_apply(FuseRoot,to_call,args);
}

static VALUE
rf_int_protected(VALUE args) {
  static VALUE empty_ary = Qnil;
  if (empty_ary == Qnil) empty_ary = rb_ary_new();
  return rb_apply(args,id_to_i,empty_ary);
}

#define rf_call(p,m,a) \
  rf_mcall(p,m, c_ ## m, a)

static VALUE
rf_mcall(const char *path, ID method, char *methname, VALUE arg) {
  int error;
  VALUE result;
  VALUE methargs;

  if (!rb_respond_to(FuseRoot,method)) {
    return Qnil;
  }

  if (arg == Qnil) {
    debug("    root.%s(%s)\n", methname, path );
  } else {
    debug("    root.%s(%s,...)\n", methname, path );
  }

  if (TYPE(arg) == T_ARRAY) {
    methargs = arg;
  } else if (arg != Qnil) {
    methargs = rb_ary_new();
    rb_ary_push(methargs,arg);
  } else {
    methargs = rb_ary_new();
  }

  rb_ary_unshift(methargs,rb_str_new2(path));
  rb_ary_unshift(methargs,ID2SYM(method));

  /* Set up the call and make it. */
  result = rb_protect(rf_protected, methargs, &error);
 
  /* Did it error? */
  if (error) return Qnil;

  return result;
}

/* 
 * rf_getint:
 *
 * Used for: An integer wrapper around rf_call
 */
#define rf_intval(p,m,a) \
  rf_mintval(p,m, c_ ## m, a)

static int
rf_mintval(const char *path,ID method,char *methname,int def) {
  VALUE arg = rf_mcall(path,method,methname,Qnil);
  VALUE retval;
  int   error;
  if (FIXNUM_P(arg)) {
    return rb_fix2int(arg);
  } else if (RTEST(arg)) {
    if (!rb_respond_to(arg,id_to_i)) {
      return def;
    }

    retval = rb_protect(rf_int_protected, arg, &error);
   
    /* Did it error? */
    if (error) return def;

    return rb_num2long(retval);
  } else {
    return def;
  }
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

  debug("rf_getattr(%s)\n", path );
  /* Zero out the stat buffer */
  memset(stbuf, 0, sizeof(struct stat));

  /* "/" is automatically a dir. */
  if (strcmp(path,"/") == 0) {
    stbuf->st_mode = S_IFDIR | 0555;
    stbuf->st_size = 4096;
    stbuf->st_nlink = 1;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_mtime = rf_intval(path,id_mtime,init_time);
    stbuf->st_atime = rf_intval(path,id_atime,init_time);
    stbuf->st_ctime = rf_intval(path,id_ctime,init_time);
    return 0;
  }

  /* If we created it with mknod, then it "exists" */
  debug("  Checking for created file ...");
  if (created_file && (strcmp(created_file,path) == 0)) {
    /* It's created */
    debug(" created.\n");
    stbuf->st_mode = S_IFREG | 0666;
    stbuf->st_nlink = 1 + file_openedP(path);
    stbuf->st_size = 0;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_mtime = created_time;
    stbuf->st_atime = created_time;
    stbuf->st_ctime = created_time;
    return 0;
  }
  debug(" no.\n");

  /* debug("  Checking file_opened ...");
  if (file_openedP(path)) {
    debug(" opened.\n");
    stbuf->st_mode = S_IFREG | 0666;
    stbuf->st_nlink = 1 + file_openedP(path);
    stbuf->st_size = 0;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_mtime = init_time;
    stbuf->st_atime = init_time;
    stbuf->st_ctime = init_time;
    return 0;
  }
  debug(" no.\n");
  */

  debug("  Checking if editor file...");
  switch (editor_fileP(path)) {
  case 2:
    debug(" Yes, and does exist.\n");
    stbuf->st_mode = S_IFREG | 0444;
    stbuf->st_nlink = 1;
    stbuf->st_size = 0;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_mtime = init_time;
    stbuf->st_atime = init_time;
    stbuf->st_ctime = init_time;
    return 0;
  case 1:
    debug(" Yes, but doesn't exist.\n");
    return -ENOENT;
  default:
    debug("No.\n");
  }

  /* If FuseRoot says the path is a directory, we set it 0555.
   * If FuseRoot says the path is a file, it's 0444.
   *
   * Otherwise, -ENOENT */
  debug("Checking filetype ...");
  if (RTEST(rf_call(path, is_directory,Qnil))) {
    debug(" directory.\n");
    stbuf->st_mode = S_IFDIR | 0555;
    stbuf->st_nlink = 1;
    stbuf->st_size = 4096;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_mtime = rf_intval(path,id_mtime,init_time);
    stbuf->st_atime = rf_intval(path,id_atime,init_time);
    stbuf->st_ctime = rf_intval(path,id_ctime,init_time);
    return 0;
  } else if (RTEST(rf_call(path, is_file,Qnil))) {
    debug(" file.\n");
    stbuf->st_mode = S_IFREG | 0444;
    if (RTEST(rf_call(path,can_write,Qnil))) {
      stbuf->st_mode |= 0666;
    }
    if (RTEST(rf_call(path,is_executable,Qnil))) {
      stbuf->st_mode |= 0111;
    }
    stbuf->st_nlink = 1 + file_openedP(path);
    stbuf->st_size = rf_intval(path,id_size,0);
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_mtime = rf_intval(path,id_mtime,init_time);
    stbuf->st_atime = rf_intval(path,id_atime,init_time);
    stbuf->st_ctime = rf_intval(path,id_ctime,init_time);
    return 0;
  }
  debug(" nonexistant.\n");
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

  debug("rf_readdir(%s)\n", path );

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
    debug("  Checking is_directory? ...");
    retval = rf_call(path, is_directory,Qnil);

    if (!RTEST(retval)) {
      debug(" no.\n");
      return -ENOENT;
    }
    debug(" yes.\n");
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

  debug("rf_mknod(%s)\n", path);
  /* Make sure it's not already open. */
  
  debug("  Checking if it's opened ...");
  if (file_openedP(path)) {
    debug(" yes.\n");
    return -EACCES;
  }
  debug(" no.\n");

  /* We ONLY permit regular files. No blocks, characters, fifos, etc. */
  debug("  Checking if an IFREG is requested ...");
  if (!S_ISREG(umode)) {
    debug(" no.\n");
    return -EACCES;
  }
  debug(" yes.\n");

  debug("  Checking if it's an editor file ...");
  switch (editor_fileP(path)) {
  case 2:
    debug(" yes, and it exists.\n");
    return -EEXIST;
  case 1:
    debug(" yes, and it doesn't exist.\n");
    editor_file *eptr;
    eptr = ALLOC(editor_file);
    eptr->writesize = FILE_GROW_SIZE;
    eptr->value = ALLOC_N(char,eptr->writesize);
    eptr->path  = strdup(path);
    eptr->size  = 0;
    eptr->raw = 0;
    eptr->zero_offset = 0;
    eptr->modified = 0;
    *(eptr->value) = '\0';
    eptr->next = editor_head;
    editor_head = eptr;
    return 0;
  default:
    debug("no.\n");
  }

  debug("  Checking if it's a file ..." );
  if (RTEST(rf_call(path, is_file,Qnil))) {
    debug(" yes.\n");
    return -EEXIST;
  }
  debug(" no.\n");

  /* Is this writable to */
  debug("  Checking if it's writable to ...");
  if (!RTEST(rf_call(path,can_write,Qnil))) {
    debug(" no.\n");
    debug("  Checking if it looks like an editor tempfile...");
    if (editor_head && (which_editor == EDITOR_VIM)) {
      char *ptr = strrchr(path,'/');
      while (ptr && isdigit(*ptr)) ptr++;
      if (ptr && (*ptr == '\0')) {
        debug(" yes.\n");
        editor_file *eptr;
        eptr = ALLOC(editor_file);
        eptr->writesize = FILE_GROW_SIZE;
        eptr->value = ALLOC_N(char,eptr->writesize);
        eptr->path  = strdup(path);
        eptr->raw = 0;
        eptr->size  = 0;
        eptr->zero_offset = 0;
        eptr->modified = 0;
        *(eptr->value) = '\0';
        eptr->next = editor_head;
        editor_head = eptr;
        return 0;
      }
    }
    debug(" no.\n");
    return -EACCES;
  }
  debug(" yes.\n");

  if (created_file)
    free(created_file);

  created_file = strdup(path);
  created_time = time(NULL);

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
  char open_opts[4], *optr;
  opened_file *newfile;

  debug("rf_open(%s)\n", path);

  /* Make sure it's not already open. */
  debug("  Checking if it's already open ...");
  if (file_openedP(path)) {
    debug(" yes.\n");
    return -EACCES;
  }
  debug(" no.\n");
 
  debug("Checking if an editor file is requested...");
  switch (editor_fileP(path)) {
  case 2:
    debug(" yes, and it was created.\n");
    return 0;
  case 1:
    debug(" yes, but it was not created.\n");
    return -ENOENT;
  default:
    debug(" no.\n");
  }

  optr = open_opts;
  switch (fi->flags & 3) {
  case 0:
    *(optr++) = 'r';
    break;
  case 1:
    *(optr++) = 'w';
    break;
  case 2:
    *(optr++) = 'w';
    *(optr++) = 'r';
    break;
  default:
    debug("Opening a file with something other than rd, wr, or rdwr?");
  }
  if (fi->flags & O_APPEND)
    *(optr++) = 'a';
  *(optr) = '\0';

  debug("  Checking for a raw_opened file... ");
  if (RTEST(rf_call(path,id_raw_open,rb_str_new2(open_opts)))) {
    debug(" yes.\n");
    newfile = ALLOC(opened_file);
    newfile->size = 0;
    newfile->value = NULL;
    newfile->writesize = 0;
    newfile->zero_offset = 0;
    newfile->modified = 0;
    newfile->path  = strdup(path);
    newfile->raw = 1;

    newfile->next = opened_head;
    opened_head = newfile;
    return 0;
  }
  debug(" no.\n");

  debug("  Checking open type ...");
  if ((fi->flags & 3) == O_RDONLY) {
    debug(" RDONLY.\n");
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
    newfile = ALLOC(opened_file);
    value = rb_str2cstr(body,&newfile->size);
    newfile->value = ALLOC_N(char,(newfile->size)+1);
    memcpy(newfile->value,value,newfile->size);
    newfile->value[newfile->size] = '\0';
    newfile->writesize = 0;
    newfile->zero_offset = 0;
    newfile->modified = 0;
    newfile->path  = strdup(path);
    newfile->raw = 0;

    newfile->next = opened_head;
    opened_head = newfile;
    return 0;

  } else if (((fi->flags & 3) == O_RDWR) ||
             (((fi->flags & 3) == O_WRONLY) && (fi->flags & O_APPEND))) {
    /* Can we write to it? */
    debug(" RDWR or Append.\n");
    debug("  Checking if created file ...");
    if (created_file && (strcmp(created_file,path) == 0)) {
      debug(" yes.\n");
      newfile = ALLOC(opened_file);
      newfile->writesize = FILE_GROW_SIZE;
      newfile->value = ALLOC_N(char,newfile->writesize);
      newfile->path  = strdup(path);
      newfile->size  = 0;
      newfile->raw = 0;
      newfile->zero_offset = 0;
      *(newfile->value) = '\0';
      newfile->modified = 0;
      newfile->next = opened_head;
      opened_head = newfile;
      return 0;
    }
    debug(" no\n");

    debug("  Checking if we can write to it...");
    if (!RTEST(rf_call(path,can_write,Qnil))) {
      debug(" yes.\n");
      return -EACCES;
    }
    debug(" no\n");

    /* Make sure it exists. */
    if (RTEST(rf_call(path,is_file,Qnil))) {
      body = rf_call(path, id_read_file,Qnil);

      /* I don't wanna deal with non-strings :D. */
      if (TYPE(body) != T_STRING) {
        return -ENOENT;
      }

      /* We have the body, now save it the entire contents to our
       * opened_file lists. */
      newfile = ALLOC(opened_file);
      value = rb_str2cstr(body,&newfile->size);
      newfile->value = ALLOC_N(char,(newfile->size)+1);
      memcpy(newfile->value,value,newfile->size);
      newfile->writesize = newfile->size+1;
      newfile->path  = strdup(path);
      newfile->raw = 0;
      newfile->zero_offset = 0;
    } else {
      newfile = ALLOC(opened_file);
      newfile->writesize = FILE_GROW_SIZE;
      newfile->value = ALLOC_N(char,newfile->writesize);
      newfile->path  = strdup(path);
      newfile->size  = 0;
      newfile->raw = 0;
      newfile->zero_offset = 0;
      *(newfile->value) = '\0';
    }
    newfile->modified = 0;

    if (fi->flags & O_APPEND) {
      newfile->zero_offset = newfile->size;
    }

    newfile->next = opened_head;
    opened_head = newfile;
    return 0;
  } else if ((fi->flags & 3) == O_WRONLY) {
    debug(" WRONLY.\n");
#ifdef DEBUG
    if (fi->flags & O_APPEND)
      debug("    It's opened for O_APPEND\n");
    if (fi->flags & O_ASYNC)
      debug("    It's opened for O_ASYNC\n");
    if (fi->flags & O_CREAT)
      debug("    It's opened for O_CREAT\n");
    if (fi->flags & O_EXCL)
      debug("    It's opened for O_EXCL\n");
    if (fi->flags & O_NOCTTY)
      debug("    It's opened for O_NOCTTY\n");
    if (fi->flags & O_NONBLOCK)
      debug("    It's opened for O_NONBLOCK\n");
    if (fi->flags & O_SYNC)
      debug("    It's opened for O_SYNC\n");
    if (fi->flags & O_TRUNC)
      debug("    It's opened for O_TRUNC\n");
#endif

    /* Open for write. */
    /* Can we write to it? */
    debug("  Checking if we can write to it ... ");
    if (!((created_file && (strcmp(created_file,path) == 0)) ||
        RTEST(rf_call(path,can_write,Qnil)))) {
      debug(" no.\n");
      return -EACCES;
    }
    debug(" yes.\n");

    /* We can write to it. Create an opened_write_file entry and initialize
     * it to a small size. */
    newfile = ALLOC(opened_file);
    newfile->writesize = FILE_GROW_SIZE;
    newfile->value = ALLOC_N(char,newfile->writesize);
    newfile->path  = strdup(path);
    newfile->size  = 0;
    newfile->zero_offset = 0;
    newfile->modified = 0;
    newfile->raw = 0;
    *(newfile->value) = '\0';

    newfile->next = opened_head;
    opened_head = newfile;

    if (created_file && (strcasecmp(created_file,path) == 0)) {
      free(created_file);
      created_file = NULL;
      created_time = 0;
    }
    return 0;
  } else {
    debug(" Unknown...\n");
    return -ENOENT;
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
  int is_editor = 0;

  debug("rf_release(%s)\n", path);

  debug("  Checking for opened file ...");
  /* Find the opened file. */
  for (ptr = opened_head, prev=NULL;ptr;prev = ptr,ptr = ptr->next)
    if (strcmp(ptr->path,path) == 0) break;

  /* If we don't have this open, it doesn't exist. */
  if (ptr == NULL) {
    debug(" no.\n");
    debug(" Checking for opened editor file ...");
    for (ptr = opened_head, prev=NULL;ptr;prev = ptr,ptr = ptr->next)
      if (strcmp(ptr->path,path) == 0) {
        is_editor = 1;
        break;
      }
  }
  if (ptr == NULL) {
    debug(" no.\n");
    return -ENOENT;
  }
  debug(" yes.\n");

  /* If it's opened for raw read/write, call raw_close */
  debug("  Checking if it's opened for raw write...");
  if (ptr->raw) {
    /* raw read */
    debug(" yes.\n");
    rf_call(path,id_raw_close,Qnil);
  } else {
    debug(" no.\n");

    /* Is this a file that was open for write?
     *
     * If so, call write_to. */
    debug("  Checking if it's for write ...\n");
    if ((!ptr->raw) && (ptr->writesize != 0) && !editor_fileP(path)) {
      debug(" yes ...");
      if (ptr->modified) {
        debug(" and modified.\n");
        rf_call(path,id_write_to,rb_str_new(ptr->value,ptr->size));
      } else {
        debug(" and not modified.\n");
        if (!handle_editor) {
          debug("  ... But calling write anyawy.");
          rf_call(path,id_write_to,rb_str_new(ptr->value,ptr->size));
        }
      }
    }
  }

  /* Free the file contents. */
  if (!is_editor) {
    if (prev == NULL) {
      opened_head = ptr->next;
    } else {
      prev->next = ptr->next;
    }
    if (ptr->value)
      free(ptr->value);
    free(ptr->path);
    free(ptr);
  }

  return 0;
}

/* rf_chmod
 *
 * Used when: A program tries to modify objects permissions
 *
 * As there is no support for file permissions, just stub it to make
 * other applications (like cp) happy.
 */
static int
rf_chmod(const char *path, mode_t mode) {
  VALUE set_mode = INT2NUM((int) (mode & 0x7FFF));
  rf_call(path,id_chmod,set_mode);
  return 0;
}

/* rf_touch
 *
 * Used when: A program tries to modify the file's times.
 *
 * We use this for a neat side-effect thingy. When a file is touched, we
 * call the "touch" method. i.e: "touch button" would call
 * "FuseRoot.touch('/button')" and something *can* happen. =).
 */
static int
rf_touch(const char *path, struct utimbuf *ignore) {
  debug("rf_touch(%s)\n", path);
  rf_call(path,id_touch,Qnil);
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
  int iseditor = 0;
  if (editor_fileP(path) == 2) {
    iseditor = 1;
  } else {
    debug("rf_rename(%s,%s)\n", path,dest);
    debug("  Checking if %s is file ...", path);
    if (!RTEST(rf_call(path,is_file,Qnil))) {
      debug(" no.\n");
      return -ENOENT;
    }
    debug(" yes.\n");

    /* Can we remove the old one? */
    debug("  Checking if we can delete %s ...", path);
    if (!RTEST(rf_call(path,can_delete,Qnil))) {
      debug(" no.\n");
      return -EACCES;
    }
    debug(" yes.\n");
  }
 
  /* Can we create the new one? */
  debug("  Checking if we can write to %s ...", dest);
  if (!RTEST(rf_call(dest,can_write,Qnil))) {
    debug(" no.\n");
    return -EACCES;
  }
  debug(" yes.\n");

  /* Copy it over and then remove. */
  debug("  Copying.\n");
  if (iseditor) {
    editor_file *eptr,*prev;
    for (eptr=editor_head,prev=NULL;eptr;prev = eptr,eptr = eptr->next) {
      if (strcmp(path,eptr->path) == 0) {
        if (prev == NULL) {
          editor_head = eptr->next;
        } else {
          prev->next = eptr->next;
        }
        VALUE body = rb_str_new(eptr->value,eptr->size);
        rf_call(dest,id_write_to,body);
        free(eptr->value);
        free(eptr->path);
        free(eptr);
        break;
      }
    }
  } else {
    VALUE body = rf_call(path,id_read_file,Qnil);
    if (rb_respond_to(FuseRoot,id_raw_rename)) {
        rf_call(path,id_raw_rename,rb_str_new2(dest));
    } else {
      if (TYPE(body) != T_STRING) {
        /* We just write a null file, then. Ah well. */
        VALUE newstr = rb_str_new2("");
        rf_call(path,id_delete,Qnil);
        rf_call(dest,id_write_to,newstr);
      } else {
        rf_call(path,id_delete,Qnil);
        rf_call(dest,id_write_to,body);
      }
    }
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
  editor_file *eptr,*prev;
  debug("rf_unlink(%s)\n",path);

  debug("  Checking if it's an editor file ...");
  switch (editor_fileP(path)) {
  case 2:
    debug(" yes. Removing.\n");
    for (eptr=editor_head,prev=NULL;eptr;prev = eptr,eptr = eptr->next) {
      if (strcmp(path,eptr->path) == 0) {
        if (prev == NULL) {
          editor_head = eptr->next;
        } else {
          prev->next = eptr->next;
        }
        free(eptr->value);
        free(eptr->path);
        free(eptr);
        return 0;
      }
    }
    return -ENOENT;
  case 1:
    debug(" yes, but it wasn't created.\n");
    return -ENOENT;
  }
  debug(" no.\n");

  /* Does it exist to be removed? */
  debug("  Checking if it exists...");
  if (!RTEST(rf_call(path,is_file,Qnil))) {
    debug(" no.\n");
    return -ENOENT;
  }
  debug(" yes.\n");

  /* Can we remove it? */
  debug("  Checking if we can remove it...");
  if (!RTEST(rf_call(path,can_delete,Qnil))) {
    debug(" yes.\n");
    return -EACCES;
  }
  debug(" no.\n");
 
  /* Ok, remove it! */
  debug("  Removing it.\n");
  rf_call(path,id_delete,Qnil);
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
  debug( "rf_truncate(%s,%d)\n", path, offset );

  debug("Checking if it's an editor file ... ");
  if (editor_fileP(path)) {
    debug(" Yes.\n");
    opened_file *ptr;
    for (ptr = opened_head;ptr;ptr = ptr->next) {
      if (!strcmp(ptr->path,path)) {
        ptr->size = offset;
        return 0;
      }
    }
    return 0;
  }

  /* Does it exist to be truncated? */
  if (!RTEST(rf_call(path,is_file,Qnil))) {
    return -ENOENT;
  }

  /* Can we write to it? */
  if (!RTEST(rf_call(path,can_delete,Qnil))) {
    return -EACCES;
  }
 
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
  debug("rf_mkdir(%s)",path);
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
rf_rmdir(const char *path) {
  debug("rf_rmdir(%s)",path);
  /* Does it exist? */
  if (!RTEST(rf_call(path,is_directory,Qnil))) {
    if (RTEST(rf_call(path,is_file,Qnil))) {
      return -ENOTDIR;
    } else {
      return -ENOENT;
    }
  }

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
  debug("rf_write(%s)",path);

  opened_file *ptr;

  debug( "  Offset is %d\n", offset );

  debug("  Checking if file is open... ");
  /* Find the opened file. */
  for (ptr = opened_head;ptr;ptr = ptr->next)
    if (strcmp(ptr->path,path) == 0) break;

  /* If we don't have this open, we can't write to it. */
  if (ptr == NULL) {
    for (ptr = editor_head;ptr;ptr = ptr->next)
      if (strcmp(ptr->path,path) == 0) break;
  }

  if (ptr == NULL) {
    debug(" no.\n");
    return 0;
  }
  debug(" yes.\n");

  /* Make sure it's open for write ... */
  /* If it's opened for raw read/write, call raw_write */
  debug("  Checking if it's opened for raw write...");
  if (ptr->raw) {
    /* raw read */
    VALUE args = rb_ary_new();
    debug(" yes.\n");
    rb_ary_push(args,INT2NUM(offset));
    rb_ary_push(args,INT2NUM(size));
    rb_ary_push(args,rb_str_new(buf,size));
    rf_call(path,id_raw_write,args);
    return size;
  }
  debug(" no.\n");
  debug("  Checking if it's open for write ...");
  if (ptr->writesize == 0) {
    debug(" no.\n");
    return 0;
  }
  debug(" yes.\n");

  /* Mark it modified. */
  ptr->modified = 1;

  /* We have it, so now we need to write to it. */
  offset += ptr->zero_offset;

  /* Grow memory if necessary. */
  if ((offset + size + 1) > ptr->writesize) {
    size_t newsize;
    newsize = offset + size + 1 + FILE_GROW_SIZE;
    newsize -= newsize % FILE_GROW_SIZE;
    ptr->writesize = newsize;
    ptr->value = REALLOC_N(ptr->value, char, newsize);
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
 * In most cases, this does not access FuseRoot at all. It merely reads from
 * the already-read 'file' that is saved in the opened_file list.
 *
 * For files opened with raw_open, it calls raw_read
 */
static int
rf_read(const char *path, char *buf, size_t size, off_t offset,
        struct fuse_file_info *fi) {
  opened_file *ptr;

  debug( "rf_read(%s)\n", path );
  /* Find the opened file. */
  for (ptr = opened_head;ptr;ptr = ptr->next)
    if (strcmp(ptr->path,path) == 0) break;

  /* If we don't have this open, it doesn't exist. */
  if (ptr == NULL)
    return -ENOENT;

  /* If it's opened for raw read/write, call raw_read */
  if (ptr->raw) {
    /* raw read */
    VALUE args = rb_ary_new();
    rb_ary_push(args,INT2NUM(offset));
    rb_ary_push(args,INT2NUM(size));
    VALUE ret = rf_call(path,id_raw_read,args);
    if (!RTEST(ret))
      return 0;
    if (TYPE(ret) != T_STRING)
      return 0;
    memcpy(buf, RSTRING(ret)->ptr, RSTRING(ret)->len);
    return RSTRING(ret)->len;
  }

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
    .rmdir     = rf_rmdir,
    .truncate  = rf_truncate,
    .rename    = rf_rename,
    .chmod     = rf_chmod,
    .open      = rf_open,
    .release   = rf_release,
    .utime     = rf_touch,
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

  rb_iv_set(cFuseFS,"@root",rootval);
  FuseRoot = rootval;
  return Qtrue;
}

/* rf_handle_editor
 *
 * Used by: FuseFS.handle_editor <value>
 *
 * If passed a false value, then FuseFS will not attempt to handle editor
 * swap files on its own, instead passing them to the filesystem as
 * normal files.
 */
VALUE
rf_handle_editor(VALUE self, VALUE troo) {
  if (self != cFuseFS) {
    rb_raise(cFSException,"Error: 'set_root' called outside of FuseFS?!");
    return Qnil;
  }

  handle_editor = RTEST(troo);
  return Qtrue;
}

char *valid_options[] = {
  "default_permissions",
  "allow_other",
  "allow_root",
  "direct_io",
  "max_read=",
  "fsname=",
  NULL
};

int
rf_valid_option(char *option) {
  char opt[32];
  char *ptr;
  int i;

  strncpy(opt,option,31);

  if (ptr = strchr(opt,'*')) {
    ptr++;
    *ptr = '\0';
  }

  for (i=0;valid_options[i];i++) {
    if (!strcasecmp(valid_options[i],opt)) {
      return 1;
    }
  }

  return 0;
}

/* rf_mount_to
 *
 * Used by: FuseFS.mount_to(dir)
 *
 * FuseFS.mount_to(dir) calls FUSE to mount FuseFS under the given directory.
 */
VALUE
rf_mount_to(int argc, VALUE *argv, VALUE self) {
  int i;
  char opts[1024];
  char opts2[1024];
  char *cur;
  VALUE mountpoint;

  snprintf(opts,1024,"direct_io");

  if (self != cFuseFS) {
    rb_raise(cFSException,"Error: 'mount_to' called outside of FuseFS?!");
    return Qnil;
  }

  if (argc == 0) {
    rb_raise(rb_eArgError,"mount_to requires at least 1 argument!");
    return Qnil;
  }

  mountpoint = argv[0];

  Check_Type(mountpoint, T_STRING); 

  for (i = 1;i < argc; i++) {
    Check_Type(argv[i], T_STRING);
    cur = STR2CSTR(argv[i]);
    if (!rf_valid_option(cur)) {
      rb_raise(rb_eArgError,"mount_under: \"%s\" - invalid argument.", cur);
      return Qnil;
    }
    snprintf(opts2,1024,"%s,%s",opts,STR2CSTR(argv[i]));
    strcpy(opts,opts2);
  }

  rb_iv_set(cFuseFS,"@mountpoint",mountpoint);
  fusefs_setup(STR2CSTR(mountpoint), &rf_oper, opts);
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
  if (fusefs_process()) {
    return Qtrue;
  }
  return Qfalse;
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

struct const_int {
  char *name;
  int val;
};

struct const_int constvals[] = {
  { "S_ISUID", S_ISUID },
  { "S_ISGID", S_ISGID },
  { "S_ISVTX", S_ISVTX },
  { "S_IRUSR", S_IRUSR },
  { "S_IWUSR", S_IWUSR },
  { "S_IXUSR", S_IXUSR },
  { "S_IRGRP", S_IRGRP },
  { "S_IWGRP", S_IWGRP },
  { "S_IXGRP", S_IXGRP },
  { "S_IROTH", S_IROTH },
  { "S_IWOTH", S_IWOTH },
  { "S_IXOTH", S_IXOTH },
  { NULL, 0 }
};

/* Init_fusefs_lib()
 *
 * Used by: Ruby, to initialize FuseFS.
 *
 * This is just stuff to set up and establish the Ruby module FuseFS and
 *   its methods.
 */
void
Init_fusefs_lib() {
  struct const_int *vals;

  opened_head = NULL;
  init_time = time(NULL);

  /* module FuseFS */
  cFuseFS = rb_define_module("FuseFS");

  /* Our exception */
  cFSException = rb_define_class_under(cFuseFS,"FuseFSException",rb_eStandardError);

  /* def Fuse.run */
  rb_define_singleton_method(cFuseFS,"fuse_fd",     (rbfunc) rf_fd, 0);
  rb_define_singleton_method(cFuseFS,"reader_uid",  (rbfunc) rf_uid, 0);
  rb_define_singleton_method(cFuseFS,"uid",         (rbfunc) rf_uid, 0);
  rb_define_singleton_method(cFuseFS,"reader_gid",  (rbfunc) rf_gid, 0);
  rb_define_singleton_method(cFuseFS,"gid",         (rbfunc) rf_gid, 0);
  rb_define_singleton_method(cFuseFS,"process",     (rbfunc) rf_process, 0);
  rb_define_singleton_method(cFuseFS,"mount_to",    (rbfunc) rf_mount_to, -1);
  rb_define_singleton_method(cFuseFS,"mount_under", (rbfunc) rf_mount_to, -1);
  rb_define_singleton_method(cFuseFS,"mountpoint",  (rbfunc) rf_mount_to, -1);
  rb_define_singleton_method(cFuseFS,"set_root",    (rbfunc) rf_set_root, 1);
  rb_define_singleton_method(cFuseFS,"root=",       (rbfunc) rf_set_root, 1);
  rb_define_singleton_method(cFuseFS,"handle_editor",   (rbfunc) rf_handle_editor, 1);
  rb_define_singleton_method(cFuseFS,"handle_editor=",  (rbfunc) rf_handle_editor, 1);

  for (vals = constvals; vals->name; vals++) {
    rb_define_const(cFuseFS, vals->name, INT2NUM(vals->val));
  }

#undef RMETHOD
#define RMETHOD(name,cstr) \
  name = rb_intern(cstr);

  RMETHOD(id_dir_contents,"contents");
  RMETHOD(id_read_file,"read_file");
  RMETHOD(id_write_to,"write_to");
  RMETHOD(id_delete,"delete");
  RMETHOD(id_mkdir,"mkdir");
  RMETHOD(id_rmdir,"rmdir");
  RMETHOD(id_touch,"touch");
  RMETHOD(id_chmod,"chmod");
  RMETHOD(id_size,"size");

  RMETHOD(id_mtime,"mtime");
  RMETHOD(id_ctime,"ctime");
  RMETHOD(id_atime,"atime");

  RMETHOD(is_directory,"directory?");
  RMETHOD(is_file,"file?");
  RMETHOD(is_executable,"executable?");
  RMETHOD(can_write,"can_write?");
  RMETHOD(can_delete,"can_delete?");
  RMETHOD(can_mkdir,"can_mkdir?");
  RMETHOD(can_rmdir,"can_rmdir?");

  RMETHOD(id_raw_open,"raw_open");
  RMETHOD(id_raw_close,"raw_close");
  RMETHOD(id_raw_read,"raw_read");
  RMETHOD(id_raw_write,"raw_write");
  RMETHOD(id_raw_rename,"raw_rename");

  RMETHOD(id_dup,"dup");
  RMETHOD(id_to_i,"to_i");
}
