#include "spi.h"

void SPI_Initialize(void)
{
    
}

void SPI_WaitForNotBusy(void)
{
    uint32_t i;
    while((SPI1->SR & SPI_SR_BSY) == SPI_SR_BSY);
    
    for(i = 0; i < SPI_BUSY_WAIT_EXTRA; i++);
}

void SPI_WaitForRX(void)
{
    while((SPI1->SR & SPI_SR_RXNE) != SPI_SR_RXNE);
}

void SPI_WaitForTX(void)
{
    while((SPI1->SR & SPI_SR_TXE) != SPI_SR_TXE);
}

void SPI_WriteByte(uint8_t byte)
{
    uint8_t dummy;
    (void)dummy;
    
    SPI_WaitForTX();   
    *(uint8_t *)&(SPI1->DR) = byte; /* Will inititiate 8-bit transmission */
    SPI_WaitForRX();
    dummy = (uint8_t)SPI1->DR;
}

void SPI_WriteBytes(uint8_t* bytes, uint8_t len)
{
    uint8_t i;
    uint8_t dummy;
    (void)dummy;
    
    for(i = 0; i < len; i++)
    {
        SPI_WaitForTX();
        *(uint8_t *)&(SPI1->DR) = bytes[i]; /* Will inititiate 8-bit transmission */
        SPI_WaitForRX();
        dummy = (uint8_t)SPI1->DR;
    }
}

void SPI_ReadByte(uint8_t* byte)
{
    SPI_WaitForTX();
    *(uint8_t *)&(SPI1->DR) = 0xFF; /* Will inititiate 8-bit transmission */
    SPI_WaitForRX();
    *byte = (uint8_t)SPI1->DR;
}

void SPI_ReadBytes(uint8_t* bytes, uint8_t len)
{
    uint8_t i;
    for(i = 0; i < len; i++)
    {
        SPI_WaitForTX();
        *(uint8_t *)&(SPI1->DR) = 0xFF; /* Will inititiate 8-bit transmission */
        SPI_WaitForRX();
        bytes[i] = (uint8_t)SPI1->DR;
    }
}

void SPI_WriteReadBytes(uint8_t* writeBytes, uint8_t* readBytes, uint8_t len)
{
    uint8_t i;
    for(i = 0; i < len; i++)
    {
        SPI_WaitForTX();
        *(uint8_t *)&(SPI1->DR) = writeBytes[i]; /* Will inititiate 8-bit transmission */
        SPI_WaitForRX();
        readBytes[i] = (uint8_t)SPI1->DR;
    }
}
