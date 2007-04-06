# FuseFS.rb
#
# The ruby portion of FuseFS main library
#
# This includes helper functions, common uses, etc.

require 'fusefs_lib'

module FuseFS
  VERSION = '0.7.0'
  @running = true
  def FuseFS.run
    fd = FuseFS.fuse_fd
    io = IO.for_fd(fd)
    while @running
      reads, foo, errs = IO.select([io],nil,[io])
      break unless FuseFS.process
    end
  end
  def FuseFS.unmount
    system("fusermount -u #{@mountpoint}")
  end
  def FuseFS.exit
    @running = false
  end
  class FuseDir
    def split_path(path)
      cur, *rest = path.scan(/[^\/]+/)
      if rest.empty?
        [ cur, nil ]
      else
        [ cur, File.join(rest) ]
      end
    end
    def scan_path(path)
      path.scan(/[^\/]+/)
    end
  end
  class MetaDir < FuseDir
    def initialize
      @subdirs  = Hash.new(nil)
      @files    = Hash.new(nil)
      @times    = Hash.new(nil)
      @ctime    = Time.now
      @mtime    = Time.now
      @atime    = Time.now
    end

    def size(path)
      base, rest = split_path(path)
      case
      when base.nil?
        default
      when rest
        @subdirs[base].size(rest)
      when @files.has_key?(base)
        @files[base].to_s.size
      else
        @subdirs[base].size(rest)
      end
    end

    def time(path,sym,index,default)
      base, rest = split_path(path)
      case
      when base.nil?
        default
      when rest
        @subdirs[base].respond_to?(sym) ? @subdirs[base].send(sym,path) : default
      when @files.has_key?(base)
        if @files[base].respond_to?(sym)
          @files[base].send(sym)
        else
          @times[base][index]
        end
      else
        @subdirs[base].respond_to?(sym) ? @subdirs[base].send(sym,path) : default
      end
    end

    def atime(path)
      time(path,:atime,0,@atime)
    end
    def ctime(path)
      time(path,:ctime,1,@ctime)
    end
    def mtime(path)
      time(path,:mtime,2,@mtime)
    end

    # Contents of directory.
    def contents(path)
      @atime = Time.now
      base, rest = split_path(path)
      case
      when base.nil?
        (@files.keys + @subdirs.keys).sort.uniq
      when ! @subdirs.has_key?(base)
        nil
      when rest.nil?
        @subdirs[base].contents('/')
      else
        @subdirs[base].contents(rest)
      end
    end

    # File types
    def directory?(path)
      base, rest = split_path(path)
      case
      when base.nil?
        true
      when ! @subdirs.has_key?(base)
        false
      when rest.nil?
        true
      else
        @subdirs[base].directory?(rest)
      end
    end
    def file?(path)
      base, rest = split_path(path)
      case
      when base.nil?
        false
      when rest.nil?
        @files.has_key?(base)
      when ! @subdirs.has_key?(base)
        false
      else
        @subdirs[base].file?(rest)
      end
    end

    # File Reading
    def read_file(path)
      base, rest = split_path(path)
      case
      when base.nil?
        nil
      when rest.nil?
        @times[base][0] = Time.now
        @files[base].to_s
      when ! @subdirs.has_key?(base)
        nil
      else
        @subdirs[base].read_file(rest)
      end
    end

    # Write to a file
    def can_write?(path)
      return false unless Process.uid == FuseFS.reader_uid
      base, rest = split_path(path)
      case
      when base.nil?
        true
      when rest.nil?
        true
      when ! @subdirs.has_key?(base)
        false
      else
        @subdirs[base].can_write?(rest)
      end
    end
    def write_to(path,file)
      base, rest = split_path(path)
      case
      when base.nil?
        false
      when rest.nil?
        if @files.has_key?(base)
          @times[base][2] = Time.now # mtime
          @times[base][0] = Time.now # atime
        else
          @times[base] = [Time.now,Time.now,Time.now]
        end
        @files[base] = file
      when ! @subdirs.has_key?(base)
        false
      else
        @subdirs[base].write_to(rest,file)
      end
    end

    # Delete a file
    def can_delete?(path)
      return false unless Process.uid == FuseFS.reader_uid
      base, rest = split_path(path)
      case
      when base.nil?
        false
      when rest.nil?
        @files.has_key?(base)
      when ! @subdirs.has_key?(base)
        false
      else
        @subdirs[base].can_delete?(rest)
      end
    end
    def delete(path)
      base, rest = split_path(path)
      case
      when base.nil?
        nil
      when rest.nil?
        # Delete it.
        @files.delete(base)
        @times.delete(base)
        @mtime = Time.now
      when ! @subdirs.has_key?(base)
        nil
      else
        @subdirs[base].delete(rest)
        @mtime = Time.now
      end
    end

    # Make a new directory
    def can_mkdir?(path)
      return false unless Process.uid == FuseFS.reader_uid
      base, rest = split_path(path)
      case
      when base.nil?
        false
      when rest.nil?
        ! (@subdirs.has_key?(base) || @files.has_key?(base))
      when ! @subdirs.has_key?(base)
        false
      else
        @subdirs[base].can_mkdir?(rest)
      end
    end
    def mkdir(path,dir=nil)
      base, rest = split_path(path)
      case
      when base.nil?
        false
      when rest.nil?
        dir ||= self.class.new
        @subdirs[base] = dir
        @mtime = Time.now
        true
      when ! @subdirs.has_key?(base)
        false
      else
        @subdirs[base].mkdir(rest,dir)
      end
    end

    # Delete an existing directory.
    def can_rmdir?(path)
      return false unless Process.uid == FuseFS.reader_uid
      base, rest = split_path(path)
      case
      when base.nil?
        false
      when rest.nil?
        @subdirs.has_key?(base)
      when ! @subdirs.has_key?(base)
        false
      else
        @subdirs[base].can_rmdir?(rest)
      end
    end
    def rmdir(path)
      base, rest = split_path(path)
      case
      when base.nil?
        false
      when rest.nil?
        @subdirs.delete(base)
        @mtime = Time.now
        true
      when ! @subdirs.has_key?(base)
        false
      else
        @subdirs[base].rmdir(rest)
      end
    end
  end
end
