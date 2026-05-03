/**
 * @file    aes.c
 * @brief   AES-128 CBC 加密/解密实现
 * @note    本模块实现 AES-128 算法的完整加密与解密流程，
 *          包含 GF(2^8) 有限域运算、S-Box 生成、密钥扩展、
 *          CBC 模式加密/解密等核心功能，用于 OTA 固件的安全传输。
 *
 * @par 安全密钥管理
 *      生产环境优先使用外部密钥头文件 ota_security_secrets.h，
 *      仅在未检测到外部密钥时回退到内置开发密钥。
 *      Keil/ARMCC 编译器默认优先包含外部密钥，
 *      可通过定义 OTA_FORCE_DEV_AES_KEY 强制使用开发密钥。
 *
 * @par 工作缓冲区
 *      使用三个 256 字节全局缓冲区（block1、block2、tempbuf）作为
 *      查找表和密钥扩展的临时工作空间，避免动态内存分配。
 *
 * @version 2.0
 * @date    2026-05-01
 */

/* =========================================================================
 *  1. 头文件包含
 * ======================================================================= */

#include "aes.h"

/*
 * Keil/ARMCC 在 STM32 上可能不提供 __has_include，此前会导致本文件
 * 即使存在本地 ota_security_secrets.h 也会静默回退到内置开发密钥，
 * 从而在 OTA 传输完成后引发 AUTH HEXP 错误。
 *
 * 因此 ARMCC 生产构建默认优先包含本地密钥头文件。
 * 若需在 STM32 上强制使用开发密钥，请在工程选项中定义 OTA_FORCE_DEV_AES_KEY。
 */
#if !defined(OTA_FORCE_DEV_AES_KEY)
#if defined(__CC_ARM) || defined(__ARMCC_VERSION)
#include "ota_security_secrets.h"
#define OTA_HAS_EXTERNAL_AES_KEY   (1)
#elif defined(__has_include)
#if __has_include("ota_security_secrets.h")
#include "ota_security_secrets.h"
#define OTA_HAS_EXTERNAL_AES_KEY   (1)
#endif
#endif
#endif

/* =========================================================================
 *  2. 密钥定义
 * ======================================================================= */

#if defined(OTA_HAS_EXTERNAL_AES_KEY)
/** 使用外部安全密钥（生产环境） */
const unsigned char kTable[32] = OTA_SECURITY_AES_KEY_BYTES;
#else
/** 内置开发回退密钥（仅用于开发/调试） */
const unsigned char kTable[32] =
{
    0x9D, 0xD2, 0x00, 0x24, 0x84, 0x60, 0x2E, 0xDA,
    0x0C, 0xDD, 0x52, 0x7B, 0x05, 0xC1, 0x6B, 0x01,
    0xFF, 0x17, 0xCD, 0x6F, 0x8C, 0x1E, 0x3E, 0x09,
    0xCF, 0x1F, 0x0C, 0x78, 0x87, 0xEF, 0x8A, 0xEC
};
#endif

/**
 * @brief  查询当前是否使用外部安全密钥
 * @retval 1 — 使用外部密钥；0 — 使用内置开发密钥
 */
unsigned char ota_aes_uses_external_key(void)
{
#if defined(OTA_HAS_EXTERNAL_AES_KEY)
    return 1U;
#else
    return 0U;
#endif
}

/* =========================================================================
 *  3. 全局工作缓冲区与查找表指针
 * ======================================================================= */

unsigned char block1[256];      /**< 工作缓冲区 1（查表/密钥扩展）  */
unsigned char block2[256];      /**< 工作缓冲区 2（查表/密钥扩展）  */
unsigned char tempbuf[256];     /**< 临时缓冲区                     */

unsigned char *powTbl;          /**< 指数查找表指针（GF(2^8)）       */
unsigned char *logTbl;          /**< 对数查找表指针（GF(2^8)）       */
unsigned char *sBox;            /**< S-Box 查找表指针                */
unsigned char *sBoxInv;         /**< 逆 S-Box 查找表指针             */
unsigned char *expandedKey;     /**< 扩展密钥指针                    */

/* =========================================================================
 *  4. 内部函数实现 —— GF(2^8) 有限域运算
 * ======================================================================= */

/**
 * @brief  计算 GF(2^8) 的指数表和对数表
 * @note   以 0x03 为生成元，遍历 GF(2^8) 的所有非零元素。
 *         利用循环群性质：t 乘以 3 在 GF(2^8) 中运算，
 *         当 t 回到 1 时说明遍历了全部 255 个非零元素。
 * @param  powTbl — 输出：指数表（256 字节）
 * @param  logTbl — 输出：对数表（256 字节）
 */
void CalcPowLog(unsigned char *powTbl, unsigned char *logTbl)
{
    unsigned char i = 0;
    unsigned char t = 1;

    do {
        /* 记录指数和对数值 */
        powTbl[i] = t;
        logTbl[t] = i;
        i++;

        /* 在 GF(2^8) 中将 t 乘以 3（生成元） */
        t ^= (t << 1) ^ (t & 0x80 ? BPOLY : 0);
    } while (t != 1);   /* 循环群性质保证 i < 255 */

    powTbl[255] = powTbl[0];    /* 255 = '-0'，254 = -1，依此类推 */
}

/**
 * @brief  计算 AES S-Box 查找表
 * @note   对 GF(2^8) 中每个元素：
 *         1. 计算乘法逆元（0 映射为 0）
 *         2. 进行 GF(2) 上的仿射变换（循环左移 + 异或 0x63）
 * @param  sBox — 输出：S-Box 查找表（256 字节）
 */
void CalcSBox(unsigned char *sBox)
{
    unsigned char i, rot;
    unsigned char temp;
    unsigned char result;

    i = 0;
    do {
        /* 步骤 1：GF(2^8) 中的乘法逆元 */
        if (i > 0)
        {
            temp = powTbl[255 - logTbl[i]];
        }
        else
        {
            temp = 0;
        }

        /* 步骤 2：GF(2) 上的仿射变换 */
        result = temp ^ 0x63;              /* 初始异或向量 */
        for (rot = 4; rot > 0; rot--)
        {
            temp = (temp << 1) | (temp >> 7);   /* 循环左移 1 位 */
            result ^= temp;                      /* 累加异或 */
        }

        sBox[i] = result;
    } while (++i != 0);
}

/**
 * @brief  计算逆 S-Box 查找表
 * @note   通过遍历 S-Box 构建反向映射：sBoxInv[sBox[i]] = i
 * @param  sBox    — 输入：正向 S-Box
 * @param  sBoxInv — 输出：逆 S-Box（256 字节）
 */
void CalcSBoxInv(unsigned char *sBox, unsigned char *sBoxInv)
{
    unsigned char i = 0;
    unsigned char j = 0;

    do {
        do {
            if (sBox[j] == i)
            {
                sBoxInv[i] = j;
                j = 255;    /* 找到匹配，终止内层循环 */
            }
        } while (++j != 0);
    } while (++i != 0);
}

/* =========================================================================
 *  5. 内部函数实现 —— 行/列变换辅助函数
 * ======================================================================= */

/**
 * @brief  4 字节数组循环左移 1 字节
 * @param  row — 4 字节数组
 */
void CycleLeft(unsigned char *row)
{
    unsigned char temp = row[0];

    row[0] = row[1];
    row[1] = row[2];
    row[2] = row[3];
    row[3] = temp;
}

/**
 * @brief  对 4 个字节分别在 GF(2^8) 中乘以 2
 * @note   即 xtime 操作：左移 1 位，若最高位为 1 则异或 BPOLY
 * @param  col — 4 字节数组（原地修改）
 */
void CalcCols(unsigned char *col)
{
    unsigned char i;

    for (i = 4; i > 0; i--)
    {
        *col = (*col << 1) ^ (*col & 0x80 ? BPOLY : 0);
        col++;
    }
}

/**
 * @brief  AES 逆 MixColumn 变换（单列）
 * @note   基于 3 次 CalcCols（乘以 2）和异或组合实现，
 *         避免直接矩阵乘法，提高运算效率。
 * @param  column — 4 字节列向量（原地修改）
 */
void InvMixColumn(unsigned char *column)
{
    unsigned char r[4];

    /* 计算列元素的异或组合 */
    r[0] = column[1] ^ column[2] ^ column[3];
    r[1] = column[0] ^ column[2] ^ column[3];
    r[2] = column[0] ^ column[1] ^ column[3];
    r[3] = column[0] ^ column[1] ^ column[2];

    /* 第 1 轮乘以 2 */
    CalcCols(column);

    r[0] ^= column[0] ^ column[1];
    r[1] ^= column[1] ^ column[2];
    r[2] ^= column[2] ^ column[3];
    r[3] ^= column[0] ^ column[3];

    /* 第 2 轮乘以 2 */
    CalcCols(column);

    r[0] ^= column[0] ^ column[2];
    r[1] ^= column[1] ^ column[3];
    r[2] ^= column[0] ^ column[2];
    r[3] ^= column[1] ^ column[3];

    /* 第 3 轮乘以 2 */
    CalcCols(column);

    column[0] ^= column[1] ^ column[2] ^ column[3];
    r[0] ^= column[0];
    r[1] ^= column[0];
    r[2] ^= column[0];
    r[3] ^= column[0];

    /* 将结果写回列向量 */
    column[0] = r[0];
    column[1] = r[1];
    column[2] = r[2];
    column[3] = r[3];
}

/* =========================================================================
 *  6. 内部函数实现 —— 字节替换与行移位
 * ======================================================================= */

/**
 * @brief  字节替换（正向 S-Box）
 * @param  bytes — 字节数组
 * @param  count — 字节数量
 */
void SubBytes(unsigned char *bytes, unsigned char count)
{
    do {
        *bytes = sBox[*bytes];
        bytes++;
    } while (--count);
}

/**
 * @brief  逆字节替换 + 异或轮密钥（合并操作提升性能）
 * @note   直接使用 block2 缓冲区作为逆 S-Box，减少一次查表。
 * @param  bytes — 字节数组（原地修改）
 * @param  key   — 轮密钥
 * @param  count — 字节数量
 */
void InvSubBytesAndXOR(unsigned char *bytes, unsigned char *key, unsigned char count)
{
    do {
        *bytes = block2[*bytes] ^ *key;     /* 逆 S-Box + 异或密钥 */
        bytes++;
        key++;
    } while (--count);
}

/**
 * @brief  逆行移位变换
 * @note   状态按列排列，逆 ShiftRows 将各行循环右移：
 *         - 第 2 行：右移 1 位
 *         - 第 3 行：右移 2 位
 *         - 第 4 行：右移 3 位（即左移 1 位）
 * @param  state — 16 字节状态矩阵（4×4，列优先排列）
 */
void InvShiftRows(unsigned char *state)
{
    unsigned char temp;

    /* 第 2 行：循环右移 1 位 */
    temp = state[1 + 3 * 4];
    state[1 + 3 * 4] = state[1 + 2 * 4];
    state[1 + 2 * 4] = state[1 + 1 * 4];
    state[1 + 1 * 4] = state[1 + 0 * 4];
    state[1 + 0 * 4] = temp;

    /* 第 3 行：循环右移 2 位 */
    temp = state[2 + 0 * 4];
    state[2 + 0 * 4] = state[2 + 2 * 4];
    state[2 + 2 * 4] = temp;
    temp = state[2 + 1 * 4];
    state[2 + 1 * 4] = state[2 + 3 * 4];
    state[2 + 3 * 4] = temp;

    /* 第 4 行：循环右移 3 位（即左移 1 位） */
    temp = state[3 + 0 * 4];
    state[3 + 0 * 4] = state[3 + 1 * 4];
    state[3 + 1 * 4] = state[3 + 2 * 4];
    state[3 + 2 * 4] = state[3 + 3 * 4];
    state[3 + 3 * 4] = temp;
}

/* =========================================================================
 *  7. 内部函数实现 —— 字节操作工具
 * ======================================================================= */

/**
 * @brief  两个字节数组逐字节异或（GF(2) 加法）
 * @param  bytes1 — 目标数组（原地修改）
 * @param  bytes2 — 源数组
 * @param  count  — 字节数量
 */
void XORBytes(unsigned char *bytes1, unsigned char *bytes2, unsigned char count)
{
    do {
        *bytes1 ^= *bytes2;
        bytes1++;
        bytes2++;
    } while (--count);
}

/**
 * @brief  字节数组复制
 * @param  to    — 目标地址
 * @param  from  — 源地址
 * @param  count — 字节数量
 */
void CopyBytes(unsigned char *to, unsigned char *from, unsigned char count)
{
    do {
        *to = *from;
        to++;
        from++;
    } while (--count);
}

/* =========================================================================
 *  8. 内部函数实现 —— 密钥扩展
 * ======================================================================= */

/**
 * @brief  AES-128 密钥扩展
 * @note   将 16 字节初始密钥扩展为 176 字节（11 轮 × 16 字节）。
 *         每 16 字节（KEYLENGTH）执行一次 RotWord + SubWord + Rcon 操作。
 * @param  expandedKey — 输出：扩展密钥缓冲区
 */
void KeyExpansion(unsigned char *expandedKey)
{
    unsigned char temp[4];
    unsigned char i;
    unsigned char Rcon[4] = { 0x01, 0x00, 0x00, 0x00 };    /* 轮常数 */

    const unsigned char *key = kTable;

    /* 将初始密钥复制到扩展密钥头部 */
    i = KEYLENGTH;
    do {
        *expandedKey = *key;
        expandedKey++;
        key++;
    } while (--i);

    /* 取扩展密钥最后 4 字节到 temp */
    CopyBytes(temp, expandedKey - 4, 4);

    /* 逐 4 字节扩展密钥 */
    i = KEYLENGTH;
    while (i < BLOCKSIZE * (ROUNDS + 1))
    {
        /* 每 KEYLENGTH 字节执行一次完整变换 */
        if ((i % KEYLENGTH) == 0)
        {
            CycleLeft(temp);            /* RotWord：循环左移 1 字节 */
            SubBytes(temp, 4);          /* SubWord：S-Box 替换       */
            XORBytes(temp, Rcon, 4);    /* 异或轮常数                 */
            *Rcon = (*Rcon << 1) ^ (*Rcon & 0x80 ? BPOLY : 0);  /* 更新 Rcon */
        }

        /* 密钥长度 > 24 字节（>192 位）时，额外 SubWord */
#if KEYLENGTH > 24
        else if ((i % KEYLENGTH) == BLOCKSIZE)
        {
            SubBytes(temp, 4);
        }
#endif

        /* 异或 KEYLENGTH 字节前的密钥字 */
        XORBytes(temp, expandedKey - KEYLENGTH, 4);

        /* 写入扩展密钥 */
        *(expandedKey++) = temp[0];
        *(expandedKey++) = temp[1];
        *(expandedKey++) = temp[2];
        *(expandedKey++) = temp[3];

        i += 4;
    }
}

/* =========================================================================
 *  9. 核心算法实现 —— AES 解密
 * ======================================================================= */

/**
 * @brief  AES-128 逆密码（解密单个 16 字节块）
 * @note   解密流程（逆序）：
 *         1. AddRoundKey（最后一轮密钥）
 *         2. ROUNDS-1 轮：InvShiftRows → InvSubBytes+AddRoundKey → InvMixColumns
 *         3. 最后一轮：InvShiftRows → InvSubBytes+AddRoundKey（无 InvMixColumns）
 * @param  block       — 16 字节数据块（原地解密）
 * @param  expandedKey — 扩展密钥
 */
void InvCipher(unsigned char *block, unsigned char *expandedKey)
{
    unsigned char i, j;
    unsigned char round = ROUNDS - 1;

    /* 定位到最后一轮密钥 */
    expandedKey += BLOCKSIZE * ROUNDS;

    /* 初始轮密钥加 */
    XORBytes(block, expandedKey, 16);
    expandedKey -= BLOCKSIZE;

    /* 主循环：ROUNDS-1 轮 */
    do {
        InvShiftRows(block);
        InvSubBytesAndXOR(block, expandedKey, 16);
        expandedKey -= BLOCKSIZE;

        /* InvMixColumns：对 4 列分别处理 */
        for (i = 4, j = 0; i > 0; i--, j += 4)
            InvMixColumn(block + j);
    } while (--round);

    /* 最后一轮（无 InvMixColumns） */
    InvShiftRows(block);
    InvSubBytesAndXOR(block, expandedKey, 16);
}

/* =========================================================================
 *  10. 公共接口实现 —— AES CBC 解密
 * ======================================================================= */

/**
 * @brief  初始化 AES 解密环境
 * @note   依次计算：指数/对数表 → S-Box → 扩密钥 → 逆 S-Box。
 *         复用 block1/block2/tempbuf 缓冲区，需注意调用顺序。
 */
void aesDecInit(void)
{
    /* 阶段 1：计算 GF(2^8) 指数/对数表 */
    powTbl = block1;
    logTbl = block2;
    CalcPowLog(powTbl, logTbl);

    /* 阶段 2：计算 S-Box */
    sBox = tempbuf;
    CalcSBox(sBox);

    /* 阶段 3：密钥扩展（复用 block1） */
    expandedKey = block1;
    KeyExpansion(expandedKey);

    /* 阶段 4：计算逆 S-Box（必须使用 block2） */
    sBoxInv = block2;
    CalcSBoxInv(sBox, sBoxInv);
}

/**
 * @brief  AES-128 CBC 模式解密（单个 16 字节块）
 * @note   CBC 解密流程：
 *         1. 保存密文备份
 *         2. 对密文执行 AES 逆密码
 *         3. 与前一个密文块（chainBlock）异或得到明文
 *         4. 更新 chainBlock 为当前密文
 * @param  buffer     — 输入：密文；输出：明文（16 字节）
 * @param  chainBlock — CBC 链式块（上一个密文块）
 */
void aesDecrypt(unsigned char *buffer, unsigned char *chainBlock)
{
    aesDecInit();
    CopyBytes(tempbuf, buffer, BLOCKSIZE);      /* 保存密文备份 */
    InvCipher(buffer, expandedKey);              /* AES 逆密码   */
    XORBytes(buffer, chainBlock, BLOCKSIZE);     /* CBC 异或     */
    CopyBytes(chainBlock, tempbuf, BLOCKSIZE);   /* 更新链式块   */
}

/* =========================================================================
 *  11. 内部函数实现 —— GF(2^8) 乘法与 MixColumn
 * ======================================================================= */

/**
 * @brief  GF(2^8) 乘法
 * @note   通过逐位扫描 factor 实现：
 *         若当前位为 1，则将 num 异或到结果中，
 *         然后将 num 在 GF(2^8) 中乘以 2（左移 + 条件异或 BPOLY）。
 * @param  num    — 被乘数
 * @param  factor — 乘数
 * @return GF(2^8) 乘法结果
 */
unsigned char Multiply(unsigned char num, unsigned char factor)
{
    unsigned char mask = 1;
    unsigned char result = 0;

    while (mask != 0)
    {
        if (mask & factor)
        {
            result ^= num;     /* 累加当前倍数 */
        }

        mask <<= 1;            /* 移到下一位   */
        num = (num << 1) ^ (num & 0x80 ? BPOLY : 0);  /* GF(2^8) 乘以 2 */
    }

    return result;
}

/**
 * @brief  4 维向量点积（GF(2^8)）
 * @note   result = v1[0]*v2[0] ^ v1[1]*v2[1] ^ v1[2]*v2[2] ^ v1[3]*v2[3]
 * @param  vector1 — 向量 1
 * @param  vector2 — 向量 2
 * @return 点积结果
 */
unsigned char DotProduct(const unsigned char *vector1, unsigned char *vector2)
{
    unsigned char result = 0, i;

    for (i = 4; i > 0; i--)
        result ^= Multiply(*vector1++, *vector2++);

    return result;
}

/**
 * @brief  AES MixColumn 正向变换（单列）
 * @note   使用固定矩阵 [2,3,1,1; 1,2,3,1; 1,1,2,3; 3,1,1,2] 与列向量相乘。
 *         矩阵行存储两次以消除循环移位需求。
 * @param  column — 4 字节列向量（原地修改）
 */
void MixColumn(unsigned char *column)
{
    const unsigned char row[8] = {
        0x02, 0x03, 0x01, 0x01,
        0x02, 0x03, 0x01, 0x01
    };  /* 第一行存储两次，消除循环移位 */

    unsigned char result[4];

    /* 分别计算 4 行的点积 */
    result[0] = DotProduct(row + 0, column);
    result[1] = DotProduct(row + 3, column);
    result[2] = DotProduct(row + 2, column);
    result[3] = DotProduct(row + 1, column);

    CopyBytes(column, result, 4);
}

/* =========================================================================
 *  12. 内部函数实现 —— 正向 ShiftRows
 * ======================================================================= */

/**
 * @brief  正向行移位变换
 * @note   状态按列排列，ShiftRows 将各行循环左移：
 *         - 第 2 行：左移 1 位
 *         - 第 3 行：左移 2 位
 *         - 第 4 行：左移 3 位（即右移 1 位）
 * @param  state — 16 字节状态矩阵（4×4，列优先排列）
 */
void ShiftRows(unsigned char *state)
{
    unsigned char temp;

    /* 第 2 行：循环左移 1 位 */
    temp = state[1 + 0 * 4];
    state[1 + 0 * 4] = state[1 + 1 * 4];
    state[1 + 1 * 4] = state[1 + 2 * 4];
    state[1 + 2 * 4] = state[1 + 3 * 4];
    state[1 + 3 * 4] = temp;

    /* 第 3 行：循环左移 2 位 */
    temp = state[2 + 0 * 4];
    state[2 + 0 * 4] = state[2 + 2 * 4];
    state[2 + 2 * 4] = temp;
    temp = state[2 + 1 * 4];
    state[2 + 1 * 4] = state[2 + 3 * 4];
    state[2 + 3 * 4] = temp;

    /* 第 4 行：循环左移 3 位（即右移 1 位） */
    temp = state[3 + 3 * 4];
    state[3 + 3 * 4] = state[3 + 2 * 4];
    state[3 + 2 * 4] = state[3 + 1 * 4];
    state[3 + 1 * 4] = state[3 + 0 * 4];
    state[3 + 0 * 4] = temp;
}

/* =========================================================================
 *  13. 核心算法实现 —— AES 加密
 * ======================================================================= */

/**
 * @brief  AES-128 正向密码（加密单个 16 字节块）
 * @note   加密流程：
 *         1. AddRoundKey（第一轮密钥）
 *         2. ROUNDS-1 轮：SubBytes → ShiftRows → MixColumns → AddRoundKey
 *         3. 最后一轮：SubBytes → ShiftRows → AddRoundKey（无 MixColumns）
 * @param  block       — 16 字节数据块（原地加密）
 * @param  expandedKey — 扩展密钥
 */
void Cipher(unsigned char *block, unsigned char *expandedKey)
{
    unsigned char i, j;
    unsigned char round = ROUNDS - 1;

    /* 初始轮密钥加 */
    XORBytes(block, expandedKey, 16);
    expandedKey += BLOCKSIZE;

    /* 主循环：ROUNDS-1 轮 */
    do {
        SubBytes(block, 16);
        ShiftRows(block);

        /* MixColumns：对 4 列分别处理 */
        for (i = 4, j = 0; i > 0; i--, j += 4)
            MixColumn(block + j);

        XORBytes(block, expandedKey, 16);
        expandedKey += BLOCKSIZE;
    } while (--round);

    /* 最后一轮（无 MixColumns） */
    SubBytes(block, 16);
    ShiftRows(block);
    XORBytes(block, expandedKey, 16);
}

/* =========================================================================
 *  14. 公共接口实现 —— AES CBC 加密
 * ======================================================================= */

/**
 * @brief  初始化 AES 加密环境
 * @note   依次计算：指数/对数表 → S-Box → 扩密钥。
 */
void aesEncInit(void)
{
    powTbl = block1;
    logTbl = block2;
    CalcPowLog(powTbl, logTbl);

    sBox = block2;
    CalcSBox(sBox);

    expandedKey = block1;
    KeyExpansion(expandedKey);
}

/**
 * @brief  AES-128 ECB 模式加密单个块（无链式）
 * @param  buffer — 16 字节数据块（原地加密）
 */
void aesEncryptBlock(unsigned char *buffer)
{
    Cipher(buffer, expandedKey);
}

/**
 * @brief  AES-128 CBC 模式加密（单个 16 字节块）
 * @note   CBC 加密流程：
 *         1. 明文与前一个密文块（chainBlock）异或
 *         2. 对异或结果执行 AES 正向密码
 *         3. 更新 chainBlock 为当前密文
 * @param  buffer     — 输入：明文；输出：密文（16 字节）
 * @param  chainBlock — CBC 链式块（上一个密文块）
 */
void aesEncrypt(unsigned char *buffer, unsigned char *chainBlock)
{
    aesEncInit();
    XORBytes(buffer, chainBlock, BLOCKSIZE);     /* CBC 异或     */
    Cipher(buffer, expandedKey);                  /* AES 正向密码 */
    CopyBytes(chainBlock, buffer, BLOCKSIZE);     /* 更新链式块   */
}
