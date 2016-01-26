#include <stdio.h>
#include <stdint.h> // we use 32-bit words


void salsa20(uint8_t *message, uint64_t mlen, uint8_t key[32], uint64_t nonce);