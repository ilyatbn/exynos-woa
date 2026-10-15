/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

   Device.c

Abstract:

    This file handles device specific operations.

Environment:

    Kernel mode only

--*/

#include "Driver.h"  
    
#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, DeviceQueryDeviceParameters)
#endif

#define STR_BAUDRATE    L"BaudRateIndex"  

VOID
DeviceQueryDeviceParameters(
    _In_ WDFDRIVER  _Driver
)
/*++
Routine Description:

    Query driver's registry location for device specific parameter, such as baudrate.

        HLM\system\CCS\Services\serialhcibus\Parameters\
        
            KeyName/Type/value

Arguments:

    _Driver - WDF Driver object

Return Value: 

    None
    
--*/   
{
    WDFKEY Key;
    NTSTATUS Status;
    UNICODE_STRING ValueName;
    ULONG Value = 0;

    PAGED_CODE();    

    Status = WdfDriverOpenParametersRegistryKey(_Driver,
                                                GENERIC_READ,
                                                WDF_NO_OBJECT_ATTRIBUTES,
                                                &Key
                                                );
    if (NT_SUCCESS(Status)) {        

        // exynos-woa (notes/bt-wifi-coex.md): Parameters\CoexAclPriority = 0 turns the ACL priority injection off.
        RtlInitUnicodeString(&ValueName, L"CoexAclPriority");
        if (NT_SUCCESS(WdfRegistryQueryULong(Key, &ValueName, &Value))) {
            gCoexAclPriority = Value;
        }
        Value = 0;

        RtlInitUnicodeString(&ValueName, STR_BAUDRATE);
        Status = WdfRegistryQueryULong(Key, &ValueName, &Value);
        
        if (NT_SUCCESS(Status)) {
            // Vendor: can cache and use this values.
        }
       
        WdfRegistryClose(Key);       
    }
    
}


ULONG gCoexAclPriority = 1;   // exynos-woa: Parameters\CoexAclPriority (default on)

NTSTATUS
DeviceEnableWakeControl(
    _In_  WDFDEVICE          _Device,
    _In_  SYSTEM_POWER_STATE _PowerState    
    )
/*++

Routine Description:

    Vendor: This is a device specific function, and it arms the wake mechanism
    for this driver to receive the wake signal.  This could be using an 
    HOST_WAKE GPIO interrupt, or inband CTS/RTS mechanism.  

Arguments:

    _Device - WDF Device object
    _PowerState - Context used for reading data from target UART device

Return Value:

    NTSTATUS
    
--*/      
{
    UNREFERENCED_PARAMETER(_Device);  
    UNREFERENCED_PARAMETER(_PowerState);      
    
    return STATUS_SUCCESS;
}

VOID
DeviceDisableWakeControl(
    WDFDEVICE _Device
    )
/*++

Routine Description:

    Vendor: This is a device specific function, and it disarms the wake mechanism
    for this driver to receive the wake signal.  

Arguments:

    _Device - WDF Device object

Return Value:

    VOID
    
--*/     
{
    UNREFERENCED_PARAMETER(_Device);
   
    return;
}

BOOLEAN
DeviceInitialize(
    _In_  PFDO_EXTENSION _FdoExtension,
    _In_  WDFIOTARGET    _IoTargetSerial,
    _In_  WDFREQUEST     _RequestSync,
    _In_  BOOLEAN        _IsUartReset
    )
/*++
Routine Description:

    This function perform device specific operations to intialize in order
    to bring the device to operational state.

Arguments:

    _FdoExtension - Function device object extension    

    _IoTargetSerial - IO Target to issue request to serial port

    _RequestSync - A reuseable WDF Request to issue serial control

    -IsUartReset - Is UART Reset is required

Return Value:

    TRUE if initialization is completed and successful; FALSE otherwise.

--*/    
{
    UNREFERENCED_PARAMETER(_IoTargetSerial);
    UNREFERENCED_PARAMETER(_RequestSync);
    UNREFERENCED_PARAMETER(_IsUartReset);

    //
    // exynos-woa (D4c W2): BCM4375 bring-up over the direct-MMIO UART - UART set-up, HCI_RESET, version, patchram
    // while the chip still runs its ROM, then interrupt mode (BtUart.c). Clocks, pins and bt_en are latched by UEFI.
    //
    return BtUartBringUp(&_FdoExtension->Uart);
}

NTSTATUS
DeviceEnable(
    _In_ WDFDEVICE _Device,
    _In_ BOOLEAN   _IsEnabled
    )

/*++

Routine Description:

    This function enable/wake serial bus device.

Arguments:

    _Device - Supplies a handle to the framework device object.

    _IsEnabled - Boolean to enable or disable the BT device.
    

Return Value:

    NTSTATUS code. 

--*/

{
    UNREFERENCED_PARAMETER(_Device);  
    UNREFERENCED_PARAMETER(_IsEnabled);  
    
    return STATUS_SUCCESS;
}


NTSTATUS 
DevicePowerOn(
    _In_  WDFDEVICE _Device
)
/*++

Routine Description:

    This routine powers on the serial bus device

Arguments:

    _Device - Supplies a handle to the framework device object.

Return Value:

    NT status code.

--*/
{
    UNREFERENCED_PARAMETER(_Device);    
    
    return STATUS_SUCCESS;
}

NTSTATUS 
DevicePowerOff(
    _In_  WDFDEVICE _Device
)
/*++

Routine Description:

    This routine powers off the serial bus device

Arguments:

    _Device - Supplies a handle to the framework device object.

Return Value:

    NT status code.

--*/
{
    UNREFERENCED_PARAMETER(_Device);    
    
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
DeviceDoPLDR(
    WDFDEVICE _Fdo
    )
/*++

Routine Description:

    This vendor-specific routine takes appropriate actions necessary to fully reset the device.

Arguments:

    _Fdo - Framework device object representing the FDO.

Return Value:

    VOID.

--*/
{
    UNREFERENCED_PARAMETER(_Fdo);
}
