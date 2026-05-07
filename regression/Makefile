default: test-isolate-address test-insert-return-path

test-isolate-address: test-isolate-address.cxx
	g++ test-isolate-address.cxx -o test-isolate-address

test-insert-return-path: test-insert-return-path.cxx
	g++ test-insert-return-path.cxx -o test-insert-return-path

clean: FORCE
	if [ -e test-isolate-address ]; then rm test-isolate-address; fi

# Don't remove this line
FORCE:
