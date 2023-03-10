CC=gcc
CFLAGS=-Wall -g
INCLUDE=-I. -I./headers/ -I./src/ -I./src/http-parser/

.PHONY: build clean

build: aws

aws: ./src/aws.o ./src/sock_util.o ./src/http-parser/http_parser.o
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $^ -laio

./src/aws.o: ./src/aws.c

./src/http-parser/http_parser.o: ./src/http-parser/http_parser.c ./src/http-parser/http_parser.h
	make -C ./src/http-parser http_parser.o

./src/sock_util.o: ./src/sock_util.c ./headers/sock_util.h ./headers/debug.h ./headers/util.h

clean:
	make -C ./src/http-parser/ clean
	rm -rf ./src/*.o aws
