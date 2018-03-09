#include <stdio.h>
#include <stdlib.h>

#define BLOCK_SIZE 8

/*
* Returns a char * of a hashed file.
*/
char *hash(FILE *file) {
	char t_val = '\0';
	char *hash = malloc(BLOCK_SIZE);
	int x;

	//initializes hash of size BLOCK_SIZE to all NULL chars
	for (x = 0; x < BLOCK_SIZE; x++){
		hash[x] = '\0';
	}
	
	x = 0;
	
	//reads in a file one byte at a time and bit wise xors it with 
	//a char in hash until no more bytes can be read
	while(fread(&t_val, 1, 1, file) != 0){
		hash[x] = hash[x] ^ t_val;
		x ++;
	
		if (x == 8){
			x = 0;
		}
	}
	
	return hash;
}