/*
 * Configuration of module Mcu (Mcu_Cfg.h)
 *
 * Created by: 
 * Configured for (MCU): HCS12
 *
 * Module vendor: ArcCore
 * Module version: 2.0.2
 *
 * 
 * Generated by Arctic Studio (http://arccore.com) 
 *           on Mon Nov 08 20:30:02 CET 2010
 */


#if (MCU_SW_MAJOR_VERSION != 2) 
#error "Mcu: Configuration file version differs from BSW version."
#endif


#ifndef MCU_CFG_H_
#define MCU_CFG_H_


#define MCU_DEV_ERROR_DETECT 	STD_ON 
#define MCU_PERFORM_RESET_API 	STD_OFF
#define MCU_VERSION_INFO_API 	STD_ON

typedef enum {
	MCU_CLOCKTYPE_Clock = 0,
  MCU_NBR_OF_CLOCKS,
} Mcu_ClockType;

#define MCU_DEFAULT_CONFIG McuConfigData[0]

#endif /*MCU_CFG_H_*/
