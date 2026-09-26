#ifndef __USB_CONF_H
#define __USB_CONF_H

/* CH32V203F6P6 的 USBD PMA 缓冲区地址，单位为 PMA 字节偏移。 */
#define EP_NUM              2
#define BTABLE_ADDRESS      0x00
#define ENDP0_RXADDR        0x40
#define ENDP0_TXADDR        0x80
#define ENDP1_RXADDR        0xC0
#define ENDP1_TXADDR        0x100

/* 维护设备不使用帧同步或挂起恢复中断，只开事务、错误和总线复位。 */
#define IMR_MSK (CNTR_CTRM | CNTR_DOVRM | CNTR_ERRM | CNTR_RESETM)

#define EP1_IN_Callback     UsbWchUsbd_EP1_IN_Callback
#define EP1_OUT_Callback    UsbWchUsbd_EP1_OUT_Callback
#define EP2_IN_Callback     NOP_Process
#define EP3_IN_Callback     NOP_Process
#define EP4_IN_Callback     NOP_Process
#define EP5_IN_Callback     NOP_Process
#define EP6_IN_Callback     NOP_Process
#define EP7_IN_Callback     NOP_Process
#define EP2_OUT_Callback    NOP_Process
#define EP3_OUT_Callback    NOP_Process
#define EP4_OUT_Callback    NOP_Process
#define EP5_OUT_Callback    NOP_Process
#define EP6_OUT_Callback    NOP_Process
#define EP7_OUT_Callback    NOP_Process

#endif
