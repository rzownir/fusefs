FuseFS README
============

    FuseFS is a library aimed at allowing Ruby programmers to quickly and
  easily create virtual filesystems with little more than a few lines of code.
  
    A "hello world" file system equivalent to the one demonstrated on
  fuse.sourceforge.org is just 20 lines of code!

    FuseFS is *NOT* a full implementation of the FUSE api. rfuse
  is designed for that.


Requirements
------------

  * Linux 2.6
  * FUSE (http://fuse.sourceforge.org)
  * Ruby 1.8
 (* C compiler)


Install
-------

  De-compress archive and enter its top directory.
  Then type:

   ($ su)
    # ruby setup.rb

  These simple step installs this program under the default
  location of Ruby libraries.  You can also install files into
  your favorite directory by supplying setup.rb some options.
  Try "ruby setup.rb --help".


Usage
-----

  Some sample ruby filesystems are listed in "sample/"

  When you run a fusefs script, it will listen on a socket indefinitely, so
  either background the script or open another terminal to mosey around in the
  filesystem.

  Also, check the API.txt file for more use.


License
-------

  MIT license, in file "LICENSE"


Author: Greg Millam <walker@deafcode.com>.
