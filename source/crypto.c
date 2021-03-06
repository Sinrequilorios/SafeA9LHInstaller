/*
*   crypto.c
*
*   Crypto libs from http://github.com/b1l1s/ctr
*/

#include "crypto.h"
#include "memory.h"
#include "fatfs/sdmmc/sdmmc.h"

/****************************************************************
*                  Crypto libs
****************************************************************/

/* original version by megazig */

#ifndef __thumb__
#define BSWAP32(x) {\
    __asm__\
    (\
        "eor r1, %1, %1, ror #16\n\t"\
        "bic r1, r1, #0xFF0000\n\t"\
        "mov %0, %1, ror #8\n\t"\
        "eor %0, %0, r1, lsr #8\n\t"\
        :"=r"(x)\
        :"0"(x)\
        :"r1"\
    );\
};

#define ADD_u128_u32(u128_0, u128_1, u128_2, u128_3, u32_0) {\
__asm__\
    (\
        "adds %0, %4\n\t"\
        "addcss %1, %1, #1\n\t"\
        "addcss %2, %2, #1\n\t"\
        "addcs %3, %3, #1\n\t"\
        : "+r"(u128_0), "+r"(u128_1), "+r"(u128_2), "+r"(u128_3)\
        : "r"(u32_0)\
        : "cc"\
    );\
}
#else
#define BSWAP32(x) {x = __builtin_bswap32(x);}

#define ADD_u128_u32(u128_0, u128_1, u128_2, u128_3, u32_0) {\
__asm__\
    (\
        "mov r4, #0\n\t"\
        "add %0, %0, %4\n\t"\
        "adc %1, %1, r4\n\t"\
        "adc %2, %2, r4\n\t"\
        "adc %3, %3, r4\n\t"\
        : "+r"(u128_0), "+r"(u128_1), "+r"(u128_2), "+r"(u128_3)\
        : "r"(u32_0)\
        : "cc", "r4"\
    );\
}
#endif /*__thumb__*/

static void aes_setkey(u8 keyslot, const void *key, u32 keyType, u32 mode)
{
    if(keyslot <= 0x03) return; // Ignore TWL keys for now
    u32 *key32 = (u32 *)key;
    *REG_AESCNT = (*REG_AESCNT & ~(AES_CNT_INPUT_ENDIAN | AES_CNT_INPUT_ORDER)) | mode;
    *REG_AESKEYCNT = (*REG_AESKEYCNT >> 6 << 6) | keyslot | AES_KEYCNT_WRITE;

    REG_AESKEYFIFO[keyType] = key32[0];
    REG_AESKEYFIFO[keyType] = key32[1];
    REG_AESKEYFIFO[keyType] = key32[2];
    REG_AESKEYFIFO[keyType] = key32[3];
}

static void aes_use_keyslot(u8 keyslot)
{
    if(keyslot > 0x3F)
        return;

    *REG_AESKEYSEL = keyslot;
    *REG_AESCNT = *REG_AESCNT | 0x04000000; /* mystery bit */
}

static void aes_setiv(const void *iv, u32 mode)
{
    const u32 *iv32 = (const u32 *)iv;
    *REG_AESCNT = (*REG_AESCNT & ~(AES_CNT_INPUT_ENDIAN | AES_CNT_INPUT_ORDER)) | mode;

    // Word order for IV can't be changed in REG_AESCNT and always default to reversed
    if(mode & AES_INPUT_NORMAL)
    {
        REG_AESCTR[0] = iv32[3];
        REG_AESCTR[1] = iv32[2];
        REG_AESCTR[2] = iv32[1];
        REG_AESCTR[3] = iv32[0];
    }
    else
    {
        REG_AESCTR[0] = iv32[0];
        REG_AESCTR[1] = iv32[1];
        REG_AESCTR[2] = iv32[2];
        REG_AESCTR[3] = iv32[3];
    }
}

static void aes_advctr(void *ctr, u32 val, u32 mode)
{
    u32 *ctr32 = (u32 *)ctr;

    int i;
    if(mode & AES_INPUT_BE)
    {
        for(i = 0; i < 4; ++i) // Endian swap
            BSWAP32(ctr32[i]);
    }

    if(mode & AES_INPUT_NORMAL)
    {
        ADD_u128_u32(ctr32[3], ctr32[2], ctr32[1], ctr32[0], val);
    }
    else
    {
        ADD_u128_u32(ctr32[0], ctr32[1], ctr32[2], ctr32[3], val);
    }

    if(mode & AES_INPUT_BE)
    {
        for(i = 0; i < 4; ++i) // Endian swap
            BSWAP32(ctr32[i]);
    }
}

static void aes_change_ctrmode(void *ctr, u32 fromMode, u32 toMode)
{
    u32 *ctr32 = (u32 *)ctr;
    int i;
    if((fromMode ^ toMode) & AES_CNT_INPUT_ENDIAN)
    {
        for(i = 0; i < 4; ++i)
            BSWAP32(ctr32[i]);
    }

    if((fromMode ^ toMode) & AES_CNT_INPUT_ORDER)
    {
        u32 temp = ctr32[0];
        ctr32[0] = ctr32[3];
        ctr32[3] = temp;

        temp = ctr32[1];
        ctr32[1] = ctr32[2];
        ctr32[2] = temp;
    }
}

static void aes_batch(void *dst, const void *src, u32 blockCount)
{
    *REG_AESBLKCNT = blockCount << 16;
    *REG_AESCNT |=  AES_CNT_START;

    const u32 *src32    = (const u32 *)src;
    u32 *dst32          = (u32 *)dst;

    u32 wbc = blockCount;
    u32 rbc = blockCount;

    while(rbc)
    {
        if(wbc && ((*REG_AESCNT & 0x1F) <= 0xC)) // There's space for at least 4 ints
        {
            *REG_AESWRFIFO = *src32++;
            *REG_AESWRFIFO = *src32++;
            *REG_AESWRFIFO = *src32++;
            *REG_AESWRFIFO = *src32++;
            wbc--;
        }

        if(rbc && ((*REG_AESCNT & (0x1F << 0x5)) >= (0x4 << 0x5))) // At least 4 ints available for read
        {
            *dst32++ = *REG_AESRDFIFO;
            *dst32++ = *REG_AESRDFIFO;
            *dst32++ = *REG_AESRDFIFO;
            *dst32++ = *REG_AESRDFIFO;
            rbc--;
        }
    }
}

static void aes(void *dst, const void *src, u32 blockCount, void *iv, u32 mode, u32 ivMode)
{
    *REG_AESCNT =   mode |
                    AES_CNT_INPUT_ORDER | AES_CNT_OUTPUT_ORDER |
                    AES_CNT_INPUT_ENDIAN | AES_CNT_OUTPUT_ENDIAN |
                    AES_CNT_FLUSH_READ | AES_CNT_FLUSH_WRITE;

    u32 blocks;
    while(blockCount != 0)
    {
        if((mode & AES_ALL_MODES) != AES_ECB_ENCRYPT_MODE
        && (mode & AES_ALL_MODES) != AES_ECB_DECRYPT_MODE)
            aes_setiv(iv, ivMode);

        blocks = (blockCount >= 0xFFFF) ? 0xFFFF : blockCount;

        // Save the last block for the next decryption CBC batch's iv
        if((mode & AES_ALL_MODES) == AES_CBC_DECRYPT_MODE)
        {
            memcpy(iv, src + (blocks - 1) * AES_BLOCK_SIZE, AES_BLOCK_SIZE);
            aes_change_ctrmode(iv, AES_INPUT_BE | AES_INPUT_NORMAL, ivMode);
        }

        // Process the current batch
        aes_batch(dst, src, blocks);

        // Save the last block for the next encryption CBC batch's iv
        if((mode & AES_ALL_MODES) == AES_CBC_ENCRYPT_MODE)
        {
            memcpy(iv, dst + (blocks - 1) * AES_BLOCK_SIZE, AES_BLOCK_SIZE);
            aes_change_ctrmode(iv, AES_INPUT_BE | AES_INPUT_NORMAL, ivMode);
        }

        // Advance counter for CTR mode
        else if((mode & AES_ALL_MODES) == AES_CTR_MODE)
            aes_advctr(iv, blocks, ivMode);

        src += blocks * AES_BLOCK_SIZE;
        dst += blocks * AES_BLOCK_SIZE;
        blockCount -= blocks;
    }
}

static void sha_wait_idle()
{
    while(*REG_SHA_CNT & 1);
}

static void sha(void *res, const void *src, u32 size, u32 mode)
{
    sha_wait_idle();
    *REG_SHA_CNT = mode | SHA_CNT_OUTPUT_ENDIAN | SHA_NORMAL_ROUND;

    const u32 *src32 = (const u32 *)src;
    int i;
    while(size >= 0x40)
    {
        sha_wait_idle();
        for(i = 0; i < 4; ++i)
        {
            *REG_SHA_INFIFO = *src32++;
            *REG_SHA_INFIFO = *src32++;
            *REG_SHA_INFIFO = *src32++;
            *REG_SHA_INFIFO = *src32++;
        }

        size -= 0x40;
    }

    sha_wait_idle();
    memcpy((void *)REG_SHA_INFIFO, src32, size);

    *REG_SHA_CNT = (*REG_SHA_CNT & ~SHA_NORMAL_ROUND) | SHA_FINAL_ROUND;

    while(*REG_SHA_CNT & SHA_FINAL_ROUND);
    sha_wait_idle();

    u32 hashSize = SHA_256_HASH_SIZE;
    if(mode == SHA_224_MODE)
        hashSize = SHA_224_HASH_SIZE;
    else if(mode == SHA_1_MODE)
        hashSize = SHA_1_HASH_SIZE;

    memcpy(res, (void *)REG_SHA_HASH, hashSize);
}

/*****************************************************************/

static u8 __attribute__((aligned(4))) nandCtr[AES_BLOCK_SIZE];
static u8 nandSlot;
static u32 fatStart;
const u8 __attribute__((aligned(4))) key2s[3][AES_BLOCK_SIZE] = {
    {0x42, 0x3F, 0x81, 0x7A, 0x23, 0x52, 0x58, 0x31, 0x6E, 0x75, 0x8E, 0x3A, 0x39, 0x43, 0x2E, 0xD0},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3B, 0xF5, 0xF6},
    {0x65, 0x29, 0x3E, 0x12, 0x56, 0x0C, 0x0B, 0xD1, 0xDD, 0xB5, 0x63, 0x1C, 0xB6, 0xD9, 0x52, 0x75}
};

void getNandCtr(void)
{
    u8 __attribute__((aligned(4))) cid[AES_BLOCK_SIZE];
    u8 __attribute__((aligned(4))) shaSum[SHA_256_HASH_SIZE];

    sdmmc_get_cid(1, (u32 *)cid);
    sha(shaSum, cid, sizeof(cid), SHA_256_MODE);
    memcpy(nandCtr, shaSum, sizeof(nandCtr));
}

void ctrNandInit(void)
{
    getNandCtr();

    if(isN3DS)
    {
        u8 __attribute__((aligned(4))) keyY0x5[AES_BLOCK_SIZE] = {0x4D, 0x80, 0x4F, 0x4E, 0x99, 0x90, 0x19, 0x46, 0x13, 0xA2, 0x04, 0xAC, 0x58, 0x44, 0x60, 0xBE};
        aes_setkey(0x05, keyY0x5, AES_KEYY, AES_INPUT_BE | AES_INPUT_NORMAL);

        nandSlot = 0x05;
        fatStart = 0x5CAD7;
    }
    else
    {
        nandSlot = 0x04;
        fatStart = 0x5CAE5;
    }
}

u32 ctrNandRead(u32 sector, u32 sectorCount, u8 *outbuf)
{
    u8 __attribute__((aligned(4))) tmpCtr[sizeof(nandCtr)];
    memcpy(tmpCtr, nandCtr, sizeof(nandCtr));
    aes_advctr(tmpCtr, ((sector + fatStart) * 0x200) / AES_BLOCK_SIZE, AES_INPUT_BE | AES_INPUT_NORMAL);

    //Read
    u32 result = sdmmc_nand_readsectors(sector + fatStart, sectorCount, outbuf);

    //Decrypt
    aes_use_keyslot(nandSlot);
    aes(outbuf, outbuf, sectorCount * 0x200 / AES_BLOCK_SIZE, tmpCtr, AES_CTR_MODE, AES_INPUT_BE | AES_INPUT_NORMAL);

    return result;
}

void readFirm0(u8 *outbuf, u32 size)
{
    u8 __attribute__((aligned(4))) ctrTmp[sizeof(nandCtr)];
    memcpy(ctrTmp, nandCtr, sizeof(nandCtr));

    //Read FIRM0 data
    sdmmc_nand_readsectors(0x0B130000 / 0x200, size / 0x200, outbuf);

    //Decrypt
    aes_advctr(ctrTmp, 0x0B130000 / AES_BLOCK_SIZE, AES_INPUT_BE | AES_INPUT_NORMAL);
    aes_use_keyslot(0x06);
    aes(outbuf, outbuf, size / AES_BLOCK_SIZE, ctrTmp, AES_CTR_MODE, AES_INPUT_BE | AES_INPUT_NORMAL);
}

void writeFirm(u8 *inbuf, bool isFirm1, u32 size)
{
    u32 offset = isFirm1 ? 0x0B530000 : 0x0B130000;
    u8 __attribute__((aligned(4))) ctrTmp[sizeof(nandCtr)];
    memcpy(ctrTmp, nandCtr, sizeof(nandCtr));

    //Encrypt FIRM data
    aes_advctr(ctrTmp, offset / AES_BLOCK_SIZE, AES_INPUT_BE | AES_INPUT_NORMAL);
    aes_use_keyslot(0x06);
    aes(inbuf, inbuf, size / AES_BLOCK_SIZE, ctrTmp, AES_CTR_MODE, AES_INPUT_BE | AES_INPUT_NORMAL);

    //Write to NAND
    sdmmc_nand_writesectors(offset / 0x200, size / 0x200, inbuf);
}

void setupKeyslot0x11(const void *otp, bool isA9lh)
{
    u8 __attribute__((aligned(4))) shasum[SHA_256_HASH_SIZE];
    u8 __attribute__((aligned(4))) keyX[AES_BLOCK_SIZE];
    u8 __attribute__((aligned(4))) keyY[AES_BLOCK_SIZE];

    //If booting via A9LH, use the leftover contents of the SHA register
    if(isA9lh) memcpy(shasum, (void *)REG_SHA_HASH, sizeof(shasum));

    //Else calculate the otp.bin hash
    else sha(shasum, otp, 0x90, SHA_256_MODE);

    //Set keyX and keyY
    memcpy(keyX, shasum, sizeof(keyX));
    memcpy(keyY, shasum + sizeof(keyX), sizeof(keyY));
    aes_setkey(0x11, keyX, AES_KEYX, AES_INPUT_BE | AES_INPUT_NORMAL);
    aes_setkey(0x11, keyY, AES_KEYY, AES_INPUT_BE | AES_INPUT_NORMAL);
}

void generateSector(u8 *keySector, u32 mode)
{
    //Inject key2
    if(mode == 0) memcpy(keySector + AES_BLOCK_SIZE, key2s[2], AES_BLOCK_SIZE);
    else if(mode == 1) memcpy(keySector + AES_BLOCK_SIZE, keySector, AES_BLOCK_SIZE);
    else memcpy(keySector + AES_BLOCK_SIZE, key2s[0], AES_BLOCK_SIZE);

    if(mode != 1)
    {
        //Encrypt key sector
        aes_use_keyslot(0x11);
        for(u32 i = 0; i < 32; i++)
            aes(keySector + (AES_BLOCK_SIZE * i), keySector + (AES_BLOCK_SIZE * i), 1, NULL, AES_ECB_ENCRYPT_MODE, 0);
    }
}

void getSector(u8 *keySector, bool isA9lh)
{
    //Read keysector from NAND
    sdmmc_nand_readsectors(0x96, 1, keySector);

    if(isA9lh)
    {
        //Decrypt key sector
        aes_use_keyslot(0x11);
        for(u32 i = 0; i < 32; i++)
            aes(keySector + (AES_BLOCK_SIZE * i), keySector + (AES_BLOCK_SIZE * i), 1, NULL, AES_ECB_DECRYPT_MODE, 0);
    }
}

u32 verifyHash(const void *data, u32 size, const u8 *hash)
{
    u8 __attribute__((aligned(4))) shasum[SHA_256_HASH_SIZE];
    sha(shasum, data, size, SHA_256_MODE);

    return memcmp(shasum, hash, sizeof(shasum)) == 0;
}

u32 decryptExeFs(u8 *inbuf)
{
    u8 *exeFsOffset = inbuf + *(u32 *)(inbuf + 0x1A0) * 0x200;
    u32 exeFsSize = *(u32 *)(inbuf + 0x1A4) * 0x200;
    u8 __attribute__((aligned(4))) ncchCtr[AES_BLOCK_SIZE] = {0};

    for(u32 i = 0; i < 8; i++)
        ncchCtr[7 - i] = *(inbuf + 0x108 + i);
    ncchCtr[8] = 2;

    aes_setkey(0x2C, inbuf, AES_KEYY, AES_INPUT_BE | AES_INPUT_NORMAL);
    aes_advctr(ncchCtr, 0x200 / AES_BLOCK_SIZE, AES_INPUT_BE | AES_INPUT_NORMAL);
    aes_use_keyslot(0x2C);
    aes(inbuf, exeFsOffset + 0x200, exeFsSize / AES_BLOCK_SIZE, ncchCtr, AES_CTR_MODE, AES_INPUT_BE | AES_INPUT_NORMAL);

    return exeFsSize - 0x200;
}