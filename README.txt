1. Compile with make
gcc -Wall -ggdb -c memory.c
gcc -Wall -ggdb -c blockedq.c
gcc -Wall -ggdb master.c memory.o blockedq.o -o master
gcc -Wall -ggdb user.c -o user -lm

2. Run the program
$ ./master -c 7
$ cat log.txt
