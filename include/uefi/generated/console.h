/** @file generated/console.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_CONSOLE_H
#define AXL_UEFI_GEN_CONSOLE_H

#include "types.h"
#include "status.h"

typedef
EFI_STATUS
(EFIAPI *EFI_TEXT_RESET) (
 IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL       *This,
 IN BOOLEAN                               ExtendedVerification
 );

typedef
EFI_STATUS
(EFIAPI *EFI_TEXT_STRING) (
 IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL    *This,
 IN CHAR16                             *String
 );

typedef
EFI_STATUS
(EFIAPI *EFI_TEXT_TEST_STRING) (
 IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL       *This,
 IN CHAR16                                *String
 );

typedef
EFI_STATUS
(EFIAPI *EFI_TEXT_QUERY_MODE) (
 IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL          *This,
 IN UINTN                                    ModeNumber,
 OUT UINTN                                   *Columns,
 OUT UINTN                                   *Rows
 );

typedef
EFI_STATUS
(EFIAPI *EFI_TEXT_SET_MODE) (
 IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL          *This,
 IN UINTN                                    ModeNumber
 );

typedef
EFI_STATUS
(EFIAPI *EFI_TEXT_SET_ATTRIBUTE) (
 IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL        *This,
 IN UINTN                                  Attribute
 );

typedef
EFI_STATUS
(EFIAPI *EFI_TEXT_CLEAR_SCREEN) (
 IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL             *This
 );

typedef
EFI_STATUS
(EFIAPI *EFI_TEXT_SET_CURSOR_POSITION) (
 IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL             *This,
 IN UINTN                                       Column,
 IN UINTN                                       Row
 );

typedef
EFI_STATUS
(EFIAPI *EFI_TEXT_ENABLE_CURSOR) (
 IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL             *This,
 IN BOOLEAN                                     Visible
 );

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
 EFI_TEXT_RESET                           Reset;
 EFI_TEXT_STRING                          OutputString;
 EFI_TEXT_TEST_STRING                     TestString;
 EFI_TEXT_QUERY_MODE                      QueryMode;
 EFI_TEXT_SET_MODE                        SetMode;
 EFI_TEXT_SET_ATTRIBUTE                   SetAttribute;
 EFI_TEXT_CLEAR_SCREEN                    ClearScreen;
 EFI_TEXT_SET_CURSOR_POSITION             SetCursorPosition;
 EFI_TEXT_ENABLE_CURSOR                   EnableCursor;
 SIMPLE_TEXT_OUTPUT_MODE                  *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *EFI_INPUT_RESET) (
 IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL                 *This,
 IN BOOLEAN                                        ExtendedVerification
 );

typedef
EFI_STATUS
(EFIAPI *EFI_INPUT_READ_KEY) (
 IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL              *This,
 OUT EFI_INPUT_KEY                              *Key
 );

typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
 EFI_INPUT_RESET                       Reset;
 EFI_INPUT_READ_KEY                    ReadKeyStroke;
 EFI_EVENT                             WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct _EFI_PIXEL_BITMASK {
  UINT32              RedMask;
  UINT32              GreenMask;
  UINT32              BlueMask;
  UINT32              ReservedMask;
 } EFI_PIXEL_BITMASK;

typedef enum {
  PixelRedGreenBlueReserved8BitPerColor,
  PixelBlueGreenRedReserved8BitPerColor,
  PixelBitMask,
  PixelBltOnly,
  PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct _EFI_GRAPHICS_OUTPUT_MODE_INFORMATION {
 UINT32                    Version;
 UINT32                    HorizontalResolution;
 UINT32                    VerticalResolution;
 EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
 EFI_PIXEL_BITMASK         PixelInformation;
 UINT32                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE {
  UINT32                                    MaxMode;
  UINT32                                    Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION      *Info;
 UINTN                                      SizeOfInfo;
  EFI_PHYSICAL_ADDRESS                      FrameBufferBase;
  UINTN                                     FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_BLT_PIXEL {
 UINT8                        Blue;
 UINT8                        Green;
 UINT8                        Red;
 UINT8                        Reserved;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

typedef enum {
 EfiBltVideoFill,
 EfiBltVideoToBltBuffer,
 EfiBltBufferToVideo,
 EfiBltVideoToVideo,
 EfiGraphicsOutputBltOperationMax
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;

typedef
EFI_STATUS
(EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE) (
 IN EFI_GRAPHICS_OUTPUT_PROTOCOL              *This,
 IN UINT32                                    ModeNumber,
 OUT UINTN                                    *SizeOfInfo,
 OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION     **Info
 );

typedef
EFI_STATUS
(EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE) (
 IN EFI_GRAPHICS_OUTPUT_PROTOCOL                *This,
 IN UINT32                                      ModeNumber
 );

typedef
EFI_STATUS
(EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT) (
 IN EFI_GRAPHICS_OUTPUT_PROTOCOL                 *This,
 IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL            *BltBuffer OPTIONAL,
 IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION            BltOperation,
 IN UINTN                                        SourceX,
 IN UINTN                                        SourceY,
 IN UINTN                                        DestinationX,
 IN UINTN                                        DestinationY,
 IN UINTN                                        Width,
 IN UINTN                                        Height,
 IN UINTN                                        Delta OPTIONAL
 );

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
 EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE     QueryMode;
 EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE       SetMode;
 EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT            Blt;
 EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE           *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;


#endif /* AXL_UEFI_GEN_CONSOLE_H */
