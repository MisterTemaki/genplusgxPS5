/* Genesis Plus GX PS5: every external name of the vendored 7z decoder (LZMA SDK 26.05) gets the prefix fe7z_, so it
   can't meet the older LZMA SDK libchdr links (core/cd_hw/libchdr/deps/lzma-24.05: LzmaDec_*, CpuArch's z7_x86_cpuid...).
   Force-included into each file of this folder (-include fe7z_names.h) and included by ps5/frontend/fe_7z.cpp before the
   SDK's headers. The Makefile checks after each build that the objects define no other global name (lzma7z-names).
   SPDX-License-Identifier: MIT */
#ifndef FE7Z_NAMES_H
#define FE7Z_NAMES_H
#define Bcj2Dec_Decode fe7z_Bcj2Dec_Decode
#define Bcj2Dec_Decode fe7z_Bcj2Dec_Decode
#define Bcj2Dec_Init fe7z_Bcj2Dec_Init
#define Buf_Create fe7z_Buf_Create
#define Buf_Free fe7z_Buf_Free
#define Buf_Init fe7z_Buf_Init
#define CrcCalc fe7z_CrcCalc
#define CrcGenerateTable fe7z_CrcGenerateTable
#define CrcUpdate fe7z_CrcUpdate
#define CrcUpdateT12 fe7z_CrcUpdateT12
#define Delta_Decode fe7z_Delta_Decode
#define Delta_Encode fe7z_Delta_Encode
#define Delta_Init fe7z_Delta_Init
#define LookInStream_LookRead fe7z_LookInStream_LookRead
#define LookInStream_Read fe7z_LookInStream_Read
#define LookInStream_Read2 fe7z_LookInStream_Read2
#define LookInStream_SeekTo fe7z_LookInStream_SeekTo
#define LookToRead2_CreateVTable fe7z_LookToRead2_CreateVTable
#define Lzma2Dec_Allocate fe7z_Lzma2Dec_Allocate
#define Lzma2Dec_AllocateProbs fe7z_Lzma2Dec_AllocateProbs
#define Lzma2Dec_DecodeToBuf fe7z_Lzma2Dec_DecodeToBuf
#define Lzma2Dec_DecodeToDic fe7z_Lzma2Dec_DecodeToDic
#define Lzma2Dec_Init fe7z_Lzma2Dec_Init
#define Lzma2Dec_Parse fe7z_Lzma2Dec_Parse
#define Lzma2Decode fe7z_Lzma2Decode
#define LzmaDec_Allocate fe7z_LzmaDec_Allocate
#define LzmaDec_AllocateProbs fe7z_LzmaDec_AllocateProbs
#define LzmaDec_DecodeToBuf fe7z_LzmaDec_DecodeToBuf
#define LzmaDec_DecodeToDic fe7z_LzmaDec_DecodeToDic
#define LzmaDec_Free fe7z_LzmaDec_Free
#define LzmaDec_FreeProbs fe7z_LzmaDec_FreeProbs
#define LzmaDec_Init fe7z_LzmaDec_Init
#define LzmaDec_InitDicAndState fe7z_LzmaDec_InitDicAndState
#define LzmaDecode fe7z_LzmaDecode
#define LzmaProps_Decode fe7z_LzmaProps_Decode
#define Ppmd7_Alloc fe7z_Ppmd7_Alloc
#define Ppmd7_Construct fe7z_Ppmd7_Construct
#define Ppmd7_Free fe7z_Ppmd7_Free
#define Ppmd7_Init fe7z_Ppmd7_Init
#define Ppmd7_MakeEscFreq fe7z_Ppmd7_MakeEscFreq
#define Ppmd7_Update1 fe7z_Ppmd7_Update1
#define Ppmd7_Update1_0 fe7z_Ppmd7_Update1_0
#define Ppmd7_Update2 fe7z_Ppmd7_Update2
#define Ppmd7_UpdateModel fe7z_Ppmd7_UpdateModel
#define Ppmd7z_DecodeSymbol fe7z_Ppmd7z_DecodeSymbol
#define Ppmd7z_RangeDec_Init fe7z_Ppmd7z_RangeDec_Init
#define SecToLook_CreateVTable fe7z_SecToLook_CreateVTable
#define SecToRead_CreateVTable fe7z_SecToRead_CreateVTable
#define SeqInStream_ReadByte fe7z_SeqInStream_ReadByte
#define SeqInStream_ReadMax fe7z_SeqInStream_ReadMax
#define SzArEx_Extract fe7z_SzArEx_Extract
#define SzArEx_Free fe7z_SzArEx_Free
#define SzArEx_GetFileNameUtf16 fe7z_SzArEx_GetFileNameUtf16
#define SzArEx_Init fe7z_SzArEx_Init
#define SzArEx_Open fe7z_SzArEx_Open
#define SzAr_DecodeFolder fe7z_SzAr_DecodeFolder
#define SzAr_GetFolderUnpackSize fe7z_SzAr_GetFolderUnpackSize
#define SzGetNextFolderItem fe7z_SzGetNextFolderItem
#define g_CrcTable fe7z_g_CrcTable
#define k7zSignature fe7z_k7zSignature
#define z7_BranchConvSt_X86_Dec fe7z_z7_BranchConvSt_X86_Dec
#define z7_BranchConv_ARM64_Dec fe7z_z7_BranchConv_ARM64_Dec
#define z7_BranchConv_ARMT_Dec fe7z_z7_BranchConv_ARMT_Dec
#define z7_BranchConv_ARM_Dec fe7z_z7_BranchConv_ARM_Dec
#define z7_BranchConv_IA64_Dec fe7z_z7_BranchConv_IA64_Dec
#define z7_BranchConv_PPC_Dec fe7z_z7_BranchConv_PPC_Dec
#define z7_BranchConv_RISCV_Dec fe7z_z7_BranchConv_RISCV_Dec
#define z7_BranchConv_RISCV_Enc fe7z_z7_BranchConv_RISCV_Enc
#define z7_BranchConv_SPARC_Dec fe7z_z7_BranchConv_SPARC_Dec
#define z7_GetFunc_CrcUpdate fe7z_z7_GetFunc_CrcUpdate
#endif
