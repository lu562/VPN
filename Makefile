INC=/usr/local/ssl/include/
LIB=/usr/local/ssl/lib/

all:
	gcc -I$(INC) -L$(LIB) simpletun.c -o simpletun -lssl -lcrypto -ldl -lpthread 
 
clean:
	rm -rf *~ cli serv
