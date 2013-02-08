all:
	gcc -O2 kqueueserver.c -lpthread -Wall -o kqueueserver
	gcc -O2 kqueueserver2.c -lpthread -Wall -o kqueueserver2
	gcc -O2 kqueueserver3.c -lpthread -Wall -o kqueueserver3
	gcc -O2 kqueueserver4.c -lpthread -Wall -o kqueueserver4
	gcc -O2 kqueueserver5.c -lpthread -Wall -o kqueueserver5
	gcc -O2 kqueueserver6.c -lpthread -Wall -o kqueueserver6
#	gcc -O2 kqueueserver.c -lpthread -Wall -o kqueueserver

clean:
	rm -f kqueueserver kqueueserver2 kqueueserver3
