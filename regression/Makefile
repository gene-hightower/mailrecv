SHELL=/bin/sh

default: test-isolate-address test-insert-return-path pcre-test

test-isolate-address: test-isolate-address.cpp
	g++ -Wall test-isolate-address.cpp -o test-isolate-address

test-insert-return-path: test-insert-return-path.cpp
	g++ -Wall test-insert-return-path.cpp -o test-insert-return-path

pcre-test: pcre-test.cpp
	g++ -Wall -I/opt/homebrew/include pcre-test.cpp -o pcre-test -l pcre

clean: FORCE
	@if [ -e test-isolate-address ]; then rm test-isolate-address; echo Removed test-isolate-address; fi
	@if [ -e test-insert-return-path ]; then rm test-insert-return-path; echo Removed test-insert-return-path; fi
	@if [ -e pcre-test ]; then rm pcre-test; echo Removed pcre-test; fi

# Don't remove this line
FORCE:


FORCE:
