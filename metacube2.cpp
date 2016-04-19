/*
 * Implementation of Metacube2 utility functions.
 *
 * Note: This file is meant to compile as both C and C++, for easier inclusion
 * in other projects.
 */

#include "metacube2.h"

/*
 * https://www.ece.cmu.edu/~koopman/pubs/KoopmanCRCWebinar9May2012.pdf
 * recommends this for messages as short as ours (see table at page 34).
 */
#define METACUBE2_CRC_POLYNOMIAL 0x8FDB

/* Semi-random starting value to make sure all-zero won't pass. */
#define METACUBE2_CRC_START 0x1234

/* This code is based on code generated by pycrc. */
uint16_t metacube2_compute_crc(const struct metacube2_block_header *hdr)
{
	static const int data_len = sizeof(hdr->size) + sizeof(hdr->flags);
	const uint8_t *data = (uint8_t *)&hdr->size;
	uint16_t crc = METACUBE2_CRC_START;
	int i, j;

	for (i = 0; i < data_len; ++i) {
		uint8_t c = data[i];
		for (j = 0; j < 8; j++) {
			int bit = crc & 0x8000;
			crc = (crc << 1) | ((c >> (7 - j)) & 0x01);
			if (bit) {
				crc ^= METACUBE2_CRC_POLYNOMIAL;
			}
		}
	}

	/* Finalize. */
	for (i = 0; i < 16; i++) {
		int bit = crc & 0x8000;
		crc = crc << 1;
		if (bit) {
			crc ^= METACUBE2_CRC_POLYNOMIAL;
		}
	}

	return crc;
}