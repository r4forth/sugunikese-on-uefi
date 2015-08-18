/** @file
A simple, basic, application showing how the Hello application could be
built using the "Standard C Libraries" from StdLib.

Copyright (c) 2010 - 2011, Intel Corporation. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution. The full text of the license may be found at
http://opensource.org/licenses/bsd-license.

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/MemoryAllocationLib.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>

#include <IndustryStandard/Bmp.h>

#include "kese.h"

EFI_SYSTEM_TABLE	*gST;
EFI_BOOT_SERVICES	*gBS;
EFI_RUNTIME_SERVICES *gRS;

EFI_STATUS
	ConvertBmpToGopBlt (
	IN     VOID      *BmpImage,
	IN     UINTN     BmpImageSize,
	IN OUT VOID      **GopBlt,
	IN OUT UINTN     *GopBltSize,
	OUT UINTN     *PixelHeight,
	OUT UINTN     *PixelWidth
	);

/**
Convert a *.BMP graphics image to a GOP blt buffer. If a NULL Blt buffer
is passed in a GopBlt buffer will be allocated by this routine. If a GopBlt
buffer is passed in it will be used if it is big enough.

@param  BmpImage      Pointer to BMP file
@param  BmpImageSize  Number of bytes in BmpImage
@param  GopBlt        Buffer containing GOP version of BmpImage.
@param  GopBltSize    Size of GopBlt in bytes.
@param  PixelHeight   Height of GopBlt/BmpImage in pixels
@param  PixelWidth    Width of GopBlt/BmpImage in pixels

@retval EFI_SUCCESS           GopBlt and GopBltSize are returned.
@retval EFI_UNSUPPORTED       BmpImage is not a valid *.BMP image
@retval EFI_BUFFER_TOO_SMALL  The passed in GopBlt buffer is not big enough.
GopBltSize will contain the required size.
@retval EFI_OUT_OF_RESOURCES  No enough buffer to allocate.

**/
EFI_STATUS
	ConvertBmpToGopBlt (
	IN     VOID      *BmpImage,
	IN     UINTN     BmpImageSize,
	IN OUT VOID      **GopBlt,
	IN OUT UINTN     *GopBltSize,
	OUT UINTN     *PixelHeight,
	OUT UINTN     *PixelWidth
	)
{
	UINT8                         *Image;
	UINT8                         *ImageHeader;
	BMP_IMAGE_HEADER              *BmpHeader;
	BMP_COLOR_MAP                 *BmpColorMap;
	EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer;
	EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Blt;
	UINT64                        BltBufferSize;
	UINTN                         Index;
	UINTN                         Height;
	UINTN                         Width;
	UINTN                         ImageIndex;
	UINT32                        DataSizePerLine;
	BOOLEAN                       IsAllocated;
	UINT32                        ColorMapNum;

	if (sizeof (BMP_IMAGE_HEADER) > BmpImageSize) {
		return EFI_INVALID_PARAMETER;
	}

	BmpHeader = (BMP_IMAGE_HEADER *) BmpImage;

	if (BmpHeader->CharB != 'B' || BmpHeader->CharM != 'M') {
		return EFI_UNSUPPORTED;
	}

	//
	// Doesn't support compress.
	//
	if (BmpHeader->CompressionType != 0) {
		return EFI_UNSUPPORTED;
	}

	//
	// Only support BITMAPINFOHEADER format.
	// BITMAPFILEHEADER + BITMAPINFOHEADER = BMP_IMAGE_HEADER
	//
	if (BmpHeader->HeaderSize != sizeof (BMP_IMAGE_HEADER) - OFFSET_OF(BMP_IMAGE_HEADER, HeaderSize)) {
		return EFI_UNSUPPORTED;
	}

	//
	// The data size in each line must be 4 byte alignment.
	//
	DataSizePerLine = ((BmpHeader->PixelWidth * BmpHeader->BitPerPixel + 31) >> 3) & (~0x3);
	BltBufferSize = MultU64x32 (DataSizePerLine, BmpHeader->PixelHeight);
	if (BltBufferSize > (UINT32) ~0) {
		return EFI_INVALID_PARAMETER;
	}

	if ((BmpHeader->Size != BmpImageSize) || 
		(BmpHeader->Size < BmpHeader->ImageOffset) ||
		(BmpHeader->Size - BmpHeader->ImageOffset !=  BmpHeader->PixelHeight * DataSizePerLine)) {
			return EFI_INVALID_PARAMETER;
	}

	//
	// Calculate Color Map offset in the image.
	//
	Image       = BmpImage;
	BmpColorMap = (BMP_COLOR_MAP *) (Image + sizeof (BMP_IMAGE_HEADER));
	if (BmpHeader->ImageOffset < sizeof (BMP_IMAGE_HEADER)) {
		return EFI_INVALID_PARAMETER;
	}

	if (BmpHeader->ImageOffset > sizeof (BMP_IMAGE_HEADER)) {
		switch (BmpHeader->BitPerPixel) {
		case 1:
			ColorMapNum = 2;
			break;
		case 4:
			ColorMapNum = 16;
			break;
		case 8:
			ColorMapNum = 256;
			break;
		default:
			ColorMapNum = 0;
			break;
		}
		//
		// BMP file may has padding data between the bmp header section and the bmp data section.
		//
		if (BmpHeader->ImageOffset - sizeof (BMP_IMAGE_HEADER) < sizeof (BMP_COLOR_MAP) * ColorMapNum) {
			return EFI_INVALID_PARAMETER;
		}
	}

	//
	// Calculate graphics image data address in the image
	//
	Image         = ((UINT8 *) BmpImage) + BmpHeader->ImageOffset;
	ImageHeader   = Image;

	//
	// Calculate the BltBuffer needed size.
	//
	BltBufferSize = MultU64x32 ((UINT64) BmpHeader->PixelWidth, BmpHeader->PixelHeight);
	//
	// Ensure the BltBufferSize * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL) doesn't overflow
	//
	if (BltBufferSize > DivU64x32 ((UINTN) ~0, sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL))) {
		return EFI_UNSUPPORTED;
	}
	BltBufferSize = MultU64x32 (BltBufferSize, sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));

	IsAllocated   = FALSE;
	if (*GopBlt == NULL) {
		//
		// GopBlt is not allocated by caller.
		//
		*GopBltSize = (UINTN) BltBufferSize;
		*GopBlt     = AllocatePool (*GopBltSize);
		IsAllocated = TRUE;
		if (*GopBlt == NULL) {
			return EFI_OUT_OF_RESOURCES;
		}
	} else {
		//
		// GopBlt has been allocated by caller.
		//
		if (*GopBltSize < (UINTN) BltBufferSize) {
			*GopBltSize = (UINTN) BltBufferSize;
			return EFI_BUFFER_TOO_SMALL;
		}
	}

	*PixelWidth   = BmpHeader->PixelWidth;
	*PixelHeight  = BmpHeader->PixelHeight;

	//
	// Convert image from BMP to Blt buffer format
	//
	BltBuffer = *GopBlt;
	for (Height = 0; Height < BmpHeader->PixelHeight; Height++) {
		Blt = &BltBuffer[(BmpHeader->PixelHeight - Height - 1) * BmpHeader->PixelWidth];
		for (Width = 0; Width < BmpHeader->PixelWidth; Width++, Image++, Blt++) {
			switch (BmpHeader->BitPerPixel) {
			case 1:
				//
				// Convert 1-bit (2 colors) BMP to 24-bit color
				//
				for (Index = 0; Index < 8 && Width < BmpHeader->PixelWidth; Index++) {
					Blt->Red    = BmpColorMap[((*Image) >> (7 - Index)) & 0x1].Red;
					Blt->Green  = BmpColorMap[((*Image) >> (7 - Index)) & 0x1].Green;
					Blt->Blue   = BmpColorMap[((*Image) >> (7 - Index)) & 0x1].Blue;
					Blt++;
					Width++;
				}

				Blt--;
				Width--;
				break;

			case 4:
				//
				// Convert 4-bit (16 colors) BMP Palette to 24-bit color
				//
				Index       = (*Image) >> 4;
				Blt->Red    = BmpColorMap[Index].Red;
				Blt->Green  = BmpColorMap[Index].Green;
				Blt->Blue   = BmpColorMap[Index].Blue;
				if (Width < (BmpHeader->PixelWidth - 1)) {
					Blt++;
					Width++;
					Index       = (*Image) & 0x0f;
					Blt->Red    = BmpColorMap[Index].Red;
					Blt->Green  = BmpColorMap[Index].Green;
					Blt->Blue   = BmpColorMap[Index].Blue;
				}
				break;

			case 8:
				//
				// Convert 8-bit (256 colors) BMP Palette to 24-bit color
				//
				Blt->Red    = BmpColorMap[*Image].Red;
				Blt->Green  = BmpColorMap[*Image].Green;
				Blt->Blue   = BmpColorMap[*Image].Blue;
				break;

			case 24:
				//
				// It is 24-bit BMP.
				//
				Blt->Blue   = *Image++;
				Blt->Green  = *Image++;
				Blt->Red    = *Image;
				break;

			default:
				//
				// Other bit format BMP is not supported.
				//
				if (IsAllocated) {
					FreePool (*GopBlt);
					*GopBlt = NULL;
				}
				return EFI_UNSUPPORTED;
				break;
			};

		}

		ImageIndex = (UINTN) (Image - ImageHeader);
		if ((ImageIndex % 4) != 0) {
			//
			// Bmp Image starts each row on a 32-bit boundary!
			//
			Image = Image + (4 - (ImageIndex % 4));
		}
	}

	return EFI_SUCCESS;
}

EFI_STATUS
	EFIAPI
	UefiMain (
	IN	EFI_HANDLE			ImageHandle,
	IN	EFI_SYSTEM_TABLE	*SystemTable
	)
{
	UINTN						 i,x,y;
	EFI_STATUS					 Status;
	EFI_TIME					 Time;
	UINTN						 TimeTotal = 0;
	EFI_GRAPHICS_OUTPUT_PROTOCOL *GraphicsOutput;
	VOID						 *GopBlt = NULL;
	UINTN					 	 GopBltSize;
	UINTN						 BmpHeight;
	UINTN						 BmpWidth;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *GraphicsInfo;
	UINTN						 GraphicsInfoSize;
	UINTN						 MaxResolutionMode = 0;
	UINTN						 MaxResolutionVertical = 0;
	UINTN						 MaxResolutionHorizontal = 0;

	gST = SystemTable;
	gBS = gST->BootServices;
	gRS = gST->RuntimeServices;

	Status = gRS->GetTime(&Time, NULL);
	TimeTotal = Time.Hour * 3600000 + Time.Minute * 60000 + Time.Second * 1000 + Time.Nanosecond / 1000000;

	if(TimeTotal % 65536 != 0) return EFI_LOAD_ERROR;

	Status = gBS->LocateProtocol (
		&gEfiGraphicsOutputProtocolGuid,
		NULL,
		&GraphicsOutput
		);

	Status = ConvertBmpToGopBlt(
		shownBmp,
		sizeof(shownBmp),
		&GopBlt,
		&GopBltSize,
		&BmpHeight,
		&BmpWidth
		);

	for(i=0; i<GraphicsOutput->Mode->MaxMode; ++i)
	{
		Status = GraphicsOutput->QueryMode(
			GraphicsOutput,
			(UINT32)i,
			&GraphicsInfoSize,
			&GraphicsInfo
			);
		if(MaxResolutionVertical * MaxResolutionHorizontal < GraphicsInfo->VerticalResolution * GraphicsInfo->HorizontalResolution)
		{
			MaxResolutionVertical = (UINTN)GraphicsInfo->VerticalResolution;
			MaxResolutionHorizontal = (UINTN)GraphicsInfo->HorizontalResolution;

			MaxResolutionMode = i;
		}
	}

	Status = GraphicsOutput->SetMode(
		GraphicsOutput,
		(UINT32)MaxResolutionMode
		);

	for(x = 0; x < MaxResolutionHorizontal ; x += BmpWidth){
		for(y = 0; y < MaxResolutionVertical ; y += BmpHeight){
			UINTN width = MaxResolutionHorizontal - x ;
			UINTN height = MaxResolutionVertical - y;

			if(width > BmpWidth) {width = BmpWidth;} else {continue;}
			if(height > BmpHeight) height = BmpHeight;

			Status = GraphicsOutput->Blt(
						GraphicsOutput,
						GopBlt,
						EfiBltBufferToVideo,
						0,0,
						x,y,
						width, height,
						0
						);
		}	
	}

	while(1) { }

	return EFI_SUCCESS;
}

