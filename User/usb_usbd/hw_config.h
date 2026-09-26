#ifndef USB_USBD_HW_CONFIG_H
#define USB_USBD_HW_CONFIG_H

#include "ch32v20x.h"

void Set_USBConfig(void);
void Enter_LowPowerMode(void);
void Leave_LowPowerMode(void);
void USB_Interrupts_Config(void);
void USB_Port_Set(FunctionalState new_state, FunctionalState pin_in_ipu);

#endif
