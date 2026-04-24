/** @file generated/mp-services.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_MP_SERVICES_H
#define AXL_UEFI_GEN_MP_SERVICES_H

#include "types.h"
#include "status.h"

typedef
VOID
(EFIAPI *EFI_AP_PROCEDURE) (
  IN VOID   *ProcedureArgument
);

typedef struct _EFI_CPU_PHYSICAL_LOCATION {
  UINT32  Package;
  UINT32  Core;
  UINT32  Thread;
} EFI_CPU_PHYSICAL_LOCATION;

typedef struct _EFI_CPU_PHYSICAL_LOCATION2 {
  UINT32  Package;
  UINT32  Module;
  UINT32  Tile;
  UINT32  Die;
  UINT32  Core;
  UINT32  Thread;
} EFI_CPU_PHYSICAL_LOCATION2;

#define PROCESSOR_AS_BSP_BIT        0x00000001

#define PROCESSOR_ENABLED_BIT       0x00000002

#define PROCESSOR_HEALTH_STATUS_BIT 0x00000004

typedef struct _EFI_PROCESSOR_INFORMATION {
  UINT64                            ProcessorId;
  UINT32                            StatusFlag;
  EFI_CPU_PHYSICAL_LOCATION         Location;
  void  *ExtendedInformation;
} EFI_PROCESSOR_INFORMATION;

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_GET_NUMBER_OF_PROCESSORS) (
  IN EFI_MP_SERVICES_PROTOCOL    *This,
  OUT UINTN                      *NumberOfProcessors,
  OUT UINTN                      *NumberOfEnabledProcessors
  );

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_GET_PROCESSOR_INFO) (
  IN EFI_MP_SERVICES_PROTOCOL     *This,
  IN UINTN                        ProcessorNumber,
  OUT EFI_PROCESSOR_INFORMATION   *ProcessorInfoBuffer
  );

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_STARTUP_ALL_APS) (
  IN EFI_MP_SERVICES_PROTOCOL  *This,
  IN EFI_AP_PROCEDURE          Procedure,
  IN BOOLEAN                   SingleThread,
  IN EFI_EVENT                 WaitEvent               OPTIONAL,
  IN UINTN                     TimeoutInMicroSeconds,
  IN VOID                      *ProcedureArgument      OPTIONAL,
  OUT UINTN                    **FailedCpuList         OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_STARTUP_THIS_AP) (
  IN EFI_MP_SERVICES_PROTOCOL  *This,
  IN EFI_AP_PROCEDURE          Procedure,
  IN UINTN                     ProcessorNumber,
  IN EFI_EVENT                 WaitEvent               OPTIONAL,
  IN UINTN                     TimeoutInMicroseconds,
  IN VOID                      *ProcedureArgument      OPTIONAL,
  OUT BOOLEAN                  *Finished               OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_SWITCH_BSP) (
  IN EFI_MP_SERVICES_PROTOCOL    *This,
  IN UINTN                       ProcessorNumber,
  IN BOOLEAN                     EnableOldBSP
  );

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_ENABLEDISABLEAP) (
  IN EFI_MP_SERVICES_PROTOCOL  *This,
  IN UINTN                     ProcessorNumber,
  IN BOOLEAN                   EnableAP,
  IN UINT32                    HealthFlag     OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_MP_SERVICES_WHOAMI) (
  IN EFI_MP_SERVICES_PROTOCOL   *This,
  OUT UINTN                     *ProcessorNumber
  );

typedef struct _EFI_MP_SERVICES_PROTOCOL {
  EFI_MP_SERVICES_GET_NUMBER_OF_PROCESSORS  GetNumberOfProcessors;
  EFI_MP_SERVICES_GET_PROCESSOR_INFO        GetProcessorInfo;
  EFI_MP_SERVICES_STARTUP_ALL_APS           StartupAllAPs;
  EFI_MP_SERVICES_STARTUP_THIS_AP           StartupThisAP;
  EFI_MP_SERVICES_SWITCH_BSP                SwitchBSP;
  EFI_MP_SERVICES_ENABLEDISABLEAP           EnableDisableAP;
  EFI_MP_SERVICES_WHOAMI                    WhoAmI;
} EFI_MP_SERVICES_PROTOCOL;


#endif /* AXL_UEFI_GEN_MP_SERVICES_H */
