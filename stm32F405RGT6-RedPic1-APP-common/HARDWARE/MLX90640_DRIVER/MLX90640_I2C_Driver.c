#include "MLX90640_I2C_Driver.h"
#include "math.h"

/*
 * 硬件 I2C，本工程当前默认启用，性能和稳定性都更适合持续采帧。
 *
 * 上层 API 只关心“读寄存器 / 写寄存器”，这里负责把这些访问翻译成
 * I2C 总线时序，并在总线异常时尽量避免 MCU 永久卡死。
 */

#define I2Cx               I2C1
#define I2Cx_CLK           RCC_APB1Periph_I2C1
#define I2Cx_GPIO_PORT     GPIOB
#define I2Cx_SCL_PIN       GPIO_Pin_6
#define I2Cx_SDA_PIN       GPIO_Pin_7
#define MLX90640_I2C_ERROR_NACK    (-1)
#define MLX90640_I2C_ERROR_VERIFY  (-2)
#define MLX90640_I2C_ERROR_TIMEOUT (-3)
#define MLX90640_I2C_BUSY_TIMEOUT_US  5000UL
#define MLX90640_I2C_EVENT_TIMEOUT_US 5000UL
#define I2Cx_SPEED         1000000  // 鏍囧噯妯″紡100kHz

/*
 * 超时后主动发 STOP、恢复 ACK，并重新初始化 I2C 外设。
 * 这样做的原因是 I2C 一旦因为噪声、NACK、时序中断等问题卡在 BUSY 状态，
 * 后续所有访问都可能继续失败；先把外设状态机拉回到一个干净的起点更稳妥。
 */
/* Prevent hard lockups if the bus stalls. After a timeout we abort the
 * current transfer and reinitialize the peripheral so the next retry can
 * start from a clean state. */
static void MLX90640_I2CAbortTransfer(void)
{
    I2C_GenerateSTOP(I2Cx, ENABLE);
    I2C_AcknowledgeConfig(I2Cx, ENABLE);
    I2C_Cmd(I2Cx, DISABLE);
    MLX90640_I2CInit();
}

static void MLX90640_I2CTimerInit(void)
{
    if((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0U;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
}

static uint32_t MLX90640_I2CElapsedUs(uint32_t startCycle)
{
    uint32_t cyclesPerUs = SystemCoreClock / 1000000UL;

    if(cyclesPerUs == 0U)
    {
        cyclesPerUs = 1U;
    }

    return (DWT->CYCCNT - startCycle) / cyclesPerUs;
}

/*
 * 等待 I2C 总线空闲。
 * 只要 BUSY 一直不释放，就说明前一笔传输可能没有正常收尾；这里加超时是为了防止死等。
 */
static int MLX90640_I2CWaitWhileBusy(void)
{
    uint32_t startCycle;

    MLX90640_I2CTimerInit();
    startCycle = DWT->CYCCNT;

    while(I2C_GetFlagStatus(I2Cx, I2C_FLAG_BUSY))
    {
        if(MLX90640_I2CElapsedUs(startCycle) >= MLX90640_I2C_BUSY_TIMEOUT_US)
        {
            MLX90640_I2CAbortTransfer();
            return MLX90640_I2C_ERROR_TIMEOUT;
        }
    }

    return 0;
}

/*
 * 等待指定 I2C 事件出现。
 * 读写流程里的每一步都依赖硬件状态机推进，所以这里把“带超时的等待”统一封装起来。
 */
static int MLX90640_I2CWaitEvent(uint32_t event)
{
    uint32_t startCycle;

    MLX90640_I2CTimerInit();
    startCycle = DWT->CYCCNT;

    while(!I2C_CheckEvent(I2Cx, event))
    {
        if(MLX90640_I2CElapsedUs(startCycle) >= MLX90640_I2C_EVENT_TIMEOUT_US)
        {
            MLX90640_I2CAbortTransfer();
            return MLX90640_I2C_ERROR_TIMEOUT;
        }
    }

    return 0;
}

/*
 * 初始化硬件 I2C1。
 * 当前工程默认就是走这一套路径，PB6/PB7 配成 I2C1 的复用开漏模式，由硬件外设负责时序。
 */
void MLX90640_I2CInit(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    I2C_InitTypeDef I2C_InitStruct;
    
    // 1. 浣胯兘鏃堕挓锛堟敞鎰忛『搴忥細鍏圙PIO鍚嶪2C锛?
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);          // GPIOB鏃堕挓
    RCC_APB1PeriphClockCmd(I2Cx_CLK, ENABLE);                      // I2C1鏃堕挓

    // 2. 閰嶇疆GPIO涓哄鐢ㄥ紑婕忔ā寮忥紙鍏抽敭鏀硅繘锛?
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource6, GPIO_AF_I2C1);      // PB6澶嶇敤涓篒2C1_SCL
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource7, GPIO_AF_I2C1);      // PB7澶嶇敤涓篒2C1_SDA

    GPIO_InitStruct.GPIO_Pin = I2Cx_SCL_PIN | I2Cx_SDA_PIN;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF;                  // 澶嶇敤寮€婕忔ā寮?
		GPIO_InitStruct.GPIO_OType = GPIO_OType_OD;//鎺ㄦ尳杈撳嚭
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;                     // 鍚敤鍐呴儴涓婃媺
    GPIO_Init(I2Cx_GPIO_PORT, &GPIO_InitStruct);

    // 3. 閰嶇疆I2C鍙傛暟锛堜慨姝ｅ垎棰戦€昏緫锛?
    I2C_StructInit(&I2C_InitStruct);                              // 鍒濆鍖栭粯璁ゅ€?
    I2C_InitStruct.I2C_Mode = I2C_Mode_I2C;
    I2C_InitStruct.I2C_DutyCycle = I2C_DutyCycle_2;               // 鏍囧噯妯″紡鍗犵┖姣?
    I2C_InitStruct.I2C_ClockSpeed = I2Cx_SPEED;
    I2C_InitStruct.I2C_OwnAddress1 = 0x00;                        // 涓绘ā寮忓湴鍧€鏃犳晥
    I2C_InitStruct.I2C_Ack = I2C_Ack_Enable;                      // 鍚敤ACK鍝嶅簲
    I2C_InitStruct.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    
    // 4. 澶嶄綅I2C澶栬锛堥伩鍏岯usy鏍囧織闂锛?
    RCC_APB1PeriphResetCmd(I2Cx_CLK, ENABLE);                     // 澶嶄綅I2C1
    RCC_APB1PeriphResetCmd(I2Cx_CLK, DISABLE);                    // 閲婃斁澶嶄綅
    
    I2C_Init(I2Cx, &I2C_InitStruct);                              // 搴旂敤閰嶇疆
    
    // 5. 浣胯兘I2C
    I2C_Cmd(I2Cx, ENABLE);
}

/*
 * 用硬件 I2C 从 MLX90640 连续读取多个 16 位寄存器。
 * 逻辑上仍然和软件 I2C 一样：先写寄存器地址，再重复起始切换到读模式，
 * 最后把收到的字节流重新组装成 uint16_t 数据数组。
 */
int MLX90640_I2CRead(uint8_t slaveAddr, uint16_t startAddress, uint16_t nMemAddressRead, uint16_t *data)
{
		uint8_t sa;
    uint8_t cmd[2];
    uint8_t i2cData[1664] = {0};
    uint16_t *p = data;
    int error;
		
		sa = (slaveAddr << 1);
    // 瀹屽叏淇濇寔鍘熷彉閲忓懡鍚嶅拰鎿嶄綔椤哄簭
    cmd[0] = startAddress >> 8;    // 楂樺瓧鑺傚湪鍓嶏紙涓庢ā鎷熶唬鐮佷弗鏍间竴鑷达級
    cmd[1] = startAddress & 0xFF;  // 浣庡瓧鑺傚湪鍚?

    error = MLX90640_I2CWaitWhileBusy();
    if(error != 0)
    {
        return error;
    }
    // 瀵瑰簲鍘烮2CStart()
    I2C_GenerateSTART(I2Cx, ENABLE);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_MODE_SELECT);
    if(error != 0)
    {
        return error;
    }

    // 瀵瑰簲鍘烮2CSendByte(sa)
    I2C_Send7bitAddress(I2Cx, sa, I2C_Direction_Transmitter);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED);
    if(error != 0)
    {
        return error;
    }

    // 淇濇寔鍘焎md鍙戦€侀『搴?
    I2C_SendData(I2Cx, cmd[0]);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED);
    if(error != 0)
    {
        return error;
    }
    I2C_SendData(I2Cx, cmd[1]);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED);
    if(error != 0)
    {
        return error;
    }
		
    // 瀵瑰簲鍘烮2CRepeatedStart()
    I2C_GenerateSTART(I2Cx, ENABLE);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_MODE_SELECT);
    if(error != 0)
    {
        return error;
    }
		
		sa = sa | 0x01;
    // 瀵瑰簲鍘焥a | 0x01鎿嶄綔
    I2C_Send7bitAddress(I2Cx, sa, I2C_Direction_Receiver);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED);
    if(error != 0)
    {
        return error;
    }

    /* Read data bytes */
    for(uint16_t i=0; i<(nMemAddressRead<<1); i++)
    {
        if(i == ((nMemAddressRead<<1)-1)) /* Last byte */
        {
            I2C_AcknowledgeConfig(I2Cx, DISABLE);
            I2C_GenerateSTOP(I2Cx, ENABLE);
        }
        error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_BYTE_RECEIVED);
        if(error != 0)
        {
            return error;
        }
        i2cData[i] = I2C_ReceiveData(I2Cx);
    }

    /* Convert data to uint16_t array */
    for(uint16_t cnt=0; cnt<nMemAddressRead; cnt++)
    {
        uint16_t i = cnt << 1;
        *p++ = ((uint16_t)i2cData[i]<<8) | (uint16_t)i2cData[i+1];
    }
    
    I2C_AcknowledgeConfig(I2Cx, ENABLE); // Restore ACK
    return 0;
}

/*
 * 用硬件 I2C 写单个 16 位寄存器。
 * 普通寄存器写完后会回读一次做校验；这样一旦总线偶发出错，上层能明确收到失败结果。
 * 但 0x8000 是状态寄存器，它的位可能在写完后立即被器件自行更新，因此不能做“必须完全相等”的回读校验。
 */
int MLX90640_I2CWrite(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data)
{
		uint8_t sa;
    uint8_t cmd[4];
    static uint16_t dataCheck;
    int error;
		
		sa = (slaveAddr << 1);
    // 淇濇寔鍘熸暟鎹粨鏋勯『搴?
    cmd[0] = writeAddress >> 8;   // 楂樺瓧鑺傚湪鍓?
    cmd[1] = writeAddress & 0xFF; // 浣庡瓧鑺傚湪鍚?
    cmd[2] = data >> 8;           // 鏁版嵁楂樺瓧鑺?
    cmd[3] = data & 0xFF;         // 鏁版嵁浣庡瓧鑺?

    error = MLX90640_I2CWaitWhileBusy();
    if(error != 0)
    {
        return error;
    }
    // 瀵瑰簲鍘烮2CStart()
    I2C_GenerateSTART(I2Cx, ENABLE);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_MODE_SELECT);
    if(error != 0)
    {
        return error;
    }

    // 鍙戦€佽澶囧湴鍧€
    I2C_Send7bitAddress(I2Cx, sa, I2C_Direction_Transmitter);
    error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED);
    if(error != 0)
    {
        return error;
    }

    // 淇濇寔鍘熸暟鎹彂閫侀『搴?
    for(int i=0; i<4; i++){
        I2C_SendData(I2Cx, cmd[i]);
        error = MLX90640_I2CWaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED);
        if(error != 0)
        {
            return error;
        }
    }

    // 瀵瑰簲鍘烮2CStop()
    I2C_GenerateSTOP(I2Cx, ENABLE);

    // 淇濇寔鍘熸暟鎹牎楠岄€昏緫
    /* 0x8000 is the MLX90640 status register. Its bits can change
     * immediately after the write, so strict read-back equality is not a
     * valid success test here. */
    if(writeAddress == 0x8000)
    {
        return 0;
    }

    error = MLX90640_I2CRead(slaveAddr, writeAddress, 1, &dataCheck);
    if(error != 0)
    {
        return error;
    }

    return (dataCheck == data) ? 0 : MLX90640_I2C_ERROR_VERIFY;
}





