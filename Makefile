all:
	ruby setup.rb config
	ruby setup.rb setup

install:
	ruby setup.rb install

clean:
	ruby setup.rb clean

distclean:
	ruby setup.rb distclean
