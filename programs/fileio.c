/*
  fileio.c - simple generic file i/o handler
  Copyright (C) Yann Collet 2013-2015

  GPL v2 License

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

  You can contact the author at :
  - FSE source repository : https://github.com/Cyan4973/FiniteStateEntropy
  - Public forum : https://groups.google.com/forum/#!forum/lz4c
  */
/*
  Note : this is stand-alone program.
  It is not part of FSE compression library, it is a user program of the FSE library.
  The license of FSE library is BSD.
  The license of this library is GPLv2.
  */

/**************************************
*  Compiler Options
**************************************/
/* Disable some Visual warning messages */
#ifdef _MSC_VER
#  define _CRT_SECURE_NO_WARNINGS
#  define _CRT_SECURE_NO_DEPRECATE     /* VS2005 */
#  pragma warning(disable : 4127)      /* disable: C4127: conditional expression is constant */
#endif

#define GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)

#define _FILE_OFFSET_BITS 64   /* Large file support on 32-bits unix */
#define _POSIX_SOURCE 1        /* enable fileno() within <stdio.h> on unix */


/**************************************
*  Includes
**************************************/
#include <stdio.h>    /* fprintf, fopen, fread, _fileno, stdin, stdout */
#include <stdlib.h>   /* malloc, free */
#include <string.h>   /* strcmp, strlen */
#include <time.h>     /* clock */
#include "fileio.h"
#include "fse.h"
#include "huff0.h"
#include "zlibh.h"    /*ZLIBH_compress */
#include "xxhash.h"

#include "isaac64\isaac64.h";
#include "isaac64\standard.h";

#include <errno.h>      /* errno */
#include <sys/types.h>  /* stat64 */
#include <sys/stat.h>   /* stat64 */
#include "mem.h"
//#include "fileiozstd.h"
#include "zstd/zstd_static.h"   /* ZSTD_magicNumber */
#include "zstd/zstd_buffered_static.h"

#include "salsa20/salsa20.h"

#if defined(ZSTD_LEGACY_SUPPORT) && (ZSTD_LEGACY_SUPPORT==1)
#  include "zstd_legacy.h"    /* legacy */
#  include "fileio_legacy.h"  /* legacy */
#endif


/**************************************
*  OS-specific Includes
**************************************/
#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(_WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>    // _O_BINARY
#  include <io.h>       // _setmode, _isatty
#  ifdef __MINGW32__
int _fileno(FILE *stream);   // MINGW somehow forgets to include this windows declaration into <stdio.h>
#  endif
#  define SET_BINARY_MODE(file) { int unused = _setmode(_fileno(file), _O_BINARY); (void)unused; }
#  define IS_CONSOLE(stdStream) _isatty(_fileno(stdStream))
#else
#  include <unistd.h>   // isatty
#  define SET_BINARY_MODE(file)
#  define IS_CONSOLE(stdStream) isatty(fileno(stdStream))
#endif


#if !defined(S_ISREG)
#  define S_ISREG(x) (((x) & S_IFMT) == S_IFREG)
#endif

/**************************************
*  Basic Types
**************************************/
#if defined (__STDC_VERSION__) && __STDC_VERSION__ >= 199901L   /* C99 */
# include <stdint.h>
typedef uint8_t  BYTE;
typedef uint16_t U16;
typedef uint32_t U32;
typedef  int32_t S32;
typedef uint64_t U64;
#else
typedef unsigned char       BYTE;
typedef unsigned short      U16;
typedef unsigned int        U32;
typedef   signed int        S32;
typedef unsigned long long  U64;
#endif


/**************************************
*  Constants
**************************************/
#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)

#define _1BIT  0x01
#define _2BITS 0x03
#define _3BITS 0x07
#define _4BITS 0x0F
#define _6BITS 0x3F
#define _8BITS 0xFF

#define BIT5  0x20
#define BIT6  0x40
#define BIT7  0x80

#define FIO_magicNumber_fse   0x183E2309
#define FIO_magicNumber_huff0 0x183E3309
#define FIO_magicNumber_zlibh 0x183E4309
static const unsigned FIO_maxBlockSizeID = 6;   /* => 64 KB block */
static const unsigned FIO_maxBlockHeaderSize = 5;

#define FIO_FRAMEHEADERSIZE 5        /* as a define, because needed to allocated table on stack */
#define FIO_BLOCKSIZEID_DEFAULT  5   /* as a define, because needed to init static g_blockSizeId */
#define FSE_CHECKSUM_SEED        0

/* ZSTD DICT SIZE */
#define MAX_DICT_SIZE (512 KB)

#define CACHELINE 64
#define MAGICNUMBERSIZE 8

/**************************************
*  Complex types
**************************************/
typedef enum { bt_compressed, bt_raw, bt_rle, bt_crc } bType_t;


/**************************************
*  Memory operations
**************************************/
static void FIO_writeLE32(void* memPtr, U32 val32)
{
	BYTE* p = (BYTE*)memPtr;
	p[0] = (BYTE)val32;
	p[1] = (BYTE)(val32 >> 8);
	p[2] = (BYTE)(val32 >> 16);
	p[3] = (BYTE)(val32 >> 24);
}

static void FIO_writeLE64(void* memPtr, ub8 val64)
{
	BYTE* p = (BYTE*)memPtr;
	p[0] = (BYTE)val64;
	p[1] = (BYTE)(val64 >> 8);
	p[2] = (BYTE)(val64 >> 16);
	p[3] = (BYTE)(val64 >> 24);
	p[4] = (BYTE)(val64 >> 32);
	p[5] = (BYTE)(val64 >> 40);
	p[6] = (BYTE)(val64 >> 48);
	p[7] = (BYTE)(val64 >> 56);
}


static U32 FIO_readLE32(const void* memPtr)
{
	const BYTE* p = (const BYTE*)memPtr;
	return (U32)((U32)p[0] + ((U32)p[1] << 8) + ((U32)p[2] << 16) + ((U32)p[3] << 24));
}

static ub8 FIO_readLE64(const void* memPtr)
{
	const BYTE* p = (const BYTE*)memPtr;
	return (ub8)((ub8)p[0] + ((ub8)p[1] << 8) + ((ub8)p[2] << 16) + ((ub8)p[3] << 24) + ((ub8)p[4] << 32) + ((ub8)p[5] << 40) + ((ub8)p[6] << 48) + ((ub8)p[7] << 56));
}


/**************************************
*  Macros
**************************************/
#define DISPLAY(...)         fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) if (g_displayLevel>=l) { DISPLAY(__VA_ARGS__); }
static U32 g_displayLevel = 2;   /* 0 : no display;   1: errors;   2 : + result + interaction + warnings;   3 : + progression;   4 : + information */

#define DISPLAYUPDATE(l, ...) if (g_displayLevel>=l) { \
if ((FIO_GetMilliSpan(g_time) > refreshRate) || (g_displayLevel >= 4)) \
{ g_time = clock(); DISPLAY(__VA_ARGS__); \
if (g_displayLevel >= 4) fflush(stdout); } }
static const unsigned refreshRate = 150;
static clock_t g_time = 0;


/**************************************
*  Local Parameters
**************************************/
static U32 g_overwrite = 0;
static U32 g_blockSizeId = FIO_BLOCKSIZEID_DEFAULT;
FIO_compressor_t g_compressor = FIO_fse;

void FIO_overwriteMode(void) { g_overwrite = 1; }
void FIO_setCompressor(FIO_compressor_t c) { g_compressor = c; }


/**************************************
*  Exceptions
**************************************/
#define DEBUG 0
#define DEBUGOUTPUT(...) if (DEBUG) DISPLAY(__VA_ARGS__);
#define EXM_THROW(error, ...)                                             \
{                                                                         \
	DEBUGOUTPUT("Error defined at %s, line %i : \n", __FILE__, __LINE__); \
	DISPLAYLEVEL(1, "Error %i : ", error);                                \
	DISPLAYLEVEL(1, __VA_ARGS__);                                         \
	DISPLAYLEVEL(1, "\n");                                                \
	exit(error);                                                          \
}


/**************************************
*  Version modifiers
**************************************/
#define DEFAULT_COMPRESSOR    FSE_compress
#define DEFAULT_DECOMPRESSOR  FSE_decompress


/**************************************
*  Functions
**************************************/
static unsigned FIO_GetMilliSpan(clock_t nPrevious)
{
	clock_t nCurrent = clock();
	unsigned nSpan = (unsigned)(((nCurrent - nPrevious) * 1000) / CLOCKS_PER_SEC);
	return nSpan;
}

static int FIO_blockID_to_blockSize(int id) { return (1 << id) KB; }
size_t FIO_loadFile(void** dict_buffer, const char* dict_file_name);
int FIO_getFiles(FILE** dst_file, FILE** src_file, const char* dst_file_name, const char* src_file_name);

/**************************************
*  Salsa20 arguments
**************************************/
uint8_t key[32] = { 0 };
uint64_t nonce = 0;


static void get_fileHandle(const char* input_filename, const char* output_filename, FILE** pfinput, FILE** pfoutput)
{
	
	if (!strcmp(input_filename, stdinmark))
	{
		DISPLAYLEVEL(4, "Using stdin for input\n");
		*pfinput = stdin;
		SET_BINARY_MODE(stdin);
	}
	else
	{
		*pfinput = fopen(input_filename, "rb");
	}

	if (!strcmp(output_filename, stdoutmark))
	{
		DISPLAYLEVEL(4, "Using stdout for output\n");
		*pfoutput = stdout;
		SET_BINARY_MODE(stdout);
	}
	else
	{
		/* Check if destination file already exists */
		*pfoutput = 0;
		if (strcmp(output_filename, nulmark)) *pfoutput = fopen(output_filename, "rb");
		if (*pfoutput != 0)
		{
			fclose(*pfoutput);
			if (!g_overwrite)
			{
				char ch;
				if (g_displayLevel <= 1)   /* No interaction possible */
					EXM_THROW(11, "Operation aborted : %s already exists", output_filename);
				DISPLAYLEVEL(2, "Warning : %s already exists\n", output_filename);
				DISPLAYLEVEL(2, "Overwrite ? (Y/N) : ");
				ch = (char)getchar();
				if ((ch != 'Y') && (ch != 'y')) EXM_THROW(11, "Operation aborted : %s already exists", output_filename);
			}
		}
		*pfoutput = fopen(output_filename, "wb");
	}

	if (*pfinput == 0) EXM_THROW(12, "Pb opening %s", input_filename);
	if (*pfoutput == 0) EXM_THROW(13, "Pb opening %s", output_filename);
}


size_t FIO_ZLIBH_compress(void* dst, size_t dstSize, const void* src, size_t srcSize, unsigned scrambler)
{
	if (scrambler) 	{}
	(void)dstSize;
	return (size_t)ZLIBH_compress((char*)dst, (const char*)src, (int)srcSize);
}

static int password_length = 0;

static unsigned simlple_scrambler(const char * password, int index)
{
	return abs(getNumber64ForPassword(password + index)) % (unsigned)password[index % password_length];
}

static unsigned empty_scrambler(const char * password, int index)
{
	return 0;
}


static U64 FIO_getFileSize2(const char* infilename)
{
	int r;
#if defined(_MSC_VER)
	struct _stat64 statbuf;
	r = _stat64(infilename, &statbuf);
#else
	struct stat statbuf;
	r = stat(infilename, &statbuf);
#endif
	if (r || !S_ISREG(statbuf.st_mode)) return 0;
	return (U64)statbuf.st_size;
}

/*
Compressed format : MAGICNUMBER - STREAMDESCRIPTOR - ( BLOCKHEADER - COMPRESSEDBLOCK ) - STREAMCRC
MAGICNUMBER - 4 bytes - Designates compression algo
STREAMDESCRIPTOR - 1 byte
bits 0-3 : max block size, 2^value from 0 to 0xA; min 0=>1KB, max 0x6=>64KB, typical 5=>32 KB
bits 4-7 = 0 : reserved;
BLOCKHEADER - 1-5 bytes
1st byte :
bits 6-7 : blockType (compressed, raw, rle, crc (end of Frame)
bit 5 : full block
** if not full block **
2nd & 3rd byte : regenerated size of block (big endian); note : 0 = 64 KB
** if blockType==compressed **
next 2 bytes : compressed size of block
COMPRESSEDBLOCK
the compressed data itself.
STREAMCRC - 3 bytes (including 1-byte blockheader)
22 bits (xxh32() >> 5) checksum of the original data, big endian
*/
unsigned long long FIO_compressFilename(const char* output_filename, const char* input_filename, const char* password)
{
	U64 filesize = 0;
	U64 compressedfilesize = 0;
	char* in_buff;
	char* out_buff;
	FILE* finput;
	FILE* foutput;
	size_t sizeCheck;
	size_t inputBlockSize = FIO_blockID_to_blockSize(g_blockSizeId);
	XXH32_state_t xxhState;
	typedef size_t(*compressor_t) (void* dst, size_t dstSize, const void* src, size_t srcSize, unsigned scrambler);
	compressor_t compressor;
	unsigned magicNumber;

	/* Init */
	XXH32_reset(&xxhState, FSE_CHECKSUM_SEED);
	get_fileHandle(input_filename, output_filename, &finput, &foutput);

	switch (g_compressor)
	{
	case FIO_fse:
		compressor = FSE_compress;
		magicNumber = FIO_magicNumber_fse;
		break;
	case FIO_huff0:
		compressor = HUF_compress;
		magicNumber = FIO_magicNumber_huff0;
		break;
	case FIO_zlibh:
		compressor = FIO_ZLIBH_compress;
		magicNumber = FIO_magicNumber_zlibh;
		break;
	default:
		EXM_THROW(20, "unknown compressor selection");
	}

	/* Allocate Memory */
	if (inputBlockSize == 0) EXM_THROW(0, "impossible problem, to please static analyzer");
	in_buff = (char*)malloc(inputBlockSize);
	out_buff = (char*)malloc(FSE_compressBound(inputBlockSize) + 5);
	if (!in_buff || !out_buff) EXM_THROW(21, "Allocation error : not enough memory");

	////RC4 keystream encrypt attempt
	unsigned char state[256], key[] = { "keykey" }, stream[1024];
	int len = 9, idx;
	ksa(state, key, 64);
	prga(state, stream, len);


	/* Write Frame Header */
	FIO_writeLE32(out_buff, magicNumber);
	//FIO_writeLE64(out_buff, getNumber64());
	out_buff[4] = (char)g_blockSizeId;          /* Max Block Size descriptor */
	sizeCheck = fwrite(out_buff, 1, FIO_FRAMEHEADERSIZE, foutput);
	if (sizeCheck != FIO_FRAMEHEADERSIZE) EXM_THROW(22, "Write error : cannot write header");
	compressedfilesize += FIO_FRAMEHEADERSIZE;

	unsigned index = 0;
	unsigned(*scrambler_func)(const char *, int);
	if (password != NULL)
	{
		password_length = strlen(password);
		scrambler_func = simlple_scrambler;
	}
	else
	{
		scrambler_func = empty_scrambler;
	}

	FIO_writeLE64(out_buff, getNumber64());



	/* Main compression loop */
	while (1)
	{
		/* Fill input Buffer */
		size_t cSize;
		size_t inSize = fread(in_buff, (size_t)1, (size_t)inputBlockSize, finput);

		// Salsa20 encryption
		salsa20(in_buff, sizeof(in_buff), key, nonce);

		if (inSize == 0) break;
		filesize += inSize;
		XXH32_update(&xxhState, in_buff, inSize);
		DISPLAYUPDATE(2, "\rRead : %u MB ", (U32)(filesize >> 20));

		/* Compress Block */
		cSize = compressor(out_buff + FIO_maxBlockHeaderSize, FSE_compressBound(inputBlockSize), in_buff, inSize, scrambler_func(password, index++));
		if (FSE_isError(cSize)) EXM_THROW(23, "Compression error : %s ", FSE_getErrorName(cSize));

		/* Write cBlock */
		switch (cSize)
		{
			size_t headerSize;
		case 0: /* raw */
			if (inSize == inputBlockSize)
			{
				out_buff[0] = (BYTE)((bt_raw << 6) + BIT5);
				headerSize = 1;
			}
			else
			{
				out_buff[2] = (BYTE)inSize;
				out_buff[1] = (BYTE)(inSize >> 8);
				out_buff[0] = (BYTE)(bt_raw << 6);
				headerSize = 3;
			}
			sizeCheck = fwrite(out_buff, 1, headerSize, foutput);
			if (sizeCheck != headerSize) EXM_THROW(24, "Write error : cannot write block header");
			sizeCheck = fwrite(in_buff, 1, inSize, foutput);
			if (sizeCheck != (size_t)(inSize)) EXM_THROW(25, "Write error : cannot write block");
			compressedfilesize += inSize + headerSize;
			break;
		case 1: /* rle */
			if (inSize == inputBlockSize)
			{
				out_buff[0] = (BYTE)((bt_rle << 6) + BIT5);
				headerSize = 1;
			}
			else
			{
				out_buff[2] = (BYTE)inSize;
				out_buff[1] = (BYTE)(inSize >> 8);
				out_buff[0] = (BYTE)(bt_raw << 6);
				headerSize = 3;
			}
			out_buff[headerSize] = in_buff[0];
			sizeCheck = fwrite(out_buff, 1, headerSize + 1, foutput);
			if (sizeCheck != (headerSize + 1)) EXM_THROW(26, "Write error : cannot write rle block");
			compressedfilesize += headerSize + 1;
			break;
		default: /* compressed */
			if (inSize == inputBlockSize)
			{
				out_buff[2] = (BYTE)((bt_compressed << 6) + BIT5);
				out_buff[3] = (BYTE)(cSize >> 8);
				out_buff[4] = (BYTE)cSize;
				headerSize = 3;
			}
			else
			{
				out_buff[0] = (BYTE)(bt_compressed << 6);
				out_buff[1] = (BYTE)(inSize >> 8);
				out_buff[2] = (BYTE)inSize;
				out_buff[3] = (BYTE)(cSize >> 8);
				out_buff[4] = (BYTE)cSize;
				headerSize = FIO_maxBlockHeaderSize;
			}
			sizeCheck = fwrite(out_buff + (FIO_maxBlockHeaderSize - headerSize), 1, headerSize + cSize, foutput);
			if (sizeCheck != (headerSize + cSize)) EXM_THROW(27, "Write error : cannot write rle block");
			compressedfilesize += headerSize + cSize;
			break;
		}

		DISPLAYUPDATE(2, "\rRead : %u MB  ==> %.2f%%   ", (U32)(filesize >> 20), (double)compressedfilesize / filesize * 100);
	}

	/* Checksum */
	{
		U32 checksum = XXH32_digest(&xxhState);
		checksum = (checksum >> 5) & ((1U << 22) - 1);
		out_buff[2] = (BYTE)checksum;
		out_buff[1] = (BYTE)(checksum >> 8);
		out_buff[0] = (BYTE)((checksum >> 16) + (bt_crc << 6));
		sizeCheck = fwrite(out_buff, 1, 3, foutput);
		if (sizeCheck != 3) EXM_THROW(28, "Write error : cannot write checksum");
		compressedfilesize += 3;
	}

	/* Status */
	DISPLAYLEVEL(2, "\r%79s\r", "");
	DISPLAYLEVEL(2, "Compressed %llu bytes into %llu bytes ==> %.2f%%\n",
		(unsigned long long) filesize, (unsigned long long) compressedfilesize, (double)compressedfilesize / filesize * 100);

	/* clean */
	free(in_buff);
	free(out_buff);
	fclose(finput);
	fclose(foutput);

	return compressedfilesize;
}



size_t FIO_ZLIBH_decompress(void* dst, size_t dstSize, const void* src, size_t srcSize, unsigned scrambler)
{
	if (scrambler)
	{

	}
	(void)srcSize; (void)dstSize;
	return (size_t)ZLIBH_decompress((char*)dst, (const char*)src);
}

/*
Compressed format : MAGICNUMBER - STREAMDESCRIPTOR - ( BLOCKHEADER - COMPRESSEDBLOCK ) - STREAMCRC
MAGICNUMBER - 4 bytes - Designates compression algo
STREAMDESCRIPTOR - 1 byte
bits 0-3 : max block size, 2^value from 0 to 0xA; min 0=>1KB, max 0x6=>64KB, typical 5=>32 KB
bits 4-7 = 0 : reserved;
BLOCKHEADER - 1-5 bytes
1st byte :
bits 6-7 : blockType (compressed, raw, rle, crc (end of Frame)
bit 5 : full block
** if not full block **
2nd & 3rd byte : regenerated size of block (big endian); note : 0 = 64 KB
** if blockType==compressed **
next 2 bytes : compressed size of block
COMPRESSEDBLOCK
the compressed data itself.
STREAMCRC - 3 bytes (including 1-byte blockheader)
22 bits (xxh32() >> 5) checksum of the original data, big endian
*/
unsigned long long FIO_decompressFilename(const char* output_filename, const char* input_filename, const char* password)
{
	FILE* finput, *foutput;
	U64   filesize = 0;
	U32   header32[(FIO_FRAMEHEADERSIZE + 3) >> 2];
	BYTE* header = (BYTE*)header32;
	BYTE* in_buff;
	BYTE* out_buff;
	BYTE* ip;
	U32   blockSize;
	U32   blockSizeId;
	size_t sizeCheck;
	U32   magicNumber;
	U32*  magicNumberP = header32;
	size_t inputBufferSize;
	XXH32_state_t xxhState;
	typedef size_t(*decompressor_t) (void* dst, size_t dstSize, const void* src, size_t srcSize, unsigned);
	decompressor_t decompressor = FSE_decompress;

	ub8 randomNumber;
	ub8 randomNumberP = header32;

	/* Init */
	XXH32_reset(&xxhState, FSE_CHECKSUM_SEED);
	get_fileHandle(input_filename, output_filename, &finput, &foutput);

	/* check header */
	sizeCheck = fread(header, (size_t)1, FIO_FRAMEHEADERSIZE, finput);
	if (sizeCheck != FIO_FRAMEHEADERSIZE) EXM_THROW(30, "Read error : cannot read header\n");

	magicNumber = FIO_readLE32(magicNumberP);
	

	switch (magicNumber)
	{
	case FIO_magicNumber_fse:
		decompressor = FSE_decompress;
		break;
	case FIO_magicNumber_huff0:
		decompressor = HUF_decompress;
		break;
	case FIO_magicNumber_zlibh:
		decompressor = FIO_ZLIBH_decompress;
		break;
	default:
		EXM_THROW(31, "Wrong file type : unknown header\n");
	}

	blockSizeId = header[4];
	if (blockSizeId > FIO_maxBlockSizeID) EXM_THROW(32, "Wrong version : unknown header flags\n");
	blockSize = FIO_blockID_to_blockSize(blockSizeId);

	/* Allocate Memory */
	inputBufferSize = blockSize + FIO_maxBlockHeaderSize;
	in_buff = (BYTE*)malloc(inputBufferSize);
	out_buff = (BYTE*)malloc(blockSize);
	if (!in_buff || !out_buff) EXM_THROW(33, "Allocation error : not enough memory");
	ip = in_buff;

	/* read first bHeader */
	sizeCheck = fread(in_buff, 1, 1, finput);
	if (sizeCheck != 1) EXM_THROW(34, "Read error : cannot read header\n");
	/* Main Loop */

	unsigned index = 0;
	unsigned(*scrambler_func)(const char *, int);
	if (password != NULL)
	{
		password_length = strlen(password);
		scrambler_func = simlple_scrambler;
	}
	else
	{
		scrambler_func = empty_scrambler;
	}

	randomNumber = FIO_readLE64(randomNumberP);

	while (1)
	{
		size_t toReadSize, readSize, bType, rSize = 0, cSize;
		//static U32 blockNb=0;
		//printf("blockNb = %u \n", ++blockNb);

		/* Decode header */
		bType = (ip[0] & (BIT7 + BIT6)) >> 6;
		if (bType == bt_crc) break;   /* end - frame content CRC */
		rSize = blockSize;
		if (!(ip[0] & BIT5))   /* non full block */
		{
			sizeCheck = fread(in_buff, 1, 2, finput);
			if (sizeCheck != 2) EXM_THROW(35, "Read error : cannot read header\n");
			rSize = (in_buff[0] << 8) + in_buff[1];
		}

		switch (bType)
		{
		case bt_compressed:
			sizeCheck = fread(in_buff, 1, 2, finput);
			if (sizeCheck != 2) EXM_THROW(36, "Read error : cannot read header\n");
			cSize = (in_buff[0] << 8) + in_buff[1];
			break;
		case bt_raw:
			cSize = rSize;
			break;
		case bt_rle:
			cSize = 1;
			break;
		default:
			EXM_THROW(37, "unknown block header");   /* should not happen */
		}

		/* Fill input buffer */
		toReadSize = cSize + 1;
		readSize = fread(in_buff, 1, toReadSize, finput);
		if (readSize != toReadSize) EXM_THROW(38, "Read error");
		ip = in_buff + cSize;

		/* Decode block */
		switch (bType)
		{
		case bt_compressed:
			//rSize = decompressor(out_buff, rSize, in_buff, cSize, scrambler);
			rSize = decompressor(out_buff, rSize, in_buff, cSize, scrambler_func(password, index++));
			if (FSE_isError(rSize)) EXM_THROW(39, "Decoding error : %s", FSE_getErrorName(rSize));
			break;
		case bt_raw:
			/* will read directly from in_buff, so no need to memcpy */
			break;
		case bt_rle:
			memset(out_buff, in_buff[0], rSize);
			break;
		default:
			EXM_THROW(40, "unknown block header");   /* should not happen */
		}

		// Salsa20 decryption
		salsa20(out_buff, sizeof(out_buff), key, nonce);


		/* Write block */
		switch (bType)
		{
			size_t writeSizeCheck;

		case bt_compressed:
		case bt_rle:
			writeSizeCheck = fwrite(out_buff, 1, rSize, foutput);
			if (writeSizeCheck != rSize) EXM_THROW(41, "Write error : unable to write data block to destination file");
			XXH32_update(&xxhState, out_buff, rSize);
			filesize += rSize;
			break;
		case bt_raw:
			writeSizeCheck = fwrite(in_buff, 1, cSize, foutput);
			if (writeSizeCheck != cSize) EXM_THROW(42, "Write error : unable to write data block to destination file");
			XXH32_update(&xxhState, in_buff, cSize);
			filesize += cSize;
			break;
		default:
			EXM_THROW(41, "unknown block header");   /* should not happen */
		}
	}

	/* CRC verification */
	sizeCheck = fread(ip + 1, 1, 2, finput);
	if (sizeCheck != 2) EXM_THROW(43, "Read error");
	{
		U32 CRCsaved = ip[2] + (ip[1] << 8) + ((ip[0] & _6BITS) << 16);
		U32 CRCcalculated = (XXH32_digest(&xxhState) >> 5) & ((1U << 22) - 1);
		//if (CRCsaved != CRCcalculated) EXM_THROW(44, "CRC error : wrong checksum, corrupted data");
	}

	DISPLAYLEVEL(2, "\r%79s\r", "");
	DISPLAYLEVEL(2, "Decoded %llu bytes \n", (long long unsigned)filesize);

	/* clean */
	free(in_buff);
	free(out_buff);
	fclose(finput);
	fclose(foutput);

	return filesize;
}





/* **********************************************************************
*  Compression
************************************************************************/
typedef struct {
	void*  srcBuffer;
	size_t srcBufferSize;
	void*  dstBuffer;
	size_t dstBufferSize;
	void*  dictBuffer;
	size_t dictBufferSize;
	ZBUFF_CCtx* ctx;
} cRess_t;

static cRess_t FIO_createCResources(const char* dictFileName)
{
	cRess_t ress;

	ress.ctx = ZBUFF_createCCtx();
	if (ress.ctx == NULL) EXM_THROW(30, "Allocation error : can't create ZBUFF context");

	/* Allocate Memory */
	ress.srcBufferSize = ZBUFF_recommendedCInSize();
	ress.srcBuffer = malloc(ress.srcBufferSize);
	ress.dstBufferSize = ZBUFF_recommendedCOutSize();
	ress.dstBuffer = malloc(ress.dstBufferSize);
	if (!ress.srcBuffer || !ress.dstBuffer) EXM_THROW(31, "Allocation error : not enough memory");

	/* dictionary */
	ress.dictBufferSize = FIO_loadFile(&(ress.dictBuffer), dictFileName);

	return ress;
}

static void FIO_freeCResources(cRess_t ress)
{
	size_t errorCode;
	free(ress.srcBuffer);
	free(ress.dstBuffer);
	free(ress.dictBuffer);
	errorCode = ZBUFF_freeCCtx(ress.ctx);
	if (ZBUFF_isError(errorCode)) EXM_THROW(38, "Error : can't release ZBUFF context resource : %s", ZBUFF_getErrorName(errorCode));
}


/*
* FIO_compressZstdFilename_extRess()
* result : 0 : compression completed correctly
*          1 : missing or pb opening srcFileName
*/
static int FIO_compressZstdFilename_extRess(cRess_t ress,
	const char* dstFileName, const char* srcFileName,
	int cLevel, const char* passwordValue)
{
	FILE* srcFile;
	FILE* dstFile;
	U64 filesize = 0;
	U64 compressedfilesize = 0;
	size_t dictSize = ress.dictBufferSize;
	size_t sizeCheck, errorCode;

	/* File check */
	if (FIO_getFiles(&dstFile, &srcFile, dstFileName, srcFileName)) return 1;

	/* init */
	filesize = FIO_getFileSize2(srcFileName) + dictSize;
	errorCode = ZBUFF_compressInit_advanced(ress.ctx, ZSTD_getParams(cLevel, filesize));
	if (ZBUFF_isError(errorCode)) EXM_THROW(21, "Error initializing compression");
	errorCode = ZBUFF_compressWithDictionary(ress.ctx, ress.dictBuffer, ress.dictBufferSize);
	if (ZBUFF_isError(errorCode)) EXM_THROW(22, "Error initializing dictionary");

	unsigned(*scrambler_func)(const char *, int);
	unsigned index = 0;
	if (passwordValue != NULL)
	{
		password_length = strlen(passwordValue);
		scrambler_func = simlple_scrambler;
	}
	else
	{
		scrambler_func = empty_scrambler;
	}

	/* Main compression loop */
	filesize = 0;
	while (1)
	{
		size_t inSize;
		// changePasswordValue to unsigned 

		unsigned scrambler = scrambler_func(passwordValue, index++);

		// Salsa20 encryption - ZSTD
		salsa20(ress.srcBuffer, sizeof(ress.srcBuffer), key, nonce);

		/* Fill input Buffer */
		inSize = fread(ress.srcBuffer, (size_t)1, ress.srcBufferSize, srcFile);
		if (inSize == 0) break;
		filesize += inSize;
		DISPLAYUPDATE(2, "\rRead : %u MB  ", (U32)(filesize >> 20));

		{
			/* Compress (buffered streaming ensures appropriate formatting) */
			size_t usedInSize = inSize;
			size_t cSize = ress.dstBufferSize;
			size_t result = ZBUFF_compressContinue(ress.ctx, ress.dstBuffer, &cSize, ress.srcBuffer, &usedInSize, scrambler);
			if (ZBUFF_isError(result))
				EXM_THROW(23, "Compression error : %s ", ZBUFF_getErrorName(result));
			if (inSize != usedInSize)
				/* inBuff should be entirely consumed since buffer sizes are recommended ones */
				EXM_THROW(24, "Compression error : input block not fully consumed");

			/* Write cBlock */
			sizeCheck = fwrite(ress.dstBuffer, 1, cSize, dstFile);
			if (sizeCheck != cSize) EXM_THROW(25, "Write error : cannot write compressed block into %s", dstFileName);
			compressedfilesize += cSize;
		}

		DISPLAYUPDATE(2, "\rRead : %u MB  ==> %.2f%%   ", (U32)(filesize >> 20), (double)compressedfilesize / filesize * 100);
	}

	/* End of Frame */
	{
		size_t cSize = ress.dstBufferSize;
		size_t result = ZBUFF_compressEnd(ress.ctx, ress.dstBuffer, &cSize);
		if (result != 0) EXM_THROW(26, "Compression error : cannot create frame end");

		sizeCheck = fwrite(ress.dstBuffer, 1, cSize, dstFile);
		if (sizeCheck != cSize) EXM_THROW(27, "Write error : cannot write frame end into %s", dstFileName);
		compressedfilesize += cSize;
	}

	/* Status */
	DISPLAYLEVEL(2, "\r%79s\r", "");
	DISPLAYLEVEL(2, "Compressed %llu bytes into %llu bytes ==> %.2f%%\n",
		(unsigned long long) filesize, (unsigned long long) compressedfilesize, (double)compressedfilesize / filesize * 100);

	/* clean */
	fclose(srcFile);
	if (fclose(dstFile)) EXM_THROW(28, "Write error : cannot properly close %s", dstFileName);

	return 0;
}


int FIO_compressZstdFilename(const char* dstFileName, const char* srcFileName,
	const char* dictFileName, int compressionLevel, const char* passwordValue)
{
	clock_t start, end;
	cRess_t ress;
	int issueWithSrcFile = 0;

	/* Init */
	start = clock();
	ress = FIO_createCResources(dictFileName);

	/* Compress File */
	issueWithSrcFile += FIO_compressZstdFilename_extRess(ress, dstFileName, srcFileName, compressionLevel, passwordValue);

	/* Free resources */
	FIO_freeCResources(ress);

	/* Final Status */
	end = clock();
	{
		double seconds = (double)(end - start) / CLOCKS_PER_SEC;
		DISPLAYLEVEL(4, "Completed in %.2f sec \n %.s", seconds, passwordValue);
	}

	return issueWithSrcFile;
}


#define FNSPACE 30
int FIO_compressMultipleFilenames(const char** inFileNamesTable, unsigned nbFiles,
	const char* suffix,
	const char* dictFileName, int compressionLevel)
{
	unsigned u;
	int missed_files = 0;
	char* dstFileName = (char*)malloc(FNSPACE);
	size_t dfnSize = FNSPACE;
	const size_t suffixSize = strlen(suffix);
	cRess_t ress;

	/* init */
	ress = FIO_createCResources(dictFileName);

	/* loop on each file */
	for (u = 0; u<nbFiles; u++)
	{
		size_t ifnSize = strlen(inFileNamesTable[u]);
		if (dfnSize <= ifnSize + suffixSize + 1) { free(dstFileName); dfnSize = ifnSize + 20; dstFileName = (char*)malloc(dfnSize); }
		strcpy(dstFileName, inFileNamesTable[u]);
		strcat(dstFileName, suffix);

		missed_files += FIO_compressZstdFilename_extRess(ress, dstFileName, inFileNamesTable[u], compressionLevel, "TODO passwordValue");
	}

	/* Close & Free */
	FIO_freeCResources(ress);
	free(dstFileName);

	return missed_files;
}


/* **************************************************************************
*  Decompression
****************************************************************************/
typedef struct {
	void*  srcBuffer;
	size_t srcBufferSize;
	void*  dstBuffer;
	size_t dstBufferSize;
	void*  dictBuffer;
	size_t dictBufferSize;
	ZBUFF_DCtx* dctx;
} dRess_t;



static dRess_t FIO_createDResources(const char* dictFileName)
{
	dRess_t ress;

	/* init */
	ress.dctx = ZBUFF_createDCtx();
	if (ress.dctx == NULL) EXM_THROW(60, "Can't create ZBUFF decompression context");

	/* Allocate Memory */
	ress.srcBufferSize = ZBUFF_recommendedDInSize();
	ress.srcBuffer = malloc(ress.srcBufferSize);
	ress.dstBufferSize = ZBUFF_recommendedDOutSize();
	ress.dstBuffer = malloc(ress.dstBufferSize);
	if (!ress.srcBuffer || !ress.dstBuffer) EXM_THROW(61, "Allocation error : not enough memory");

	/* dictionary */
	ress.dictBufferSize = FIO_loadFile(&(ress.dictBuffer), dictFileName);

	return ress;
}

static void FIO_freeDResources(dRess_t ress)
{
	size_t errorCode = ZBUFF_freeDCtx(ress.dctx);
	if (ZBUFF_isError(errorCode)) EXM_THROW(69, "Error : can't free ZBUFF context resource : %s", ZBUFF_getErrorName(errorCode));
	free(ress.srcBuffer);
	free(ress.dstBuffer);
	free(ress.dictBuffer);
}


unsigned long long FIO_decompressFrame(dRess_t ress,
	FILE* foutput, FILE* finput, size_t alreadyLoaded, const char* passwordValue)
{
	U64    frameSize = 0;
	size_t readSize = alreadyLoaded;

	/* Main decompression Loop */
	ZBUFF_decompressInit(ress.dctx);
	ZBUFF_decompressWithDictionary(ress.dctx, ress.dictBuffer, ress.dictBufferSize);

	unsigned(*scrambler_func)(const char *, int);
	unsigned index = 0;
	if (passwordValue != NULL)
	{
		password_length = strlen(passwordValue);
		scrambler_func = simlple_scrambler;
	}
	else
	{
		scrambler_func = empty_scrambler;
	}

	// Salsa20 decryption - ZSTD
	salsa20(ress.dstBuffer, sizeof(ress.dstBuffer), key, nonce);
	while (1)
	{
		unsigned scrambler = scrambler_func(passwordValue, index++);
		/* Decode */
		size_t sizeCheck;
		size_t inSize = readSize, decodedSize = ress.dstBufferSize;
		size_t toRead = ZBUFF_decompressContinue(ress.dctx, ress.dstBuffer, &decodedSize, ress.srcBuffer, &inSize, scrambler);
		if (ZBUFF_isError(toRead)) EXM_THROW(36, "Decoding error : %s", ZBUFF_getErrorName(toRead));
		readSize -= inSize;



		/* Write block */
		sizeCheck = fwrite(ress.dstBuffer, 1, decodedSize, foutput);
		if (sizeCheck != decodedSize) EXM_THROW(37, "Write error : unable to write data block to destination file");
		frameSize += decodedSize;
		DISPLAYUPDATE(2, "\rDecoded : %u MB...     ", (U32)(frameSize >> 20));

		if (toRead == 0) break;
		if (readSize) EXM_THROW(38, "Decoding error : should consume entire input");

		/* Fill input buffer */
		if (toRead > ress.srcBufferSize) EXM_THROW(34, "too large block");
		readSize = fread(ress.srcBuffer, 1, toRead, finput);
		if (readSize != toRead) EXM_THROW(35, "Read error");
	}

	return frameSize;
}


static int FIO_decompressFile_extRess(dRess_t ress,
	const char* dstFileName, const char* srcFileName, const char* passwordValue)
{
	unsigned long long filesize = 0;
	FILE* srcFile;
	FILE* dstFile;

	/* Init */
	if (FIO_getFiles(&dstFile, &srcFile, dstFileName, srcFileName)) return 1;


	/* for each frame */
	for (;;)
	{
		size_t sizeCheck;
		/* check magic number -> version */
		size_t toRead = 4;
		sizeCheck = fread(ress.srcBuffer, (size_t)1, toRead, srcFile);
		if (sizeCheck == 0) break;   /* no more input */
		if (sizeCheck != toRead) EXM_THROW(31, "Read error : cannot read header");
#if defined(ZSTD_LEGACY_SUPPORT) && (ZSTD_LEGACY_SUPPORT==1)
		if (ZSTD_isLegacy(MEM_readLE32(ress.srcBuffer)))
		{
			filesize += FIO_decompressLegacyFrame(dstFile, srcFile, MEM_readLE32(ress.srcBuffer));
			continue;
		}
#endif   /* ZSTD_LEGACY_SUPPORT */

		filesize += FIO_decompressFrame(ress, dstFile, srcFile, toRead, passwordValue);
	}

	/* Final Status */
	DISPLAYLEVEL(2, "\r%79s\r", "");
	DISPLAYLEVEL(2, "Successfully decoded %llu bytes \n", filesize);

	/* Close */
	fclose(srcFile);
	if (fclose(dstFile)) EXM_THROW(38, "Write error : cannot properly close %s", dstFileName);

	return 0;
}


int FIO_decompressZstdFilename(const char* dstFileName, const char* srcFileName,
	const char* dictFileName, const char* passwordValue)
{
	int missingFiles = 0;
	dRess_t ress = FIO_createDResources(dictFileName);

	missingFiles += FIO_decompressFile_extRess(ress, dstFileName, srcFileName, passwordValue);

	FIO_freeDResources(ress);
	return missingFiles;
}


#define MAXSUFFIXSIZE 8
int FIO_decompressMultipleFilenames(const char** srcNamesTable, unsigned nbFiles, 
	const char* suffix,
	const char* dictFileName,const char* passwordValue)
{
	unsigned u;
	int skippedFiles = 0;
	int missingFiles = 0;
	char* dstFileName = (char*)malloc(FNSPACE);
	size_t dfnSize = FNSPACE;
	const size_t suffixSize = strlen(suffix);
	dRess_t ress;

	if (dstFileName == NULL) EXM_THROW(70, "not enough memory for dstFileName");
	ress = FIO_createDResources(dictFileName);

	for (u = 0; u<nbFiles; u++)
	{
		const char* srcFileName = srcNamesTable[u];
		size_t sfnSize = strlen(srcFileName);
		const char* suffixPtr = srcFileName + sfnSize - suffixSize;
		if (dfnSize <= sfnSize - suffixSize + 1) { free(dstFileName); dfnSize = sfnSize + 20; dstFileName = (char*)malloc(dfnSize); if (dstFileName == NULL) EXM_THROW(71, "not enough memory for dstFileName"); }
		if (sfnSize <= suffixSize || strcmp(suffixPtr, suffix) != 0)
		{
			DISPLAYLEVEL(1, "File extension doesn't match expected extension (%4s); will not process file: %s\n", suffix, srcFileName);
			skippedFiles++;
			continue;
		}
		memcpy(dstFileName, srcFileName, sfnSize - suffixSize);
		dstFileName[sfnSize - suffixSize] = '\0';

		missingFiles += FIO_decompressFile_extRess(ress, dstFileName, srcFileName, passwordValue);
	}

	FIO_freeDResources(ress);
	free(dstFileName);
	return missingFiles + skippedFiles;
}




static int FIO_getFiles(FILE** fileOutPtr, FILE** fileInPtr,
	const char* dstFileName, const char* srcFileName)
{
	if (!strcmp(srcFileName, stdinmark))
	{
		DISPLAYLEVEL(4, "Using stdin for input\n");
		*fileInPtr = stdin;
		SET_BINARY_MODE(stdin);
	}
	else
	{
		*fileInPtr = fopen(srcFileName, "rb");
	}

	if (*fileInPtr == 0)
	{
		DISPLAYLEVEL(1, "Unable to access file for processing: %s\n", srcFileName);
		return 1;
	}

	if (!strcmp(dstFileName, stdoutmark))
	{
		DISPLAYLEVEL(4, "Using stdout for output\n");
		*fileOutPtr = stdout;
		SET_BINARY_MODE(stdout);
	}
	else
	{
		/* Check if destination file already exists */
		if (!g_overwrite)
		{
			*fileOutPtr = fopen(dstFileName, "rb");
			if (*fileOutPtr != 0)
			{
				/* prompt for overwrite authorization */
				int ch = 'N';
				fclose(*fileOutPtr);
				DISPLAY("Warning : %s already exists \n", dstFileName);
				if ((g_displayLevel <= 1) || (*fileInPtr == stdin))
				{
					/* No interaction possible */
					DISPLAY("Operation aborted : %s already exists \n", dstFileName);
					return 1;
				}
				DISPLAY("Overwrite ? (y/N) : ");
				while ((ch = getchar()) != '\n' && ch != EOF);   /* flush integrated */
				ch = getchar();
				if (getchar() != '\n')
				{
					ch = 'N';
				}
				//while ((ch = getchar()) != '\n' && ch != EOF);   /* flush integrated */
				if ((ch != 'Y') && (ch != 'y'))
				{
					DISPLAY("No. Operation aborted : %s already exists \n", dstFileName);
					return 1;
				}
			}
		}
		*fileOutPtr = fopen(dstFileName, "wb");
	}

	if (*fileOutPtr == 0) EXM_THROW(13, "Pb opening %s", dstFileName);

	return 0;
}

/*!FIO_loadFile
*  creates a buffer, pointed by *bufferPtr,
*  loads "filename" content into it
*  up to MAX_DICT_SIZE bytes
*/
static size_t FIO_loadFile(void** bufferPtr, const char* fileName)
{
	FILE* fileHandle;
	size_t readSize;
	U64 fileSize;

	*bufferPtr = NULL;
	if (fileName == NULL)
		return 0;

	DISPLAYLEVEL(4, "Loading %s as dictionary \n", fileName);
	fileHandle = fopen(fileName, "rb");
	if (fileHandle == 0) EXM_THROW(31, "Error opening file %s", fileName);
	fileSize = FIO_getFileSize2(fileName);
	if (fileSize > MAX_DICT_SIZE)
	{
		int seekResult;
		if (fileSize > 1 GB) EXM_THROW(32, "Dictionary file %s is too large", fileName);   /* avoid extreme cases */
		DISPLAYLEVEL(2, "Dictionary %s is too large : using last %u bytes only \n", fileName, MAX_DICT_SIZE);
		seekResult = fseek(fileHandle, (long int)(fileSize - MAX_DICT_SIZE), SEEK_SET);   /* use end of file */
		if (seekResult != 0) EXM_THROW(33, "Error seeking into file %s", fileName);
		fileSize = MAX_DICT_SIZE;
	}
	*bufferPtr = (BYTE*)malloc((size_t)fileSize);
	if (*bufferPtr == NULL) EXM_THROW(34, "Allocation error : not enough memory for dictBuffer");
	readSize = fread(*bufferPtr, 1, (size_t)fileSize, fileHandle);
	if (readSize != fileSize) EXM_THROW(35, "Error reading dictionary file %s", fileName);
	fclose(fileHandle);
	return (size_t)fileSize;
}
