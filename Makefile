#GCC=/usr/bin/gcc
GCC=gcc

simplefs: shell.o fs.o disk.o
	$(GCC) shell.o fs.o disk.o -o simplefs

shell.o: shell.c
	$(GCC) -Wall --std=c99 shell.c -c -o shell.o -g

fs.o: fs.c fs.h
	$(GCC) -Wall --std=c99 fs.c -c -o fs.o -g

disk.o: disk.c disk.h
	$(GCC) -Wall disk.c -c -o disk.o -g

clean:
	rm simplefs disk.o fs.o shell.o
