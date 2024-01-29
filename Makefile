build:
	cc -O0 -g -w -I/usr/local/include/ -L/usr/local/lib -lSDL2 -ospaceinvaders ./src/*.c
run:
	./spaceinvaders

clean:
	rm spaceinvaders