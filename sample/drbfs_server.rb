#!/usr/bin/env ruby
#
# The server portion of DRbFS - This is run on the  machine that provides
# the files that the clients (which mount it as a filesystem) will be
# dealing with.
#
# Author: Kent Sibilev
#

require 'drb'

# We're basically just passing all requests on to the local filesystem.
class RemoteDirectory
  def initialize(dir)
    @dir = dir
    @files = {}
  end

  def contents(path)
    Dir[File.join(@dir, path,'*')].map{|fn| File.basename fn}
  end

  %w|file? directory? executable? size delete|.each do |name|
    define_method(name) do |path|
      File.send name, File.join(@dir, path)
    end
  end

  %w|mkdir rmdir|.each do |name|
    define_method(name) do |path|
      Dir.send name, File.join(@dir, path)
    end
  end

  %w|can_write? can_delete? can_mkdir? can_rmdir?|.each do |name|
    define_method(name) do |path|
      true
    end
  end

  def raw_rename(path,dest)
    File.rename(File.join(@dir,path),File.join(@dir,dest))
  end

  def raw_open(path, mode)
    return true if @files.has_key? path
    @files[path] = File.open(File.join(@dir, path), mode)
    return true
  rescue
    puts $!
    false
  end

  def raw_read(path, off, size)
    file = @files[path]
    return unless file
    file.seek(off, File::SEEK_SET)
    file.read(size)
  rescue
    puts $!
    nil
  end

  def raw_write(path, off, sz, buf)
    file = @files[path]
    return unless file
    file.seek(off, File::SEEK_SET)
    file.write(buf[0, sz])
  rescue
    puts $!
  end

  def raw_close(path)
    file = @files[path]
    return unless file
    file.close
    @files.delete path
  rescue
    puts $!
  end
end

if $0 == __FILE__
  dir = RemoteDirectory.new ARGV.shift || '.'
  uri = ARGV.shift || 'druby://0.0.0.0:7777'
  DRb.start_service uri, dir
  puts DRb.uri
  DRb.thread.join
end

