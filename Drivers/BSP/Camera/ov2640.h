#ifndef OV2640_H
#define OV2640_H

#include "main.h"
#include "camera.h"

#define CAMERA_Picture 1
int ov2640_init(framesize_t framesize);
int ov2640_init_pic();
void     CAMERA_Delay(uint32_t delay);
void ov2640_set_picture_mode(uint8_t action, uint16_t DeviceAddr);
#endif
