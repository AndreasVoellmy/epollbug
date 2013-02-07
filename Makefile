all:
	gcc -O2 kqueueserver2.c -lpthread -Wall -o kqueueserver2
	gcc -O2 kqueueserver3.c -lpthread -Wall -o kqueueserver3
#	gcc -O2 kqueueserver.c -lpthread -Wall -o kqueueserver

clean:
	rm -f kqueueserver kqueueserver2 kqueueserver3
