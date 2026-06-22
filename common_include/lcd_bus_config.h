#ifndef _LCD_BUS_CONFIG_H_
#define _LCD_BUS_CONFIG_H_

// 所有 bus 共用的 DMA queue 深度
// 設 8 可一次入隊 4 × 32KB SPI chunk，不需 drain
#define LCD_BUS_DMA_QUEUE_DEPTH 8

#endif
