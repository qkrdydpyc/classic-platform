/* -------------------------------- Arctic Core ------------------------------
 * Arctic Core - the open source AUTOSAR platform http://arccore.com
 *
 * Copyright (C) 2009  ArcCore AB <contact@arccore.com>
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation; See <http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 * -------------------------------- Arctic Core ------------------------------*/


#include "Can.h"
#include "core_cr4.h"
#if defined(USE_DEM)
#include "Dem.h"
#endif
#include "Det.h"
#include "CanIf_Cbk.h"
#include "Os.h"
#include "irq.h"
#include "Mcu.h"

#define DCAN1_MAX_MESSAGEBOXES 64
#define DCAN2_MAX_MESSAGEBOXES 64
#define DCAN3_MAX_MESSAGEBOXES 32

#define CAN_TIMEOUT_DURATION   0xFFFFFFFF
#define CAN_INSTANCE           0


#if !defined(USE_DEM)
#define Dem_ReportErrorStatus(...)
#endif


/*----------------------------------------------------------------------------*/
/* Variable Definition                                                        */

// Array for easy access to DCAN register definitions.
static Can_RegisterType* CanBase[]=
{
	DCAN1_Base,
	DCAN2_Base,
	DCAN3_Base
};

typedef enum
{
    CAN_UNINIT,
    CAN_READY
} Can_StateType;


typedef struct
{
    uint16       MaxBoxes;
    uint32       WakeupSrc;
    Can_PduType *PduPtr;
    uint8       *CancelPtr;
    uint8       *TxPtr;    
} Controller_PreConfigType;


#if(CAN_DEV_ERROR_DETECT == STD_ON)
/* Module is in uninitialized state */
static Can_StateType            ModuleState = CAN_UNINIT;
#endif

static CanIf_ControllerModeType ControllerMode[CAN_ARC_CTRL_CONFIG_CNT];

/* Used to switch between IF1 and IF2 of DCAN */
static uint8 IfRegId = 0;

/* Used to order Data Bytes according to hardware registers in DCAN */
static const uint8 ElementIndex[] = {3, 2, 1, 0, 7, 6, 5, 4};

/* To save pointer to the configuration set */
static const Can_ConfigType *CurConfig;

/* To save the PduData of transmission objects */
static Can_PduType PduInfoArray_1[DCAN1_MAX_MESSAGEBOXES];
/* To save pending Cancel Requests of transmission objects */
static uint8 CancelRqstArray_1[DCAN1_MAX_MESSAGEBOXES];
/* To save pending Transmit Requests of transmission objects */
static uint8 TxRqstArray_1[DCAN1_MAX_MESSAGEBOXES];

static Can_PduType PduInfoArray_2[DCAN2_MAX_MESSAGEBOXES];
static uint8 CancelRqstArray_2[DCAN2_MAX_MESSAGEBOXES];
static uint8 TxRqstArray_2[DCAN2_MAX_MESSAGEBOXES];

static Can_PduType PduInfoArray_3[DCAN3_MAX_MESSAGEBOXES];
static uint8 CancelRqstArray_3[DCAN3_MAX_MESSAGEBOXES];
static uint8 TxRqstArray_3[DCAN3_MAX_MESSAGEBOXES];


/* Holds the Controller specific configuration */ 
static Controller_PreConfigType ControllerConfig[] =
{
    { // DCAN1
        .MaxBoxes = DCAN1_MAX_MESSAGEBOXES,
        .WakeupSrc = 0,
        .PduPtr = PduInfoArray_1,
        .CancelPtr = CancelRqstArray_1,
        .TxPtr = TxRqstArray_1
    },
    { // DCAN2
        .MaxBoxes = DCAN2_MAX_MESSAGEBOXES,
        .WakeupSrc = 1,
        .PduPtr = PduInfoArray_2,
        .CancelPtr = CancelRqstArray_2,
        .TxPtr = TxRqstArray_2
    },
    { // DCAN3
		.MaxBoxes = DCAN3_MAX_MESSAGEBOXES,
		.WakeupSrc = 1,
		.PduPtr = PduInfoArray_3,
		.CancelPtr = CancelRqstArray_3,
		.TxPtr = TxRqstArray_3
	}

};

/* Shadow Buffer is used for buffering of received data */ 
static uint8 RxShadowBuf[CAN_ARC_CTRL_CONFIG_CNT][8];

/* Driver must know how often Can_DisableControllerInterrupts() has been called */
static uint32 IntDisableCount[CAN_ARC_CTRL_CONFIG_CNT];


static inline const Can_HardwareObjectType * Can_FindTxHoh(Can_Arc_HTHType hth) {
	for (int i = 0; i < CAN_ARC_CTRL_CONFIG_CNT; i++) {
		const Can_HardwareObjectType * hoh = CanControllerConfigData[i].Can_Arc_Hoh;
		hoh--;
		do {
			hoh++;
			if (hoh->CanObjectType == CAN_OBJECT_TYPE_TRANSMIT && hoh->CanObjectId == hth) {
				return hoh;
			}
		} while (!hoh->Can_Arc_EOL);
	}
	return 0;
}

static inline uint8 Can_FindControllerId(Can_Arc_HTHType hth) {
	for (int i = 0; i < CAN_ARC_CTRL_CONFIG_CNT; i++) {
		const Can_HardwareObjectType * hoh = CanControllerConfigData[i].Can_Arc_Hoh;
		hoh--;
		do {
			hoh++;
			if (hoh->CanObjectType == CAN_OBJECT_TYPE_TRANSMIT && hoh->CanObjectId == hth) {
				return i;
			}
		} while (!hoh->Can_Arc_EOL);
	}
	return 0;
}

static inline const Can_HardwareObjectType * Can_FindRxHoh(CanControllerIdType Controller, uint8 msgNr) {
	const Can_HardwareObjectType * hoh = CanControllerConfigData[Controller].Can_Arc_Hoh;
	hoh--;
	do {
		hoh++;
		if (hoh->CanObjectType == CAN_OBJECT_TYPE_RECEIVE
				&& (hoh->Can_Arc_MbMask & (1 << (msgNr - 1))) ) {
			return hoh;
		}
	} while (!hoh->Can_Arc_EOL);
	return 0;
}


void Can_InterruptHandler(CanControllerIdType controller)
{
    uint32  ErrCounter;
    uint32  MsgNr;
    uint32  MsgId;
    uint8   MsgDlc;
    uint8   DataByteIndex;
    uint8  *SduPtr;

    //Can_DisableControllerInterrupts(0);

    ErrCounter = CAN_TIMEOUT_DURATION;

    uint32 ir = CanBase[controller]->IR;

    if(ir == 0x8000)
    {
    	uint32 sr = CanBase[controller]->SR;
        /* WakeUp Pending */
        if(sr & 0x00000200) {
            /* Set Init Bit, so that Controller is in Stop state */
            CanBase[controller]->CTL |= 0x1;
           // EcuM_CheckWakeUp(ControllerConfig[0].WakeupSrc);

        }
        /* Bus Off */
        if(sr & 0x00000080) {
        	Can_SetControllerMode(controller, CAN_T_STOP); // CANIF272
            //CanIf_ControllerBusOff(0); // Not implemented in Arctic Core

        }
    }
    else
    {
        MsgNr = ir;

        /* Read Arbitration, Control and Data Bits and clear IntPnd and NewDat*/
        CanBase[controller]->IFx[IfRegId].COM = 0x003F0000 | MsgNr;

        /* Wait until Busy Flag is 0 */
        while(CanBase[controller]->IFx[IfRegId].COM & 0x8000)
        {
            ErrCounter--;
            if(ErrCounter == 0)
            {
                Dem_ReportErrorStatus(CAN_E_TIMEOUT, DEM_EVENT_STATUS_FAILED);
                ErrCounter = CAN_TIMEOUT_DURATION;
                return;
            }
        }

        /* Transmit Object */
        if(CanBase[controller]->IFx[IfRegId].ARB & 0x20000000)
        {
            /* Reset TxRqst-Array Element */
        	ControllerConfig[controller].TxPtr[MsgNr - 1] = 0;
            /* A Message was successfully transmitted */
            CanIf_TxConfirmation(ControllerConfig[controller].PduPtr[MsgNr - 1].swPduHandle);
        }
        /* Receive Object */
        else
        {
            /* Extended Id */
            if(CanBase[controller]->IFx[IfRegId].ARB & 0x40000000)
            {
                /* Bring Id to standardized format (MSB marks extended Id) */
                MsgId = (CanBase[controller]->IFx[IfRegId].ARB & 0x1FFFFFFF) | 0x80000000;
            }
            /* Standard Id */
            else
            {
                /* Bring Id to standardized format (MSB marks extended Id) */
                MsgId = (CanBase[controller]->IFx[IfRegId].ARB & 0x1FFC0000) >> 18;
            }
            /* DLC (Max 8) */
            MsgDlc = CanBase[controller]->IFx[IfRegId].MC & 0x000F;
            if(MsgDlc > 8)
            {
                MsgDlc = 8;
            }
            /* Let SduPtr point to Shadow Buffer */
            SduPtr = RxShadowBuf[controller];

            /* Copy Message Data to Shadow Buffer */
            for(DataByteIndex = 0; DataByteIndex < MsgDlc; DataByteIndex++)
            {
            	SduPtr[DataByteIndex] = CanBase[controller]->IFx[IfRegId].DATx[ElementIndex[DataByteIndex]];
            }
            /* Indicate successful Reception */
            const Can_HardwareObjectType *hoh = Can_FindRxHoh(controller, MsgNr);
            CanIf_RxIndication(hoh->CanObjectId, MsgId, MsgDlc, SduPtr);

        }
    }
    //Can_EnableControllerInterrupts(0);
}

void Can1_InterruptHandler() {
	Can_InterruptHandler(DCAN1);
}

void Can2_InterruptHandler() {
	Can_InterruptHandler(DCAN2);
}

void Can3_InterruptHandler() {
	Can_InterruptHandler(DCAN3);
}

uint32 Can_CalculateBTR(uint8 Controller) {

	uint32 clock = McuE_GetPeripheralClock(PERIPHERAL_CLOCK_CAN);
	uint32 tq1 = CanControllerConfigData[Controller].CanControllerPropSeg + CanControllerConfigData[Controller].CanControllerSeg1;
	uint32 tq2 = CanControllerConfigData[Controller].CanControllerSeg2;
	uint32 ntq = tq1 + tq2 + 1 + 1 + 1;
	uint32 brp = clock / (CanControllerConfigData[Controller].CanControllerBaudRate * 1000 * ntq) - 1;

	uint32 retVal = (brp | (tq1 << 8) | (tq2 << 12));
	return retVal;
}


void Can_Init(const Can_ConfigType *Config)
{
    uint32                    Controller;
    uint8                     MsgNr;
    uint32                    ErrCounter;
    uint32                    Eob;

/* DET Error Check */
#if(CAN_DEV_ERROR_DETECT == STD_ON)
    if(ModuleState != CAN_UNINIT)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 0, CAN_E_TRANSITION);
        return;
    }
    if(Config == NULL)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 0, CAN_E_PARAM_POINTER);
        return;
    }
#endif 
     
    // TODO This should be used instead of other variables in the Can_Lcfg file.
    CurConfig        = Config;

    for(Controller = 0; Controller < CAN_ARC_CTRL_CONFIG_CNT; Controller++)
    {
        ErrCounter = CAN_TIMEOUT_DURATION;

        /* Init, IE, AutomaticRetransmission, ConfChangeEnable, ABO Off,Parity On, SIE and EIE depending on ControllerConfig */
#if(CAN_WAKEUP_SUPPORT == STD_ON)
        CanBase[Controller]->CTL = 0x02001643;// | (CanControllerConfigData[Controller].CanWakeupProcessing >> 8) | (CanControllerConfigData[Controller].CanBusOffProcessing >> 7);
#else
        CanBase[Controller]->CTL = 0x00001643;// | (CanControllerConfigData[Controller].CanWakeupProcessing >> 8) | (CanControllerConfigData[Controller].CanBusOffProcessing >> 7);
#endif        
        /* LEC 7, TxOk, RxOk, PER */
        CanBase[Controller]->SR  = 0x0000011F;

        /* Test Mode only for Development time: Silent Loopback */
        //CanBase[Controller]->CTL |= 0x00000080;
        //CanBase[Controller]->TR   = 0x00000018;
            

        // Basic message object initialization
        for(MsgNr = 0; MsgNr < ControllerConfig[Controller].MaxBoxes; MsgNr++) {
            /* Initialize the Arrays for Transmit and Cancellation handling */
            *(ControllerConfig[Controller].CancelPtr + MsgNr) = 0;
            *(ControllerConfig[Controller].TxPtr     + MsgNr) = 0;
            
            /* Wait until Busy Flag is 0 */
            while(CanBase[Controller]->IFx[IfRegId].COM & 0x00008000)
            {
                ErrCounter--;
                if(ErrCounter == 0)
                {
                    Dem_ReportErrorStatus(CAN_E_TIMEOUT, DEM_EVENT_STATUS_FAILED);
                    ErrCounter = CAN_TIMEOUT_DURATION;
                    return;
                }
            }

            // Initialize all message objects for this controller to invalid state.
            /* Valid = 0 */
			CanBase[Controller]->IFx[IfRegId].ARB = 0x00000000;
			/* Start writing Arbitration Bits */
			CanBase[Controller]->IFx[IfRegId].COM = 0x00A80000 | (MsgNr + 1);

			/* Use IFx[0] and IFx[1] alternating */
			IfRegId ^= 1;

        }

		/* Configure the HOHs for this controller. */
		const Can_HardwareObjectType* hoh;
		hoh = CanControllerConfigData[Controller].Can_Arc_Hoh;
		hoh--;
		do {
			hoh++;
			uint64 mbMask = hoh->Can_Arc_MbMask;
			uint32 mbNr = 0;
			uint32 nProcessedMb = 0;

			// For every message object in this hoh
			for(; mbMask != 0; mbMask >>= 1) {
				mbNr++;
				if (!(mbMask & 1)) {
					// This message object is not part of this hoh.
					continue;
				}
				nProcessedMb++;

				// Check if this is the last message box for this hoh.
				if((mbMask != 1) &&
				   (hoh->CanObjectType == CAN_OBJECT_TYPE_RECEIVE)) {
					/* EndOfBlock Bit will not be set */
					Eob = 0x00000000;
				}
				else
				{
					/* EndOfBlock Bit will be set */
					Eob = 0x00000080;
				}

				/* DLC=8, Use Mask only for receive, Set RxIE/TxIE depending on pre-config settings, Eob */
				CanBase[Controller]->IFx[IfRegId].MC = 0x00001008 | CanControllerConfigData[Controller].CanRxProcessing | (CanControllerConfigData[Controller].CanTxProcessing << 1) | Eob & ~(hoh->CanObjectType >> 17);

				if(hoh->CanIdType == CAN_ID_TYPE_STANDARD)      /* Standard Identifiers */
				{
					/* Only Standard-Ids are accepted, Set Mask */
					CanBase[Controller]->IFx[IfRegId].MASK = 0x80000000 | ((*(hoh->CanFilterMaskRef)) & 0x1FFFFFFF);
					/* Message valid, Id, Direction */
					CanBase[Controller]->IFx[IfRegId].ARB  = 0x80000000 | ((hoh->CanIdValue & 0x7FF) << 18) | hoh->CanObjectType;
				}
				else if(hoh->CanIdType == CAN_ID_TYPE_EXTENDED) /* Extended Identifiers */
				{
					/* Only Extended-Ids are accepted, Set Mask */
					CanBase[Controller]->IFx[IfRegId].MASK = 0x80000000 | ((*(hoh->CanFilterMaskRef)) & 0x1FFFFFFF);
					/* Message valid, Id, Direction */
					CanBase[Controller]->IFx[IfRegId].ARB  = 0xC0000000 | (hoh->CanIdValue & 0x1FFFFFFF) | hoh->CanObjectType;
				}
				else /* Mixed Identifiers */
				{
					/* Standard- and Mixed-Ids are accepted, Set Mask */
					CanBase[Controller]->IFx[IfRegId].MASK = 0x00000000 | ((*(hoh->CanFilterMaskRef)) & 0x1FFFFFF);
					/* Message valid, Id, Direction */
					CanBase[Controller]->IFx[IfRegId].ARB  = 0xC0000000 | (hoh->CanIdValue & 0x1FFFFFF) | hoh->CanObjectType;
				}
				/* Start writing Mask, Arb, Control and Id bits */
				CanBase[Controller]->IFx[IfRegId].COM  = 0x00F80000 | mbNr;

				/* Use IFx[0] and IFx[1] alternating */
				IfRegId ^= 1;

			}
		} while (!hoh->Can_Arc_EOL);


        /* Set Bit Timing Register */
        CanBase[Controller]->BTR = Can_CalculateBTR(Controller);

        /* Reset CCE Bit */
        CanBase[Controller]->CTL &= ~0x00000040;

#if(CAN_DEV_ERROR_DETECT == STD_ON)
        /* Switch Controller State to CANIF_CS_STOPPED */
        ControllerMode[Controller] = CANIF_CS_STOPPED;
#endif

        // Install interrupt handlers
		TaskType tid;
		if (CanControllerConfigData[Controller].CanControllerId == DCAN1) {
			tid = Os_Arc_CreateIsr(Can1_InterruptHandler, 2 ,"DCAN1Level0");
			Irq_AttachIsr2(tid, NULL, 16);

			tid = Os_Arc_CreateIsr(Can1_InterruptHandler, 2, "DCAN1Level1");
			Irq_AttachIsr2(tid, NULL, 29);

		} else if (CanControllerConfigData[Controller].CanControllerId == DCAN2) {
			tid = Os_Arc_CreateIsr(Can2_InterruptHandler, 2 ,"DCAN2Level0");
			Irq_AttachIsr2(tid, NULL, 35);

			tid = Os_Arc_CreateIsr(Can2_InterruptHandler, 2, "DCAN2Level1");
			Irq_AttachIsr2(tid, NULL, 42);

		} else if (CanControllerConfigData[Controller].CanControllerId == DCAN3) {
			tid = Os_Arc_CreateIsr(Can3_InterruptHandler, 2 ,"DCAN3Level0");
			Irq_AttachIsr2(tid, NULL, 45);

			tid = Os_Arc_CreateIsr(Can3_InterruptHandler, 2, "DCAN3Level1");
			Irq_AttachIsr2(tid, NULL, 55);

		}

    }
#if(CAN_DEV_ERROR_DETECT == STD_ON)
    /* Switch Module State to CAN_READY */
    ModuleState = CAN_READY;
#endif



}



void Can_InitController(uint8 Controller, const Can_ControllerConfigType* Config)
{
    uint8   MsgNr;
    uint32  ErrCounter;

#if(CAN_DEV_ERROR_DETECT == STD_ON)
    if(Config == NULL)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 2, CAN_E_PARAM_POINTER);
        return;
    }
    if(ModuleState == CAN_UNINIT)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 2, CAN_E_UNINIT);
        return;
    }
    if(Controller >= CAN_ARC_CTRL_CONFIG_CNT)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 2, CAN_E_PARAM_CONTROLLER);
        return;
    }
    if(ControllerMode[Controller] != CANIF_CS_STOPPED)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 2, CAN_E_TRANSITION);
        return;
    }
#endif 

    ErrCounter = CAN_TIMEOUT_DURATION;
    
    //for(MsgNr = 0; MsgNr < ControllerConfig[Controller].MessageBoxCount; MsgNr++)
	const Can_HardwareObjectType* hoh;
	hoh = CanControllerConfigData[Controller].Can_Arc_Hoh;
	uint64 mbMask = hoh->Can_Arc_MbMask;
	uint32 nProcessedMb = 0;
	MsgNr = 0;

	// For every message object in this hoh
	for(; mbMask != 0; mbMask >>= 1) {
		if (!(mbMask & 1)) {
			// This message object is not part of this hoh.
			continue;
		}
		MsgNr++;
		nProcessedMb++;

        /* Wait until Busy Flag is 0 */
        while(CanBase[Controller]->IFx[IfRegId].COM & 0x00008000)
        {
            ErrCounter--;
            if(ErrCounter == 0)
            {
                Dem_ReportErrorStatus(CAN_E_TIMEOUT, DEM_EVENT_STATUS_FAILED);
                ErrCounter = CAN_TIMEOUT_DURATION;
                return;
            }
        }
        /* Read actual MaskRegister value of MessageObject */
        CanBase[Controller]->IFx[IfRegId].COM = 0x004C0000 | (MsgNr);

        /* Wait until Busy Flag is 0 */
        while(CanBase[Controller]->IFx[IfRegId].COM & 0x00008000)
        {
            ErrCounter--;
            if(ErrCounter == 0)
            {
                Dem_ReportErrorStatus(CAN_E_TIMEOUT, DEM_EVENT_STATUS_FAILED);
                ErrCounter = CAN_TIMEOUT_DURATION;
                return;
            }
        }
        CanBase[Controller]->IFx[IfRegId].MASK &= 0xD0000000;
        /* Set new Mask */
        CanBase[Controller]->IFx[IfRegId].MASK |= (*(hoh->CanFilterMaskRef)) & 0x1FFFFFFF;
        /* Write new Mask to MaskRegister */
        CanBase[Controller]->IFx[IfRegId].COM   = 0x00C80000 | (MsgNr);

        IfRegId ^= 1;
    }

    /* Wait until Busy Flag is 0 */
    while(CanBase[Controller]->IFx[IfRegId].COM & 0x00008000)
    {
        ErrCounter--;
        if(ErrCounter == 0)
        {
            Dem_ReportErrorStatus(CAN_E_TIMEOUT, DEM_EVENT_STATUS_FAILED);
            ErrCounter = CAN_TIMEOUT_DURATION;
            return;
        }
    }   
    /* Set CCE Bit to allow access to BitTiming Register (Init already set, in mode "stopped") */
    CanBase[Controller]->CTL |= 0x00000040;
    /* Set Bit Timing Register */
    CanBase[Controller]->BTR = Can_CalculateBTR(Controller);
    /* Clear CCE Bit */
    CanBase[Controller]->CTL &= ~0x00000040;

}


Can_ReturnType Can_SetControllerMode(uint8 Controller, Can_StateTransitionType Transition)
{
    Can_ReturnType Status     = CAN_OK;
    uint32         ErrCounter = CAN_TIMEOUT_DURATION;
    uint32         RegBuf;
    
#if(CAN_DEV_ERROR_DETECT == STD_ON)    
    if(ModuleState == CAN_UNINIT)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 3, CAN_E_UNINIT);
        return CAN_NOT_OK;
    }
    if(Controller >= CAN_ARC_CTRL_CONFIG_CNT)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 3, CAN_E_PARAM_CONTROLLER);
        return CAN_NOT_OK;
    }
    if(((Transition == CAN_T_START ) && (ControllerMode[Controller] != CANIF_CS_STOPPED)) ||
       ((Transition == CAN_T_STOP  ) && (ControllerMode[Controller] != CANIF_CS_STARTED)) ||
       ((Transition == CAN_T_SLEEP ) && (ControllerMode[Controller] != CANIF_CS_STOPPED)) ||
       ((Transition == CAN_T_WAKEUP) && (ControllerMode[Controller] != CANIF_CS_SLEEP  )))
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 3, CAN_E_TRANSITION);
        return CAN_NOT_OK;
    }
#endif 

    switch(Transition)
    {
    case CAN_T_START:
        /* Clear Init Bit */
        CanBase[Controller]->CTL  &= ~0x00000001;
        /* Clear Status Register */
        CanBase[Controller]->SR    = 0x0000011F;

        ControllerMode[Controller] = CANIF_CS_STARTED;
        Can_EnableControllerInterrupts(Controller);
        break;

    case CAN_T_STOP:
        /* Set Init Bit */
        CanBase[Controller]->CTL  |=  0x00000001;
        ControllerMode[Controller] = CANIF_CS_STOPPED;
        Can_DisableControllerInterrupts(Controller);
        break;

    case CAN_T_SLEEP:
        /* Set PDR  Bit */
        CanBase[Controller]->CTL |=  0x01000000;
        /* Save actual Register status */
        RegBuf = CanBase[Controller]->CTL;
        /* Disable Status Interrupts and WUBA */
        CanBase[Controller]->CTL &= ~0x02000004;
        /* Wait until Local Power Down Mode acknowledged */
        while(!(CanBase[Controller]->SR & 0x00000400))
        {
            /* Check if a WakeUp occurs */
            if(CanBase[Controller]->SR & 0x00000200)
            {
                Status = CAN_NOT_OK;
                break;
            }
            ErrCounter--;
            if(ErrCounter == 0)
            {
                Dem_ReportErrorStatus(CAN_E_TIMEOUT, DEM_EVENT_STATUS_FAILED);
                ErrCounter = CAN_TIMEOUT_DURATION;
                Status     = CAN_NOT_OK;
                break;
            }
        }
        /* Reset Control Register */
        CanBase[Controller]->CTL   = RegBuf;
        ControllerMode[Controller] = CANIF_CS_SLEEP;
        break;

    case CAN_T_WAKEUP:
        /* Clear PDR Bit */
        CanBase[Controller]->CTL  &= ~0x01000000;
        ControllerMode[Controller] = CANIF_CS_STOPPED;
        break;

    default:
#if(CAN_DEV_ERROR_DETECT == STD_ON)
        /* Invalid Transition */
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 3, CAN_E_TRANSITION);
        return CAN_NOT_OK;
#endif
        break;
    }

    return Status;
}


void Can_DisableControllerInterrupts(uint8 Controller)
{
#if(CAN_DEV_ERROR_DETECT == STD_ON)
    if(ModuleState == CAN_UNINIT)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 4, CAN_E_UNINIT);
        return;
    }
    if(Controller >= CAN_ARC_CTRL_CONFIG_CNT)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 4, CAN_E_PARAM_CONTROLLER);
        return;
    }
#endif 
    /* Clear IE */
    CanBase[Controller]->CTL &= ~0x00000002;
    /* Increment Disable Counter */
    IntDisableCount[Controller]++;
}

void Can_EnableControllerInterrupts(uint8 Controller)
{
#if(CAN_DEV_ERROR_DETECT == STD_ON)
    if(ModuleState == CAN_UNINIT)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 5, CAN_E_UNINIT);
        return;
    }
    if(Controller >= CAN_ARC_CTRL_CONFIG_CNT)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 5, CAN_E_PARAM_CONTROLLER);
        return;
    }
#endif    
    if(IntDisableCount[Controller] > 0)
    {
        if(IntDisableCount[Controller] == 1)
        {
            /* Set IE */
            CanBase[Controller]->CTL |= 0x00000002;
        }
        IntDisableCount[Controller]--;
    }
}


void Can_Cbk_CheckWakeup(uint8 Controller)
{
	/* TODO not supported yet.
#if(CAN_DEV_ERROR_DETECT == STD_ON)
    if(Controller >= CAN_ARC_CTRL_CONFIG_CNT)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 0x0B, CAN_E_PARAM_CONTROLLER);
        return CAN_NOT_OK;
    }
#endif
    // Check WakeUpPending
    if(CanBase[Controller]->SR & 0x00000200)
    {
        return E_OK;
    }
    else
    {
        return E_NOT_OK;
    }
    */
}


Can_ReturnType Can_Write(Can_Arc_HTHType Hth, Can_PduType *PduInfo)
{
    uint32                 ErrCounter;
    uint8                  ControllerId;
    uint8                  MsgNr;
    uint32                 ArbRegValue;
    uint8                  DataByteIndex;
    uint8                 *CurSduPtr;
    Can_PduType           *CurPduArrayPtr;
    uint8                 *CurCancelRqstPtr;
    uint8                 *CurTxRqstPtr;

    CurSduPtr       = PduInfo->sdu;
    ErrCounter      = CAN_TIMEOUT_DURATION;
    
/* DET Error Check */
#if(CAN_DEV_ERROR_DETECT == STD_ON)
    if(PduInfo == NULL || PduInfo->sdu == NULL)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 6, CAN_E_PARAM_POINTER);
        return CAN_NOT_OK;
    }
    if(ModuleState == CAN_UNINIT)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 6, CAN_E_UNINIT);
        return CAN_NOT_OK;
    }
    if(PduInfo->length > 8)
    {
        Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 6, CAN_E_PARAM_DLC);
        return CAN_NOT_OK;        
    }
#endif

    ControllerId = Can_FindControllerId(Hth);
    
    const Can_HardwareObjectType *hoh = Can_FindTxHoh(Hth);

#if(CAN_DEV_ERROR_DETECT == STD_ON)
    if(hoh->CanObjectType != CAN_OBJECT_TYPE_TRANSMIT)
	{
		Det_ReportError(CAN_MODULE_ID, CAN_INSTANCE, 6, CAN_E_PARAM_HANDLE);
		return CAN_NOT_OK;
	}
#endif
    uint64 mbMask = hoh->Can_Arc_MbMask;
    MsgNr = 0;
    for(; mbMask != 0; mbMask >>= 1) {
		if (!(mbMask & 1)) {
			continue; // This message object is not part of this hoh.
		}
		// Just use the first message object.
		MsgNr++;
    }

    CurPduArrayPtr   = ControllerConfig[ControllerId].PduPtr    + (MsgNr - 1);
    CurCancelRqstPtr = ControllerConfig[ControllerId].CancelPtr + (MsgNr - 1);
    CurTxRqstPtr     = ControllerConfig[ControllerId].TxPtr     + (MsgNr - 1);
    
    /* Bring Id Value to appropriate format and set ArbRegValue */
    if(PduInfo->id & 0x80000000)
    {
        /* MsgVal, Ext, Transmit, Extended Id */ 
        ArbRegValue = 0xD0000000 | (PduInfo->id & 0x1FFFFFFF);
    }
    else
    {
        /* MsgVal, Std, Transmit, Standard Id */ 
        ArbRegValue = 0xA0000000 | ((PduInfo->id & 0x7FF) << 18);
    }

    /* Check if TxRqst Bit of MsgObject is set */
    if(CanBase[ControllerId]->TRx[MsgNr >> 5] & (1 << (MsgNr & 0x1F)))
    {
        return CAN_BUSY;
    }

    /* Wait until Busy Flag is 0 */
    while(CanBase[ControllerId]->IFx[IfRegId].COM & 0x00008000)
    {
        ErrCounter--;
        if(ErrCounter == 0)
        {
            Dem_ReportErrorStatus(CAN_E_TIMEOUT, DEM_EVENT_STATUS_FAILED);
            ErrCounter = CAN_TIMEOUT_DURATION;
            return CAN_NOT_OK;
        }
    }

    /* Set NewDat, TxIE (dep on ControllerConfig), TxRqst, EoB and DLC */
    CanBase[ControllerId]->IFx[IfRegId].MC = 0x00000180 | (0x000F & PduInfo->length) | (CanControllerConfigData[ControllerId].CanTxProcessing << 1);

    /* Set ArbitrationRegister */
    CanBase[ControllerId]->IFx[IfRegId].ARB = ArbRegValue;

    /* Set Databytes */
    for(DataByteIndex = 0; DataByteIndex < PduInfo->length; DataByteIndex++)
    {
        CanBase[ControllerId]->IFx[IfRegId].DATx[ElementIndex[DataByteIndex]] = *CurSduPtr++;
    }

    /* Start transmission to MessageRAM */
    CanBase[ControllerId]->IFx[IfRegId].COM = 0x00BF0000 | MsgNr;
    
    /* Save the PduInfo in PduArray, so that messages can be identified later */
    *CurPduArrayPtr = *PduInfo;
    /* TxRqstArray-Elements are used to identifiy transmitted objects in polling mode */
    *CurTxRqstPtr   = 1;
    
    IfRegId ^= 1;
       
    return CAN_OK;
}


void Can_MainFunction_Write()
{
	// Not supported
}

void Can_MainFunction_Read()
{
	// Not supported
}

void Can_MainFunction_BusOff()
{
  // Not supported
}

void Can_MainFunction_Wakeup()
{
   // Not supported
}


