#ifndef _AES_H
#define _AES_H


#define BPOLY 0x1b //!< Lower 8 bits of (x^8+x^4+x^3+x+1), ie. (x^4+x^3+x+1).
#define BLOCKSIZE 16 //!< Block size in number of bytes.

#define KEY_COUNT 3

#if KEY_COUNT == 1
  #define KEYBITS 128 //!< Use AES128.
#elif KEY_COUNT == 2
  #define KEYBITS 192 //!< Use AES196.
#elif KEY_COUNT == 3
  #define KEYBITS 256 //!< Use AES256.
#else
  #error Use 1, 2 or 3 keys!
#endif

#if KEYBITS == 128
  #define ROUNDS 10 //!< Number of rounds.
  #define KEYLENGTH 16 //!< Key length in number of bytes.
#elif KEYBITS == 192
  #define ROUNDS 12 //!< Number of rounds.
  #define KEYLENGTH 24 //!< // Key length in number of bytes.
#elif KEYBITS == 256
  #define ROUNDS 14 //!< Number of rounds.
  #define KEYLENGTH 32 //!< Key length in number of bytes.
#else
  #error Key must be 128, 192 or 256 bits!
#endif

extern unsigned char *expandedKey;
	
extern void aesEncInit(void);
extern void aesEncryptBlock(unsigned char *buffer);
extern void aesEncrypt( unsigned char * buffer, unsigned char * chainBlock );
extern void aesDecInit(void);
extern void aesDecrypt( unsigned char * buffer, unsigned char * chainBlock );
extern unsigned char ota_aes_uses_external_key(void);



#endif

