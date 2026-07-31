#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

// $[CMU]
// [CMU]$

// $[LFXO]
// [LFXO]$

// $[PRS.ASYNCH0]
// [PRS.ASYNCH0]$

// $[PRS.ASYNCH1]
// [PRS.ASYNCH1]$

// $[PRS.ASYNCH2]
// [PRS.ASYNCH2]$

// $[PRS.ASYNCH3]
// [PRS.ASYNCH3]$

// $[PRS.ASYNCH4]
// [PRS.ASYNCH4]$

// $[PRS.ASYNCH5]
// [PRS.ASYNCH5]$

// $[PRS.ASYNCH6]
// [PRS.ASYNCH6]$

// $[PRS.ASYNCH7]
// [PRS.ASYNCH7]$

// $[PRS.ASYNCH8]
// [PRS.ASYNCH8]$

// $[PRS.ASYNCH9]
// [PRS.ASYNCH9]$

// $[PRS.ASYNCH10]
// [PRS.ASYNCH10]$

// $[PRS.ASYNCH11]
// [PRS.ASYNCH11]$

// $[PRS.SYNCH0]
// [PRS.SYNCH0]$

// $[PRS.SYNCH1]
// [PRS.SYNCH1]$

// $[PRS.SYNCH2]
// [PRS.SYNCH2]$

// $[PRS.SYNCH3]
// [PRS.SYNCH3]$

// $[GPIO]
// [GPIO]$

// $[TIMER0]
// [TIMER0]$

// $[TIMER1]
// [TIMER1]$

// $[TIMER2]
// [TIMER2]$

// $[TIMER3]
// [TIMER3]$

// $[TIMER4]
// [TIMER4]$

// $[USART0]
// [USART0]$

// $[I2C1]
// [I2C1]$

// $[EUSART1]
// [EUSART1]$

// $[EUSART2]
// [EUSART2]$

// $[LCD]
// [LCD]$

// $[KEYSCAN]
// [KEYSCAN]$

// $[LETIMER0]
// [LETIMER0]$

// $[IADC0]
// [IADC0]$

// $[ACMP0]
// [ACMP0]$

// $[ACMP1]
// [ACMP1]$

// $[VDAC0]
// [VDAC0]$

// $[PCNT0]
// [PCNT0]$

// $[LESENSE]
// [LESENSE]$

// $[HFXO0]
// [HFXO0]$

// $[I2C0]
// [I2C0]$

// $[EUSART0]
// EUSART0 RX on PB02
#ifndef EUSART0_RX_PORT                         
#define EUSART0_RX_PORT                          SL_GPIO_PORT_B
#endif
#ifndef EUSART0_RX_PIN                          
#define EUSART0_RX_PIN                           2
#endif

// EUSART0 TX on PB01
#ifndef EUSART0_TX_PORT                         
#define EUSART0_TX_PORT                          SL_GPIO_PORT_B
#endif
#ifndef EUSART0_TX_PIN                          
#define EUSART0_TX_PIN                           1
#endif

// [EUSART0]$

// $[PTI]
// [PTI]$

// $[MODEM]
// [MODEM]$

// $[CUSTOM_PIN_NAME]
#ifndef LEDR_PORT                               
#define LEDR_PORT                                SL_GPIO_PORT_A
#endif
#ifndef LEDR_PIN                                
#define LEDR_PIN                                 0
#endif

#ifndef _PORT                                   
#define _PORT                                    SL_GPIO_PORT_A
#endif
#ifndef _PIN                                    
#define _PIN                                     1
#endif

#ifndef LEDG_PORT                               
#define LEDG_PORT                                SL_GPIO_PORT_A
#endif
#ifndef LEDG_PIN                                
#define LEDG_PIN                                 4
#endif

#ifndef LEDY_PORT                               
#define LEDY_PORT                                SL_GPIO_PORT_A
#endif
#ifndef LEDY_PIN                                
#define LEDY_PIN                                 5
#endif

#ifndef Key_In_PORT                             
#define Key_In_PORT                              SL_GPIO_PORT_C
#endif
#ifndef Key_In_PIN                              
#define Key_In_PIN                               0
#endif

// [CUSTOM_PIN_NAME]$

#endif // PIN_CONFIG_H

