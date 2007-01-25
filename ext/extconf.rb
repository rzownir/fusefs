require 'mkmf'
dir_config('fusefs_lib.so')

unless have_library('fuse') 
  puts "No FUSE library found!"
  exit
end

# OS X boxes have statvfs.h instead of statfs.h
have_header('sys/statvfs.h')
have_header('sys/statfs.h')

# Ensure we have the fuse lib.
create_makefile('fusefs_lib')
