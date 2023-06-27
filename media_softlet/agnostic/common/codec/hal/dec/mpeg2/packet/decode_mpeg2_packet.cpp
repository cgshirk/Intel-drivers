/*
* Copyright (c) 2021-2022, Intel Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/
//!
//! \file     decode_mpeg2_packet.cpp
//! \brief    Defines the interface for mpeg2 decode packet.
//!

#include "decode_mpeg2_packet.h"
#include "decode_utils.h"
#include "decode_marker_packet.h"
#include "decode_status_report_defs.h"
#include "decode_predication_packet.h"
#include "codechal_debug.h"

namespace decode {

MOS_STATUS Mpeg2DecodePkt::Init()
{
    DECODE_FUNC_CALL();
    DECODE_CHK_NULL(m_miItf);
    DECODE_CHK_NULL(m_statusReport);
    DECODE_CHK_NULL(m_featureManager);
    DECODE_CHK_NULL(m_mpeg2Pipeline);
    DECODE_CHK_NULL(m_osInterface);

    DECODE_CHK_STATUS(CmdPacket::Init());

    m_mpeg2BasicFeature = dynamic_cast<Mpeg2BasicFeature*>(m_featureManager->GetFeature(FeatureIDs::basicFeature));
    DECODE_CHK_NULL(m_mpeg2BasicFeature);

    m_allocator = m_mpeg2Pipeline->GetDecodeAllocator();
    DECODE_CHK_NULL(m_allocator);

    DECODE_CHK_STATUS(m_statusReport->RegistObserver(this));

    DecodeSubPacket* subPacket = m_mpeg2Pipeline->GetSubPacket(DecodePacketId(m_mpeg2Pipeline, mpeg2PictureSubPacketId));
    m_picturePkt = dynamic_cast<Mpeg2DecodePicPkt*>(subPacket);
    DECODE_CHK_NULL(m_picturePkt);
    DECODE_CHK_STATUS(m_picturePkt->CalculateCommandSize(m_pictureStatesSize, m_picturePatchListSize));

    uint32_t secondLevelBBSize = 0;
    uint32_t numMacroblocks    = m_mpeg2BasicFeature->m_picWidthInMb * m_mpeg2BasicFeature->m_picHeightInMb;

    if (m_mpeg2BasicFeature->m_mode == CODECHAL_DECODE_MODE_MPEG2VLD)
    {
        subPacket = m_mpeg2Pipeline->GetSubPacket(DecodePacketId(m_mpeg2Pipeline, mpeg2SliceSubPacketId));
        m_slicePkt = dynamic_cast<Mpeg2DecodeSlcPkt*>(subPacket);
        DECODE_CHK_NULL(m_slicePkt);
        DECODE_CHK_STATUS(m_slicePkt->CalculateCommandSize(m_sliceStatesSize, m_slicePatchListSize));
        secondLevelBBSize = (m_sliceStatesSize * numMacroblocks) + m_hwInterface->m_sizeOfCmdBatchBufferEnd;
    }
    else
    {
        subPacket = m_mpeg2Pipeline->GetSubPacket(DecodePacketId(m_mpeg2Pipeline, mpeg2MbSubPacketId));
        m_mbPkt = dynamic_cast<Mpeg2DecodeMbPkt*>(subPacket);
        DECODE_CHK_NULL(m_mbPkt);
        DECODE_CHK_STATUS(m_mbPkt->CalculateCommandSize(m_mbStatesSize, m_mbPatchListSize));
        secondLevelBBSize = (m_mbStatesSize * numMacroblocks) + m_hwInterface->m_sizeOfCmdBatchBufferEnd;
    }

    m_secondLevelBBArray = m_allocator->AllocateBatchBufferArray(
        secondLevelBBSize, 1, CODEC_MPEG2_BATCH_BUFFERS_NUM, true, lockableVideoMem);
    DECODE_CHK_NULL(m_secondLevelBBArray);

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS Mpeg2DecodePkt::Prepare()
{
    DECODE_FUNC_CALL();

    DECODE_CHK_NULL(m_mpeg2BasicFeature->m_mpeg2PicParams);
    m_mpeg2PicParams = m_mpeg2BasicFeature->m_mpeg2PicParams;

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS Mpeg2DecodePkt::Destroy()
{
    m_statusReport->UnregistObserver(this);
    DECODE_CHK_STATUS(m_allocator->Destroy(m_secondLevelBBArray));

    return MOS_STATUS_SUCCESS;
}

void Mpeg2DecodePkt::SetPerfTag(CODECHAL_MODE mode, uint16_t picCodingType)
{
    DECODE_FUNC_CALL();

    uint16_t perfTag = ((mode << 4) & 0xF0) | (picCodingType & 0xF);
    m_osInterface->pfnIncPerfFrameID(m_osInterface);
    m_osInterface->pfnSetPerfTag(m_osInterface, perfTag);
    m_osInterface->pfnResetPerfBufferID(m_osInterface);
}

bool Mpeg2DecodePkt::IsPrologRequired()
{
    return true;
}

MOS_STATUS Mpeg2DecodePkt::AddForceWakeup(MOS_COMMAND_BUFFER& cmdBuffer)
{
    DECODE_FUNC_CALL();

    auto &par = m_miItf->GETPAR_MI_FORCE_WAKEUP();
    par       = {};
    par.bMFXPowerWellControl                    = true;
    par.bMFXPowerWellControlMask                = true;
    par.bHEVCPowerWellControl                   = false;
    par.bHEVCPowerWellControlMask               = true;

    DECODE_CHK_STATUS(m_miItf->ADDCMD_MI_FORCE_WAKEUP(&cmdBuffer));

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS Mpeg2DecodePkt::SendPrologWithFrameTracking(MOS_COMMAND_BUFFER& cmdBuffer, bool frameTrackingRequested)
{
    DECODE_FUNC_CALL();

    DecodeSubPacket* subPacket = m_mpeg2Pipeline->GetSubPacket(DecodePacketId(m_mpeg2Pipeline, markerSubPacketId));
    DecodeMarkerPkt *makerPacket = dynamic_cast<DecodeMarkerPkt*>(subPacket);
    DECODE_CHK_NULL(makerPacket);
    DECODE_CHK_STATUS(makerPacket->Execute(cmdBuffer));

#ifdef _MMC_SUPPORTED
    m_mmcState = m_mpeg2Pipeline->GetMmcState();
    bool isMmcEnabled = (m_mmcState != nullptr && m_mmcState->IsMmcEnabled());
    if (isMmcEnabled)
    {
        DECODE_CHK_STATUS(m_mmcState->SendPrologCmd(&cmdBuffer, false));
    }
#endif

    MHW_GENERIC_PROLOG_PARAMS  genericPrologParams;
    MOS_ZeroMemory(&genericPrologParams, sizeof(genericPrologParams));
    genericPrologParams.pOsInterface  = m_osInterface;
    genericPrologParams.pvMiInterface = nullptr;

#ifdef _MMC_SUPPORTED
    genericPrologParams.bMmcEnabled = isMmcEnabled;
#endif

    DECODE_CHK_STATUS(Mhw_SendGenericPrologCmdNext(&cmdBuffer, &genericPrologParams, m_miItf));

    subPacket = m_mpeg2Pipeline->GetSubPacket(DecodePacketId(m_mpeg2Pipeline, predicationSubPacketId));
    DecodePredicationPkt *predicationPacket = dynamic_cast<DecodePredicationPkt*>(subPacket);
    DECODE_CHK_NULL(predicationPacket);
    DECODE_CHK_STATUS(predicationPacket->Execute(cmdBuffer));

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS Mpeg2DecodePkt::MiFlush(MOS_COMMAND_BUFFER & cmdBuffer)
{
    DECODE_FUNC_CALL();

    auto &par = m_miItf->GETPAR_MI_FLUSH_DW();
    par       = {};
    DECODE_CHK_STATUS(m_miItf->ADDCMD_MI_FLUSH_DW(&cmdBuffer));

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS Mpeg2DecodePkt::Completed(void *mfxStatus, void *rcsStatus, void *statusReport)
{
    DECODE_FUNC_CALL();

    DecodeStatusMfx *decodeStatusMfx  = (DecodeStatusMfx *)mfxStatus;
    DecodeStatusReportData *statusReportData = (DecodeStatusReportData *)statusReport;
    auto                    mfxItf           = std::static_pointer_cast<mhw::vdbox::mfx::Itf>(m_hwInterface->GetMfxInterfaceNext());

    if (mfxItf && decodeStatusMfx && statusReportData)
    {
        if ((decodeStatusMfx->m_mmioErrorStatusReg & mfxItf->GetMfxErrorFlagsMask()) != 0)
        {
            statusReportData->codecStatus = CODECHAL_STATUS_ERROR;
            statusReportData->numMbsAffected = decodeStatusMfx->m_mmioMBCountReg & 0xFFFF;
        }
        DECODE_VERBOSEMESSAGE("Current Frame Index = %d", statusReportData->currDecodedPic.FrameIdx);
    }

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS Mpeg2DecodePkt::CalculateCommandSize(uint32_t &commandBufferSize, uint32_t &requestedPatchListSize)
{
    commandBufferSize = CalculateCommandBufferSize();
    requestedPatchListSize = CalculatePatchListSize();

    return MOS_STATUS_SUCCESS;
}

uint32_t Mpeg2DecodePkt::CalculateCommandBufferSize()
{
    // slice/macroblock level commands are put into 2nd level BB.
    return (m_pictureStatesSize + COMMAND_BUFFER_RESERVED_SPACE);
}

uint32_t Mpeg2DecodePkt::CalculatePatchListSize()
{
    if (!m_osInterface->bUsesPatchList)
    {
        return 0;
    }

    return m_picturePatchListSize;
}

MOS_STATUS Mpeg2DecodePkt::ReadMfxStatus(MediaStatusReport* statusReport, MOS_COMMAND_BUFFER& cmdBuffer)
{
    DECODE_FUNC_CALL();

    DECODE_CHK_NULL(statusReport);

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;
    MOS_RESOURCE* osResource = nullptr;
    uint32_t     offset = 0;

    auto &par = m_miItf->GETPAR_MI_STORE_REGISTER_MEM();
    par       = {};

    DECODE_CHK_NULL(m_hwInterface->GetVdencInterfaceNext());
    auto mmioRegisters = m_hwInterface->GetVdencInterfaceNext()->GetMmioRegisters(MHW_VDBOX_NODE_1);

    DECODE_CHK_STATUS(statusReport->GetAddress(decode::DecodeStatusReportType::DecErrorStatusOffset, osResource, offset));
    par.presStoreBuffer = osResource;
    par.dwOffset        = offset;
    par.dwRegister      = mmioRegisters->mfxErrorFlagsRegOffset;

    DECODE_CHK_STATUS(m_miItf->ADDCMD_MI_STORE_REGISTER_MEM(&cmdBuffer));

    DECODE_CHK_STATUS(statusReport->GetAddress(decode::DecodeStatusReportType::DecMBCountOffset, osResource, offset));
    par.presStoreBuffer = osResource;
    par.dwOffset        = offset;
    par.dwRegister      = mmioRegisters->mfxMBCountRegOffset;

    DECODE_CHK_STATUS(m_miItf->ADDCMD_MI_STORE_REGISTER_MEM(&cmdBuffer));

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS Mpeg2DecodePkt::StartStatusReport(uint32_t srType, MOS_COMMAND_BUFFER* cmdBuffer)
{
    DECODE_FUNC_CALL();

    DECODE_CHK_STATUS(MediaPacket::StartStatusReportNext(srType, cmdBuffer));

    SetPerfTag(m_mpeg2BasicFeature->m_mode, m_mpeg2BasicFeature->m_pictureCodingType);

    MediaPerfProfiler *perfProfiler = MediaPerfProfiler::Instance();
    DECODE_CHK_NULL(perfProfiler);
    DECODE_CHK_STATUS(perfProfiler->AddPerfCollectStartCmd(
        (void*)m_mpeg2Pipeline, m_osInterface, m_miItf, cmdBuffer));

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS Mpeg2DecodePkt::EndStatusReport(uint32_t srType, MOS_COMMAND_BUFFER* cmdBuffer)
{
    DECODE_FUNC_CALL();

    DECODE_CHK_NULL(cmdBuffer);
    DECODE_CHK_STATUS(ReadMfxStatus(m_statusReport, *cmdBuffer));
    DECODE_CHK_STATUS(MediaPacket::EndStatusReportNext(srType, cmdBuffer));

    MediaPerfProfiler *perfProfiler = MediaPerfProfiler::Instance();
    DECODE_CHK_NULL(perfProfiler);
    DECODE_CHK_STATUS(perfProfiler->AddPerfCollectEndCmd(
        (void*)m_mpeg2Pipeline, m_osInterface, m_miItf, cmdBuffer));

   DECODE_CHK_STATUS(MiFlush(*cmdBuffer));

    return MOS_STATUS_SUCCESS;
}

}
