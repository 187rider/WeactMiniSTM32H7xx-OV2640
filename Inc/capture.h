#ifndef __CAPTURE_H
#define __CAPTURE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "fatfs.h"


// Function to save RGB565 frame as BMP to SD card
uint8_t take_A_Picture(DCMI_HandleTypeDef *hdcmi);
// Optional helper to show status on LCD (if needed)
void Capture_ShowSavedMessage(void);

#ifdef __cplusplus
}
#endif

#endif /* __CAPTURE_H */