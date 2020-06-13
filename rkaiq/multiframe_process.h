#ifndef _MULTIFRAME_PROCESS_H__
#define _MULTIFRAME_PROCESS_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>

#include "logger/log.h"

void ConverToLE(uint16_t *buf, uint32_t len);
void MultiFrameAverage(uint32_t *pIn1_pOut, uint16_t width, uint16_t height,
                       uint8_t frameNumber);
void MultiFrameAddition(uint32_t *pIn1_pOut, uint16_t *pIn2, uint16_t width,
                        uint16_t height);
void FrameU32ToU16(uint32_t *pIn, uint16_t *pOut, uint16_t width,
                   uint16_t height);
void FrameU16ToU32(uint16_t *pIn, uint32_t *pOut, uint16_t width,
                   uint16_t height);

#endif
