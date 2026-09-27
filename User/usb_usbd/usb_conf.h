#ifndef __USB_CONF_H
#define __USB_CONF_H

/* WCH USBD/PMA 的 CDC 端点表：EP1 通知、EP2 OUT、EP3 IN。 */
#define EP_NUM              15
#define BTABLE_ADDRESS      0x00
#define ENDP0_RXADDR        0x40
#define ENDP0_TXADDR        0x80
#define ENDP1_TXADDR        0xC0
#define ENDP2_RXADDR        0x100
#define ENDP3_TXADDR        0x140

#define IMR_MSK (CNTR_CTRM | CNTR_WKUPM | CNTR_SUSPM | CNTR_ERRM | \
                 CNTR_SOFM | CNTR_ESOFM | CNTR_RESETM)

#define EP1_IN_Callback     UsbCdc_EP1_IN_Callback
#define EP2_IN_Callback     NOP_Process
#define EP3_IN_Callback     UsbCdc_EP3_IN_Callback
#define EP4_IN_Callback     NOP_Process
#define EP5_IN_Callback     NOP_Process
#define EP6_IN_Callback     NOP_Process
#define EP7_IN_Callback     NOP_Process

#define EP1_OUT_Callback    NOP_Process
#define EP2_OUT_Callback    UsbCdc_EP2_OUT_Callback
#define EP3_OUT_Callback    NOP_Process
#define EP4_OUT_Callback    NOP_Process
#define EP5_OUT_Callback    NOP_Process
#define EP6_OUT_Callback    NOP_Process
#define EP7_OUT_Callback    NOP_Process

#endif
