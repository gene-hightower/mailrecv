VERSION = 1.27
SHELL   = /bin/sh
NROFF   = nroff
POD2MAN = pod2man --center "mailrecv Documentation"

# Default build
all: mailrecv man html

mailrecv: mailrecv.cpp
	g++ -DVERSION=\"$(VERSION)\" -Wall mailrecv.cpp -I/opt/homebrew/include -o mailrecv -l pcre -lpthread

clean: FORCE
	@echo Cleaning all files
	-rm -f core*
	-rm -f *.o
	-rm -f pod2*.tmp
	-rm -f mailrecv
	-rm -f mailrecv.8 mailrecv.conf.8
	-rm -f mailrecv.html mailrecv.conf.html
	( cd regression; make clean )

# Build man pages
man: mailrecv.pod mailrecv.conf.pod
	$(POD2MAN) --section=8 mailrecv.pod      > mailrecv.8
	$(POD2MAN) --section=8 mailrecv.conf.pod > mailrecv.conf.8

# Build html pages
html: mailrecv.pod mailrecv.conf.pod
	pod2html mailrecv.pod      | perl -w ./html-prefilter.pl mailrecv      > mailrecv.html
	pod2html mailrecv.conf.pod | perl -w ./html-prefilter.pl mailrecv.conf > mailrecv.conf.html
	cp mailrecv.html mailrecv.conf.html docs/

# GIT OPERATIONS
commit: FORCE
	@make clean
	@echo "------------------------------"
	@git status
	@echo "------------------------------"
	@read -p "Git status look OK? (^C to abort commit)"
	git add -u && git commit

# Run gitk
#    Disconnect it from the terminal
gitk: FORCE
	( nohup gitk > /dev/null 2>&1 < /dev/null & )

FORCE:
