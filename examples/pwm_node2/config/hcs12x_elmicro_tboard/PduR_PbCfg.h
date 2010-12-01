/*
 * Configuration of module PduR (PduR_PbCfg.h)
 *
 * Created by: Arccore AB
 * Configured for (MCU): HCS12
 *
 * Module vendor: ArcCore
 * Module version: 2.0.2
 *
 * 
 * Generated by Arctic Studio (http://arccore.com) 
 *           on Wed Dec 01 17:45:16 CET 2010
 */


#if (PDUR_SW_MAJOR_VERSION != 1) 
#error "PduR: Configuration file version differs from BSW version."
#endif


#if defined(USE_DCM)
#include "Dcm.h"
#endif
#if defined(USE_COM)
#include "Com.h"
#endif
#if defined(USE_CANIF)
#include "CanIf.h"
#endif
#if defined(USE_CANTP)
#include "CanTp.h"
#endif

extern PduR_PBConfigType PduR_Config;

//  PduR Polite Defines.

#define PDUR_SOURCE_PDU_ID_LedCommandRx CANIF_PDU_ID_LedCommandRx
#define PDUR_DEST_PDU_ID_LedCommandRx COM_PDU_ID_LedCommandRx