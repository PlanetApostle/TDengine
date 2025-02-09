/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tq.h"

int32_t tqInit() {
  int8_t old;
  while (1) {
    old = atomic_val_compare_exchange_8(&tqMgmt.inited, 0, 2);
    if (old != 2) break;
  }

  if (old == 0) {
    tqMgmt.timer = taosTmrInit(10000, 100, 10000, "TQ");
    if (tqMgmt.timer == NULL) {
      atomic_store_8(&tqMgmt.inited, 0);
      return -1;
    }
    if (streamInit() < 0) {
      return -1;
    }
    atomic_store_8(&tqMgmt.inited, 1);
  }

  return 0;
}

void tqCleanUp() {
  int8_t old;
  while (1) {
    old = atomic_val_compare_exchange_8(&tqMgmt.inited, 1, 2);
    if (old != 2) break;
  }

  if (old == 1) {
    taosTmrCleanUp(tqMgmt.timer);
    streamCleanUp();
    atomic_store_8(&tqMgmt.inited, 0);
  }
}

static void destroySTqHandle(void* data) {
  STqHandle* pData = (STqHandle*)data;
  qDestroyTask(pData->execHandle.task);
  if (pData->execHandle.subType == TOPIC_SUB_TYPE__COLUMN) {
    taosMemoryFreeClear(pData->execHandle.execCol.qmsg);
  } else if (pData->execHandle.subType == TOPIC_SUB_TYPE__DB) {
    tqCloseReader(pData->execHandle.pExecReader);
    walCloseReader(pData->pWalReader);
    taosHashCleanup(pData->execHandle.execDb.pFilterOutTbUid);
  } else if (pData->execHandle.subType == TOPIC_SUB_TYPE__TABLE) {
    walCloseReader(pData->pWalReader);
    tqCloseReader(pData->execHandle.pExecReader);
  }
}

static void tqPushEntryFree(void* data) {
  STqPushEntry* p = *(void**)data;
  tDeleteSMqDataRsp(&p->dataRsp);
  taosMemoryFree(p);
}

STQ* tqOpen(const char* path, SVnode* pVnode) {
  STQ* pTq = taosMemoryCalloc(1, sizeof(STQ));
  if (pTq == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }
  pTq->path = taosStrdup(path);
  pTq->pVnode = pVnode;
  pTq->walLogLastVer = pVnode->pWal->vers.lastVer;

  pTq->pHandle = taosHashInit(64, MurmurHash3_32, true, HASH_ENTRY_LOCK);
  taosHashSetFreeFp(pTq->pHandle, destroySTqHandle);

  taosInitRWLatch(&pTq->pushLock);
  pTq->pPushMgr = taosHashInit(64, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), true, HASH_NO_LOCK);
  taosHashSetFreeFp(pTq->pPushMgr, tqPushEntryFree);

  pTq->pCheckInfo = taosHashInit(64, MurmurHash3_32, true, HASH_ENTRY_LOCK);
  taosHashSetFreeFp(pTq->pCheckInfo, (FDelete)tDeleteSTqCheckInfo);

  if (tqMetaOpen(pTq) < 0) {
    return NULL;
  }

  pTq->pOffsetStore = tqOffsetOpen(pTq);
  if (pTq->pOffsetStore == NULL) {
    return NULL;
  }

  pTq->pStreamMeta = streamMetaOpen(path, pTq, (FTaskExpand*)tqExpandTask, pTq->pVnode->config.vgId);
  if (pTq->pStreamMeta == NULL) {
    return NULL;
  }

  if (streamLoadTasks(pTq->pStreamMeta, walGetCommittedVer(pVnode->pWal)) < 0) {
    return NULL;
  }

  return pTq;
}

void tqClose(STQ* pTq) {
  if (pTq == NULL) {
    return;
  }

  tqOffsetClose(pTq->pOffsetStore);
  taosHashCleanup(pTq->pHandle);
  taosHashCleanup(pTq->pPushMgr);
  taosHashCleanup(pTq->pCheckInfo);
  taosMemoryFree(pTq->path);
  tqMetaClose(pTq);
  streamMetaClose(pTq->pStreamMeta);
  taosMemoryFree(pTq);
}

int32_t tqSendMetaPollRsp(STQ* pTq, const SRpcMsg* pMsg, const SMqPollReq* pReq, const SMqMetaRsp* pRsp) {
  int32_t len = 0;
  int32_t code = 0;
  tEncodeSize(tEncodeSMqMetaRsp, pRsp, len, code);
  if (code < 0) {
    return -1;
  }
  int32_t tlen = sizeof(SMqRspHead) + len;
  void*   buf = rpcMallocCont(tlen);
  if (buf == NULL) {
    return -1;
  }

  ((SMqRspHead*)buf)->mqMsgType = TMQ_MSG_TYPE__POLL_META_RSP;
  ((SMqRspHead*)buf)->epoch = pReq->epoch;
  ((SMqRspHead*)buf)->consumerId = pReq->consumerId;

  void* abuf = POINTER_SHIFT(buf, sizeof(SMqRspHead));

  SEncoder encoder = {0};
  tEncoderInit(&encoder, abuf, len);
  tEncodeSMqMetaRsp(&encoder, pRsp);
  tEncoderClear(&encoder);

  SRpcMsg resp = {
      .info = pMsg->info,
      .pCont = buf,
      .contLen = tlen,
      .code = 0,
  };
  tmsgSendRsp(&resp);

  tqDebug("vgId:%d, from consumer:0x%" PRIx64 " (epoch %d) send rsp, res msg type %d, offset type:%d",
          TD_VID(pTq->pVnode), pReq->consumerId, pReq->epoch, pRsp->resMsgType, pRsp->rspOffset.type);

  return 0;
}

int32_t tqPushDataRsp(STQ* pTq, STqPushEntry* pPushEntry) {
  SMqDataRsp* pRsp = &pPushEntry->dataRsp;

#if 0
  A(taosArrayGetSize(pRsp->blockData) == pRsp->blockNum);
  A(taosArrayGetSize(pRsp->blockDataLen) == pRsp->blockNum);

  A(!pRsp->withSchema);
  A(taosArrayGetSize(pRsp->blockSchema) == 0);

  if (pRsp->reqOffset.type == TMQ_OFFSET__LOG) {
    A(pRsp->rspOffset.version > pRsp->reqOffset.version);
  }
#endif

  int32_t len = 0;
  int32_t code = 0;
  tEncodeSize(tEncodeSMqDataRsp, pRsp, len, code);

  if (code < 0) {
    return -1;
  }

  int32_t tlen = sizeof(SMqRspHead) + len;
  void*   buf = rpcMallocCont(tlen);
  if (buf == NULL) {
    return -1;
  }

  memcpy(buf, &pPushEntry->dataRsp.head, sizeof(SMqRspHead));

  void* abuf = POINTER_SHIFT(buf, sizeof(SMqRspHead));

  SEncoder encoder = {0};
  tEncoderInit(&encoder, abuf, len);
  tEncodeSMqDataRsp(&encoder, pRsp);
  tEncoderClear(&encoder);

  SRpcMsg rsp = {
      .info = pPushEntry->pInfo,
      .pCont = buf,
      .contLen = tlen,
      .code = 0,
  };

  tmsgSendRsp(&rsp);

  char buf1[80] = {0};
  char buf2[80] = {0};
  tFormatOffset(buf1, tListLen(buf1), &pRsp->reqOffset);
  tFormatOffset(buf2, tListLen(buf2), &pRsp->rspOffset);
  tqDebug("vgId:%d, from consumer:0x%" PRIx64 " (epoch %d) push rsp, block num: %d, req:%s, rsp:%s",
          TD_VID(pTq->pVnode), pRsp->head.consumerId, pRsp->head.epoch, pRsp->blockNum, buf1, buf2);

  return 0;
}

int32_t tqSendDataRsp(STQ* pTq, const SRpcMsg* pMsg, const SMqPollReq* pReq, const SMqDataRsp* pRsp) {
#if 0
  A(taosArrayGetSize(pRsp->blockData) == pRsp->blockNum);
  A(taosArrayGetSize(pRsp->blockDataLen) == pRsp->blockNum);

  A(!pRsp->withSchema);
  A(taosArrayGetSize(pRsp->blockSchema) == 0);

  if (pRsp->reqOffset.type == TMQ_OFFSET__LOG) {
    if (pRsp->blockNum > 0) {
      A(pRsp->rspOffset.version > pRsp->reqOffset.version);
    } else {
      A(pRsp->rspOffset.version >= pRsp->reqOffset.version);
    }
  }
#endif

  int32_t len = 0;
  int32_t code = 0;
  tEncodeSize(tEncodeSMqDataRsp, pRsp, len, code);
  if (code < 0) {
    return -1;
  }
  int32_t tlen = sizeof(SMqRspHead) + len;
  void*   buf = rpcMallocCont(tlen);
  if (buf == NULL) {
    return -1;
  }

  ((SMqRspHead*)buf)->mqMsgType = TMQ_MSG_TYPE__POLL_RSP;
  ((SMqRspHead*)buf)->epoch = pReq->epoch;
  ((SMqRspHead*)buf)->consumerId = pReq->consumerId;

  void* abuf = POINTER_SHIFT(buf, sizeof(SMqRspHead));

  SEncoder encoder = {0};
  tEncoderInit(&encoder, abuf, len);
  tEncodeSMqDataRsp(&encoder, pRsp);
  tEncoderClear(&encoder);

  SRpcMsg rsp = {
      .info = pMsg->info,
      .pCont = buf,
      .contLen = tlen,
      .code = 0,
  };
  tmsgSendRsp(&rsp);

  char buf1[80] = {0};
  char buf2[80] = {0};
  tFormatOffset(buf1, 80, &pRsp->reqOffset);
  tFormatOffset(buf2, 80, &pRsp->rspOffset);
  tqDebug("vgId:%d consumer:0x%" PRIx64 " (epoch %d), block num:%d, req:%s, rsp:%s",
          TD_VID(pTq->pVnode), pReq->consumerId, pReq->epoch, pRsp->blockNum, buf1, buf2);

  return 0;
}

int32_t tqSendTaosxRsp(STQ* pTq, const SRpcMsg* pMsg, const SMqPollReq* pReq, const STaosxRsp* pRsp) {
#if 0
  A(taosArrayGetSize(pRsp->blockData) == pRsp->blockNum);
  A(taosArrayGetSize(pRsp->blockDataLen) == pRsp->blockNum);

  if (pRsp->withSchema) {
    A(taosArrayGetSize(pRsp->blockSchema) == pRsp->blockNum);
  } else {
    A(taosArrayGetSize(pRsp->blockSchema) == 0);
  }

  if (pRsp->reqOffset.type == TMQ_OFFSET__LOG) {
    if (pRsp->blockNum > 0) {
      A(pRsp->rspOffset.version > pRsp->reqOffset.version);
    } else {
      A(pRsp->rspOffset.version >= pRsp->reqOffset.version);
    }
  }
#endif

  int32_t len = 0;
  int32_t code = 0;
  tEncodeSize(tEncodeSTaosxRsp, pRsp, len, code);
  if (code < 0) {
    return -1;
  }
  int32_t tlen = sizeof(SMqRspHead) + len;
  void*   buf = rpcMallocCont(tlen);
  if (buf == NULL) {
    return -1;
  }

  ((SMqRspHead*)buf)->mqMsgType = TMQ_MSG_TYPE__TAOSX_RSP;
  ((SMqRspHead*)buf)->epoch = pReq->epoch;
  ((SMqRspHead*)buf)->consumerId = pReq->consumerId;

  void* abuf = POINTER_SHIFT(buf, sizeof(SMqRspHead));

  SEncoder encoder = {0};
  tEncoderInit(&encoder, abuf, len);
  tEncodeSTaosxRsp(&encoder, pRsp);
  tEncoderClear(&encoder);

  SRpcMsg rsp = {
      .info = pMsg->info,
      .pCont = buf,
      .contLen = tlen,
      .code = 0,
  };
  tmsgSendRsp(&rsp);

  char buf1[80] = {0};
  char buf2[80] = {0};
  tFormatOffset(buf1, 80, &pRsp->reqOffset);
  tFormatOffset(buf2, 80, &pRsp->rspOffset);
  tqDebug("taosx rsp, vgId:%d, from consumer:0x%" PRIx64 " (epoch %d) send rsp, numOfBlks:%d, req:%s, rsp:%s",
          TD_VID(pTq->pVnode), pReq->consumerId, pReq->epoch, pRsp->blockNum, buf1, buf2);

  return 0;
}

static FORCE_INLINE bool tqOffsetLessOrEqual(const STqOffset* pLeft, const STqOffset* pRight) {
  return pLeft->val.type == TMQ_OFFSET__LOG && pRight->val.type == TMQ_OFFSET__LOG &&
         pLeft->val.version <= pRight->val.version;
}

int32_t tqProcessOffsetCommitReq(STQ* pTq, int64_t sversion, char* msg, int32_t msgLen) {
  STqOffset offset = {0};

  SDecoder decoder;
  tDecoderInit(&decoder, (uint8_t*)msg, msgLen);
  if (tDecodeSTqOffset(&decoder, &offset) < 0) {
    return -1;
  }

  tDecoderClear(&decoder);

  if (offset.val.type == TMQ_OFFSET__SNAPSHOT_DATA || offset.val.type == TMQ_OFFSET__SNAPSHOT_META) {
    tqDebug("receive offset commit msg to %s on vgId:%d, offset(type:snapshot) uid:%" PRId64 ", ts:%" PRId64,
            offset.subKey, TD_VID(pTq->pVnode), offset.val.uid, offset.val.ts);
  } else if (offset.val.type == TMQ_OFFSET__LOG) {
    tqDebug("receive offset commit msg to %s on vgId:%d, offset(type:log) version:%" PRId64, offset.subKey,
            TD_VID(pTq->pVnode), offset.val.version);
    if (offset.val.version + 1 == sversion) {
      offset.val.version += 1;
    }
  } else {
    tqError("invalid commit offset type:%d", offset.val.type);
    return -1;
  }

  STqOffset* pSavedOffset = tqOffsetRead(pTq->pOffsetStore, offset.subKey);
  if (pSavedOffset != NULL && tqOffsetLessOrEqual(&offset, pSavedOffset)) {
    return 0;  // no need to update the offset value
  }

  // save the new offset value
  if (tqOffsetWrite(pTq->pOffsetStore, &offset) < 0) {
    return -1;
  }

  if (offset.val.type == TMQ_OFFSET__LOG) {
    STqHandle* pHandle = taosHashGet(pTq->pHandle, offset.subKey, strlen(offset.subKey));
    if (pHandle && (walRefVer(pHandle->pRef, offset.val.version) < 0)) {
      return -1;
    }
  }

  return 0;
}

int32_t tqCheckColModifiable(STQ* pTq, int64_t tbUid, int32_t colId) {
  void* pIter = NULL;

  while (1) {
    pIter = taosHashIterate(pTq->pCheckInfo, pIter);
    if (pIter == NULL) {
      break;
    }

    STqCheckInfo* pCheck = (STqCheckInfo*)pIter;

    if (pCheck->ntbUid == tbUid) {
      int32_t sz = taosArrayGetSize(pCheck->colIdList);
      for (int32_t i = 0; i < sz; i++) {
        int16_t forbidColId = *(int16_t*)taosArrayGet(pCheck->colIdList, i);
        if (forbidColId == colId) {
          taosHashCancelIterate(pTq->pCheckInfo, pIter);
          return -1;
        }
      }
    }
  }

  return 0;
}

static int32_t tqInitDataRsp(SMqDataRsp* pRsp, const SMqPollReq* pReq, int8_t subType) {
  pRsp->reqOffset = pReq->reqOffset;

  pRsp->blockData = taosArrayInit(0, sizeof(void*));
  pRsp->blockDataLen = taosArrayInit(0, sizeof(int32_t));

  if (pRsp->blockData == NULL || pRsp->blockDataLen == NULL) {
    return -1;
  }

  pRsp->withTbName = 0;
#if 0
  pRsp->withTbName = pReq->withTbName;
  if (pRsp->withTbName) {
    pRsp->blockTbName = taosArrayInit(0, sizeof(void*));
    if (pRsp->blockTbName == NULL) {
      // TODO free
      return -1;
    }
  }
#endif

  /*A(subType == TOPIC_SUB_TYPE__COLUMN);*/
  pRsp->withSchema = false;

  return 0;
}

static int32_t tqInitTaosxRsp(STaosxRsp* pRsp, const SMqPollReq* pReq) {
  pRsp->reqOffset = pReq->reqOffset;

  pRsp->withTbName = 1;
  pRsp->withSchema = 1;
  pRsp->blockData = taosArrayInit(0, sizeof(void*));
  pRsp->blockDataLen = taosArrayInit(0, sizeof(int32_t));
  pRsp->blockTbName = taosArrayInit(0, sizeof(void*));
  pRsp->blockSchema = taosArrayInit(0, sizeof(void*));

  if (pRsp->blockData == NULL || pRsp->blockDataLen == NULL || pRsp->blockTbName == NULL || pRsp->blockSchema == NULL) {
    return -1;
  }

  return 0;
}

int32_t tqProcessPollReq(STQ* pTq, SRpcMsg* pMsg) {
  SMqPollReq   req = {0};
  int32_t      code = 0;
  STqOffsetVal fetchOffsetNew;
  SWalCkHead*  pCkHead = NULL;

  if (tDeserializeSMqPollReq(pMsg->pCont, pMsg->contLen, &req) < 0) {
    tqError("tDeserializeSMqPollReq %d failed", pMsg->contLen);
    return -1;
  }

  int64_t      consumerId = req.consumerId;
  int32_t      reqEpoch = req.epoch;
  STqOffsetVal reqOffset = req.reqOffset;

  // 1. find handle
  STqHandle* pHandle = taosHashGet(pTq->pHandle, req.subKey, strlen(req.subKey));
  if (pHandle == NULL) {
    tqError("tmq poll: consumer:0x%" PRIx64 " vgId:%d, subkey %s not found", consumerId, TD_VID(pTq->pVnode),
            req.subKey);
    return -1;
  }

  // 2. check rebalance
  if (pHandle->consumerId != consumerId) {
    tqError("tmq poll: consumer:0x%" PRIx64 " vgId:%d, subkey %s, mismatch for saved handle consumer:0x%" PRIx64,
            consumerId, TD_VID(pTq->pVnode), req.subKey, pHandle->consumerId);
    terrno = TSDB_CODE_TMQ_CONSUMER_MISMATCH;
    return -1;
  }

  // update epoch if need
  int32_t savedEpoch = atomic_load_32(&pHandle->epoch);
  while (savedEpoch < reqEpoch) {
    tqDebug("tmq poll: consumer:0x%"PRIx64 " epoch update from %d to %d by poll req", consumerId, savedEpoch, reqEpoch);
    savedEpoch = atomic_val_compare_exchange_32(&pHandle->epoch, savedEpoch, reqEpoch);
  }

  char buf[80];
  tFormatOffset(buf, 80, &reqOffset);
  tqDebug("tmq poll: consumer:0x%" PRIx64 " (epoch %d), subkey %s, recv poll req vgId:%d, req:%s", consumerId,
          req.epoch, pHandle->subKey, TD_VID(pTq->pVnode), buf);

  // 2.reset offset if needed
  if (reqOffset.type > 0) {
    fetchOffsetNew = reqOffset;
  } else {
    STqOffset* pOffset = tqOffsetRead(pTq->pOffsetStore, req.subKey);
    if (pOffset != NULL) {
      fetchOffsetNew = pOffset->val;
      char formatBuf[80];
      tFormatOffset(formatBuf, 80, &fetchOffsetNew);
      tqDebug("tmq poll: consumer:0x%" PRIx64 ", subkey %s, vg %d, offset reset to %s", consumerId, pHandle->subKey,
              TD_VID(pTq->pVnode), formatBuf);
    } else {
      if (reqOffset.type == TMQ_OFFSET__RESET_EARLIEAST) {
        if (req.useSnapshot) {
          if (pHandle->fetchMeta) {
            tqOffsetResetToMeta(&fetchOffsetNew, 0);
          } else {
            tqOffsetResetToData(&fetchOffsetNew, 0, 0);
          }
        } else {
          pHandle->pRef = walRefFirstVer(pTq->pVnode->pWal, pHandle->pRef);
          if (pHandle->pRef == NULL) {
            terrno = TSDB_CODE_OUT_OF_MEMORY;
            return -1;
          }
          tqOffsetResetToLog(&fetchOffsetNew, pHandle->pRef->refVer - 1);
        }
      } else if (reqOffset.type == TMQ_OFFSET__RESET_LATEST) {
        if (pHandle->execHandle.subType == TOPIC_SUB_TYPE__COLUMN) {
          SMqDataRsp dataRsp = {0};
          tqInitDataRsp(&dataRsp, &req, pHandle->execHandle.subType);

          tqOffsetResetToLog(&dataRsp.rspOffset, walGetLastVer(pTq->pVnode->pWal));
          tqDebug("tmq poll: consumer:0x%" PRIx64 ", subkey %s, vgId:%d, offset reset to %" PRId64, consumerId,
                  pHandle->subKey, TD_VID(pTq->pVnode), dataRsp.rspOffset.version);
          if (tqSendDataRsp(pTq, pMsg, &req, &dataRsp) < 0) {
            code = -1;
          }
          tDeleteSMqDataRsp(&dataRsp);
          return code;
        } else {
          STaosxRsp taosxRsp = {0};
          tqInitTaosxRsp(&taosxRsp, &req);
          tqOffsetResetToLog(&taosxRsp.rspOffset, walGetLastVer(pTq->pVnode->pWal));
          if (tqSendTaosxRsp(pTq, pMsg, &req, &taosxRsp) < 0) {
            code = -1;
          }
          tDeleteSTaosxRsp(&taosxRsp);
          return code;
        }
      } else if (reqOffset.type == TMQ_OFFSET__RESET_NONE) {
        tqError("tmq poll: subkey %s, no offset committed for consumer:0x%" PRIx64
                " in vg %d, subkey %s, reset none failed",
                pHandle->subKey, consumerId, TD_VID(pTq->pVnode), req.subKey);
        terrno = TSDB_CODE_TQ_NO_COMMITTED_OFFSET;
        return -1;
      }
    }
  }

  if (pHandle->execHandle.subType == TOPIC_SUB_TYPE__COLUMN) {
    SMqDataRsp dataRsp = {0};
    tqInitDataRsp(&dataRsp, &req, pHandle->execHandle.subType);
    // lock
    taosWLockLatch(&pTq->pushLock);
    if (tqScanData(pTq, pHandle, &dataRsp, &fetchOffsetNew) < 0) {
      return -1;
    }

    // till now, all data has been rsp to consumer, new data needs to push client once arrived.
    if (dataRsp.blockNum == 0 && dataRsp.reqOffset.type == TMQ_OFFSET__LOG &&
        dataRsp.reqOffset.version == dataRsp.rspOffset.version) {
      STqPushEntry* pPushEntry = taosMemoryCalloc(1, sizeof(STqPushEntry));
      if (pPushEntry != NULL) {
        pPushEntry->pInfo = pMsg->info;
        memcpy(pPushEntry->subKey, pHandle->subKey, TSDB_SUBSCRIBE_KEY_LEN);
        dataRsp.withTbName = 0;
        memcpy(&pPushEntry->dataRsp, &dataRsp, sizeof(SMqDataRsp));
        pPushEntry->dataRsp.head.consumerId = consumerId;
        pPushEntry->dataRsp.head.epoch = reqEpoch;
        pPushEntry->dataRsp.head.mqMsgType = TMQ_MSG_TYPE__POLL_RSP;
        taosHashPut(pTq->pPushMgr, pHandle->subKey, strlen(pHandle->subKey) + 1, &pPushEntry, sizeof(void*));

        tqDebug("tmq poll: consumer:0x%" PRIx64 ", subkey %s offset:%" PRId64 ", vgId:%d save handle to push mgr",
                consumerId, pHandle->subKey, dataRsp.reqOffset.version, TD_VID(pTq->pVnode));
        // unlock
        taosWUnLockLatch(&pTq->pushLock);
        return 0;
      }
    }
    taosWUnLockLatch(&pTq->pushLock);

    if (tqSendDataRsp(pTq, pMsg, &req, &dataRsp) < 0) {
      code = -1;
    }

    tqDebug("tmq poll: consumer:0x%" PRIx64 ", subkey %s, vgId:%d, rsp block:%d, offset type:%d, uid/version:%" PRId64 ", ts:%" PRId64 "",
            consumerId, pHandle->subKey, TD_VID(pTq->pVnode), dataRsp.blockNum, dataRsp.rspOffset.type,
            dataRsp.rspOffset.uid, dataRsp.rspOffset.ts);

    tDeleteSMqDataRsp(&dataRsp);
    return code;
  }

  // for taosx
  SMqMetaRsp metaRsp = {0};
  STaosxRsp taosxRsp = {0};
  tqInitTaosxRsp(&taosxRsp, &req);

  if (fetchOffsetNew.type != TMQ_OFFSET__LOG) {
    if (tqScanTaosx(pTq, pHandle, &taosxRsp, &metaRsp, &fetchOffsetNew) < 0) {
      return -1;
    }

    if (metaRsp.metaRspLen > 0) {
      if (tqSendMetaPollRsp(pTq, pMsg, &req, &metaRsp) < 0) {
        code = -1;
      }
      tqDebug("tmq poll: consumer:0x%" PRIx64 " subkey %s, vg %d, send meta offset type:%d,uid:%" PRId64
              ",version:%" PRId64,
              consumerId, pHandle->subKey, TD_VID(pTq->pVnode), metaRsp.rspOffset.type, metaRsp.rspOffset.uid,
              metaRsp.rspOffset.version);
      taosMemoryFree(metaRsp.metaRsp);
      tDeleteSTaosxRsp(&taosxRsp);
      return code;
    }

    if (taosxRsp.blockNum > 0) {
      if (tqSendTaosxRsp(pTq, pMsg, &req, &taosxRsp) < 0) {
        code = -1;
      }
      tDeleteSTaosxRsp(&taosxRsp);
      return code;
    } else {
      fetchOffsetNew = taosxRsp.rspOffset;
    }

    tqDebug("taosx poll: consumer:0x%" PRIx64 " subkey %s, vg %d, send data blockNum:%d, offset type:%d,uid:%" PRId64
            ",version:%" PRId64,
            consumerId, pHandle->subKey, TD_VID(pTq->pVnode), taosxRsp.blockNum, taosxRsp.rspOffset.type,
            taosxRsp.rspOffset.uid, taosxRsp.rspOffset.version);
  }

  if (fetchOffsetNew.type == TMQ_OFFSET__LOG) {
    int64_t fetchVer = fetchOffsetNew.version + 1;
    pCkHead = taosMemoryMalloc(sizeof(SWalCkHead) + 2048);
    if (pCkHead == NULL) {
      tDeleteSTaosxRsp(&taosxRsp);
      return -1;
    }

    walSetReaderCapacity(pHandle->pWalReader, 2048);

    while (1) {
      savedEpoch = atomic_load_32(&pHandle->epoch);
      if (savedEpoch > reqEpoch) {
        tqWarn("tmq poll: consumer:0x%" PRIx64 " (epoch %d), subkey %s, vg %d offset %" PRId64
               ", found new consumer epoch %d, discard req epoch %d",
               consumerId, req.epoch, pHandle->subKey, TD_VID(pTq->pVnode), fetchVer, savedEpoch, reqEpoch);
        break;
      }

      if (tqFetchLog(pTq, pHandle, &fetchVer, &pCkHead) < 0) {
        tqOffsetResetToLog(&taosxRsp.rspOffset, fetchVer);
        if (tqSendTaosxRsp(pTq, pMsg, &req, &taosxRsp) < 0) {
          code = -1;
        }
        tDeleteSTaosxRsp(&taosxRsp);
        taosMemoryFreeClear(pCkHead);
        return code;
      }

      SWalCont* pHead = &pCkHead->head;

      tqDebug("tmq poll: consumer:0x%" PRIx64 " (epoch %d) iter log, vgId:%d offset %" PRId64 " msgType %d", consumerId,
              req.epoch, TD_VID(pTq->pVnode), fetchVer, pHead->msgType);

      if (pHead->msgType == TDMT_VND_SUBMIT) {
        SPackedData submit = {
            .msgStr = POINTER_SHIFT(pHead->body, sizeof(SSubmitReq2Msg)),
            .msgLen = pHead->bodyLen - sizeof(SSubmitReq2Msg),
            .ver = pHead->version,
        };
        if (tqTaosxScanLog(pTq, pHandle, submit, &taosxRsp) < 0) {
          tqError("tmq poll: tqTaosxScanLog error %" PRId64 ", in vgId:%d, subkey %s", consumerId, TD_VID(pTq->pVnode),
                  req.subKey);
          return -1;
        }
        if (taosxRsp.blockNum > 0 /* threshold */) {
          tqOffsetResetToLog(&taosxRsp.rspOffset, fetchVer);
          if (tqSendTaosxRsp(pTq, pMsg, &req, &taosxRsp) < 0) {
            code = -1;
          }
          tDeleteSTaosxRsp(&taosxRsp);
          taosMemoryFreeClear(pCkHead);
          return code;
        } else {
          fetchVer++;
        }

      } else {
        /*A(pHandle->fetchMeta);*/
        /*A(IS_META_MSG(pHead->msgType));*/
        tqDebug("fetch meta msg, ver:%" PRId64 ", type:%s", pHead->version, TMSG_INFO(pHead->msgType));
        tqOffsetResetToLog(&metaRsp.rspOffset, fetchVer);
        metaRsp.resMsgType = pHead->msgType;
        metaRsp.metaRspLen = pHead->bodyLen;
        metaRsp.metaRsp = pHead->body;
        if (tqSendMetaPollRsp(pTq, pMsg, &req, &metaRsp) < 0) {
          code = -1;
          taosMemoryFreeClear(pCkHead);
          tDeleteSTaosxRsp(&taosxRsp);
          return code;
        }
        code = 0;
        taosMemoryFreeClear(pCkHead);
        tDeleteSTaosxRsp(&taosxRsp);
        return code;
      }
    }
  }

  tDeleteSTaosxRsp(&taosxRsp);
  taosMemoryFreeClear(pCkHead);
  return 0;
}

int32_t tqProcessDeleteSubReq(STQ* pTq, int64_t sversion, char* msg, int32_t msgLen) {
  SMqVDeleteReq* pReq = (SMqVDeleteReq*)msg;

  tqDebug("vgId:%d, tq process delete sub req %s", pTq->pVnode->config.vgId, pReq->subKey);

  taosWLockLatch(&pTq->pushLock);
  int32_t code = taosHashRemove(pTq->pPushMgr, pReq->subKey, strlen(pReq->subKey));
  if (code != 0) {
    tqDebug("vgId:%d, tq remove push handle %s", pTq->pVnode->config.vgId, pReq->subKey);
  }
  taosWUnLockLatch(&pTq->pushLock);

  STqHandle* pHandle = taosHashGet(pTq->pHandle, pReq->subKey, strlen(pReq->subKey));
  if (pHandle) {
    // walCloseRef(pHandle->pWalReader->pWal, pHandle->pRef->refId);
    if (pHandle->pRef) {
      walCloseRef(pTq->pVnode->pWal, pHandle->pRef->refId);
    }
    code = taosHashRemove(pTq->pHandle, pReq->subKey, strlen(pReq->subKey));
    if (code != 0) {
      tqError("cannot process tq delete req %s, since no such handle", pReq->subKey);
    }
  }

  code = tqOffsetDelete(pTq->pOffsetStore, pReq->subKey);
  if (code != 0) {
    tqError("cannot process tq delete req %s, since no such offset in cache", pReq->subKey);
  }

  if (tqMetaDeleteHandle(pTq, pReq->subKey) < 0) {
    tqError("cannot process tq delete req %s, since no such offset in tdb", pReq->subKey);
  }
  return 0;
}

int32_t tqProcessAddCheckInfoReq(STQ* pTq, int64_t sversion, char* msg, int32_t msgLen) {
  STqCheckInfo info = {0};
  SDecoder     decoder;
  tDecoderInit(&decoder, (uint8_t*)msg, msgLen);
  if (tDecodeSTqCheckInfo(&decoder, &info) < 0) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }
  tDecoderClear(&decoder);
  if (taosHashPut(pTq->pCheckInfo, info.topic, strlen(info.topic), &info, sizeof(STqCheckInfo)) < 0) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }
  if (tqMetaSaveCheckInfo(pTq, info.topic, msg, msgLen) < 0) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }
  return 0;
}

int32_t tqProcessDelCheckInfoReq(STQ* pTq, int64_t sversion, char* msg, int32_t msgLen) {
  if (taosHashRemove(pTq->pCheckInfo, msg, strlen(msg)) < 0) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }
  if (tqMetaDeleteCheckInfo(pTq, msg) < 0) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }
  return 0;
}

int32_t tqProcessSubscribeReq(STQ* pTq, int64_t sversion, char* msg, int32_t msgLen) {
  SMqRebVgReq req = {0};
  tDecodeSMqRebVgReq(msg, &req);
  // todo lock

  tqDebug("vgId:%d, tq process sub req %s", pTq->pVnode->config.vgId, req.subKey);

  STqHandle* pHandle = taosHashGet(pTq->pHandle, req.subKey, strlen(req.subKey));
  if (pHandle == NULL) {
    if (req.oldConsumerId != -1) {
      tqError("vgId:%d, build new consumer handle %s for consumer:0x%" PRIx64 ", but old consumerId is %" PRId64 "",
              req.vgId, req.subKey, req.newConsumerId, req.oldConsumerId);
    }
    if (req.newConsumerId == -1) {
      tqError("vgId:%d, tq invalid rebalance request, new consumerId %" PRId64 "", req.vgId, req.newConsumerId);
      taosMemoryFree(req.qmsg);
      return 0;
    }
    STqHandle tqHandle = {0};
    pHandle = &tqHandle;
    /*taosInitRWLatch(&pExec->lock);*/

    memcpy(pHandle->subKey, req.subKey, TSDB_SUBSCRIBE_KEY_LEN);
    pHandle->consumerId = req.newConsumerId;
    pHandle->epoch = -1;

    pHandle->execHandle.subType = req.subType;
    pHandle->fetchMeta = req.withMeta;

    // TODO version should be assigned and refed during preprocess
    SWalRef* pRef = walRefCommittedVer(pTq->pVnode->pWal);
    if (pRef == NULL) {
      return -1;
    }
    int64_t ver = pRef->refVer;
    pHandle->pRef = pRef;

    SReadHandle handle = {
        .meta = pTq->pVnode->pMeta,
        .vnode = pTq->pVnode,
        .initTableReader = true,
        .initTqReader = true,
        .version = ver,
    };
    pHandle->snapshotVer = ver;

    if (pHandle->execHandle.subType == TOPIC_SUB_TYPE__COLUMN) {
      pHandle->execHandle.execCol.qmsg = req.qmsg;
      req.qmsg = NULL;

      pHandle->execHandle.task =
          qCreateQueueExecTaskInfo(pHandle->execHandle.execCol.qmsg, &handle, &pHandle->execHandle.numOfCols, NULL);
      void* scanner = NULL;
      qExtractStreamScanner(pHandle->execHandle.task, &scanner);
      pHandle->execHandle.pExecReader = qExtractReaderFromStreamScanner(scanner);
    } else if (pHandle->execHandle.subType == TOPIC_SUB_TYPE__DB) {
      pHandle->pWalReader = walOpenReader(pTq->pVnode->pWal, NULL);
      pHandle->execHandle.pExecReader = tqOpenReader(pTq->pVnode);
      pHandle->execHandle.execDb.pFilterOutTbUid =
          taosHashInit(64, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), false, HASH_NO_LOCK);
      buildSnapContext(handle.meta, handle.version, 0, pHandle->execHandle.subType, pHandle->fetchMeta,
                       (SSnapContext**)(&handle.sContext));

      pHandle->execHandle.task = qCreateQueueExecTaskInfo(NULL, &handle, NULL, NULL);
    } else if (pHandle->execHandle.subType == TOPIC_SUB_TYPE__TABLE) {
      pHandle->pWalReader = walOpenReader(pTq->pVnode->pWal, NULL);

      pHandle->execHandle.execTb.suid = req.suid;

      SArray* tbUidList = taosArrayInit(0, sizeof(int64_t));
      vnodeGetCtbIdList(pTq->pVnode, req.suid, tbUidList);
      tqDebug("vgId:%d, tq try to get all ctb, suid:%" PRId64, pTq->pVnode->config.vgId, req.suid);
      for (int32_t i = 0; i < taosArrayGetSize(tbUidList); i++) {
        int64_t tbUid = *(int64_t*)taosArrayGet(tbUidList, i);
        tqDebug("vgId:%d, idx %d, uid:%" PRId64, TD_VID(pTq->pVnode), i, tbUid);
      }
      pHandle->execHandle.pExecReader = tqOpenReader(pTq->pVnode);
      tqReaderSetTbUidList(pHandle->execHandle.pExecReader, tbUidList);
      taosArrayDestroy(tbUidList);

      buildSnapContext(handle.meta, handle.version, req.suid, pHandle->execHandle.subType, pHandle->fetchMeta,
                       (SSnapContext**)(&handle.sContext));
      pHandle->execHandle.task = qCreateQueueExecTaskInfo(NULL, &handle, NULL, NULL);
    }
    taosHashPut(pTq->pHandle, req.subKey, strlen(req.subKey), pHandle, sizeof(STqHandle));
    tqDebug("try to persist handle %s consumer:0x%" PRIx64, req.subKey, pHandle->consumerId);
    if (tqMetaSaveHandle(pTq, req.subKey, pHandle) < 0) {
      return -1;
    }
  } else {
    // TODO handle qmsg and exec modification
    atomic_store_32(&pHandle->epoch, -1);
    atomic_store_64(&pHandle->consumerId, req.newConsumerId);
    atomic_add_fetch_32(&pHandle->epoch, 1);
    taosMemoryFree(req.qmsg);
    if (pHandle->execHandle.subType == TOPIC_SUB_TYPE__COLUMN) {
      qStreamCloseTsdbReader(pHandle->execHandle.task);
    }
    if (tqMetaSaveHandle(pTq, req.subKey, pHandle) < 0) {
      return -1;
    }
    // close handle
  }

  return 0;
}

int32_t tqExpandTask(STQ* pTq, SStreamTask* pTask, int64_t ver) {
#if 0
  if (pTask->taskLevel == TASK_LEVEL__AGG) {
    A(taosArrayGetSize(pTask->childEpInfo) != 0);
  }
#endif

  pTask->refCnt = 1;
  pTask->schedStatus = TASK_SCHED_STATUS__INACTIVE;

  pTask->inputQueue = streamQueueOpen();
  pTask->outputQueue = streamQueueOpen();

  if (pTask->inputQueue == NULL || pTask->outputQueue == NULL) {
    return -1;
  }

  pTask->inputStatus = TASK_INPUT_STATUS__NORMAL;
  pTask->outputStatus = TASK_OUTPUT_STATUS__NORMAL;

  pTask->pMsgCb = &pTq->pVnode->msgCb;

  pTask->startVer = ver;

  // expand executor
  if (pTask->fillHistory) {
    pTask->taskStatus = TASK_STATUS__WAIT_DOWNSTREAM;
  }

  if (pTask->taskLevel == TASK_LEVEL__SOURCE) {
    pTask->pState = streamStateOpen(pTq->pStreamMeta->path, pTask, false, -1, -1);
    if (pTask->pState == NULL) {
      return -1;
    }

    SReadHandle handle = {
        .meta = pTq->pVnode->pMeta,
        .vnode = pTq->pVnode,
        .initTqReader = 1,
        .pStateBackend = pTask->pState,
    };
    pTask->exec.executor = qCreateStreamExecTaskInfo(pTask->exec.qmsg, &handle);
    if (pTask->exec.executor == NULL) {
      return -1;
    }

  } else if (pTask->taskLevel == TASK_LEVEL__AGG) {
    pTask->pState = streamStateOpen(pTq->pStreamMeta->path, pTask, false, -1, -1);
    if (pTask->pState == NULL) {
      return -1;
    }
    SReadHandle mgHandle = {
        .vnode = NULL,
        .numOfVgroups = (int32_t)taosArrayGetSize(pTask->childEpInfo),
        .pStateBackend = pTask->pState,
    };
    pTask->exec.executor = qCreateStreamExecTaskInfo(pTask->exec.qmsg, &mgHandle);
    if (pTask->exec.executor == NULL) {
      return -1;
    }
  }

  // sink
  /*pTask->ahandle = pTq->pVnode;*/
  if (pTask->outputType == TASK_OUTPUT__SMA) {
    pTask->smaSink.vnode = pTq->pVnode;
    pTask->smaSink.smaSink = smaHandleRes;
  } else if (pTask->outputType == TASK_OUTPUT__TABLE) {
    pTask->tbSink.vnode = pTq->pVnode;
    pTask->tbSink.tbSinkFunc = tqSinkToTablePipeline2;

    /*A(pTask->tbSink.pSchemaWrapper);*/
    /*A(pTask->tbSink.pSchemaWrapper->pSchema);*/

    pTask->tbSink.pTSchema =
        tBuildTSchema(pTask->tbSink.pSchemaWrapper->pSchema, pTask->tbSink.pSchemaWrapper->nCols, 1);
    ASSERT(pTask->tbSink.pTSchema);
  }

  streamSetupTrigger(pTask);

  tqInfo("expand stream task on vg %d, task id %d, child id %d, level %d", TD_VID(pTq->pVnode), pTask->taskId,
         pTask->selfChildId, pTask->taskLevel);
  return 0;
}

int32_t tqProcessStreamTaskCheckReq(STQ* pTq, SRpcMsg* pMsg) {
  char*               msgStr = pMsg->pCont;
  char*               msgBody = POINTER_SHIFT(msgStr, sizeof(SMsgHead));
  int32_t             msgLen = pMsg->contLen - sizeof(SMsgHead);
  SStreamTaskCheckReq req;
  SDecoder            decoder;
  tDecoderInit(&decoder, (uint8_t*)msgBody, msgLen);
  tDecodeSStreamTaskCheckReq(&decoder, &req);
  tDecoderClear(&decoder);
  int32_t             taskId = req.downstreamTaskId;
  SStreamTaskCheckRsp rsp = {
      .reqId = req.reqId,
      .streamId = req.streamId,
      .childId = req.childId,
      .downstreamNodeId = req.downstreamNodeId,
      .downstreamTaskId = req.downstreamTaskId,
      .upstreamNodeId = req.upstreamNodeId,
      .upstreamTaskId = req.upstreamTaskId,
  };
  SStreamTask* pTask = streamMetaAcquireTask(pTq->pStreamMeta, taskId);
  if (pTask && atomic_load_8(&pTask->taskStatus) == TASK_STATUS__NORMAL) {
    rsp.status = 1;
  } else {
    rsp.status = 0;
  }

  if (pTask) streamMetaReleaseTask(pTq->pStreamMeta, pTask);

  tqDebug("tq recv task check req(reqId:0x%" PRIx64 ") %d at node %d check req from task %d at node %d, status %d",
          rsp.reqId, rsp.downstreamTaskId, rsp.downstreamNodeId, rsp.upstreamTaskId, rsp.upstreamNodeId, rsp.status);

  SEncoder encoder;
  int32_t  code;
  int32_t  len;
  tEncodeSize(tEncodeSStreamTaskCheckRsp, &rsp, len, code);
  if (code < 0) {
    tqError("unable to encode rsp %d", __LINE__);
    return -1;
  }

  void* buf = rpcMallocCont(sizeof(SMsgHead) + len);
  ((SMsgHead*)buf)->vgId = htonl(req.upstreamNodeId);

  void* abuf = POINTER_SHIFT(buf, sizeof(SMsgHead));
  tEncoderInit(&encoder, (uint8_t*)abuf, len);
  tEncodeSStreamTaskCheckRsp(&encoder, &rsp);
  tEncoderClear(&encoder);

  SRpcMsg rspMsg = {
      .code = 0,
      .pCont = buf,
      .contLen = sizeof(SMsgHead) + len,
      .info = pMsg->info,
  };

  tmsgSendRsp(&rspMsg);
  return 0;
}

int32_t tqProcessStreamTaskCheckRsp(STQ* pTq, int64_t sversion, char* msg, int32_t msgLen) {
  int32_t             code;
  SStreamTaskCheckRsp rsp;

  SDecoder decoder;
  tDecoderInit(&decoder, (uint8_t*)msg, msgLen);
  code = tDecodeSStreamTaskCheckRsp(&decoder, &rsp);
  if (code < 0) {
    tDecoderClear(&decoder);
    return -1;
  }
  tDecoderClear(&decoder);

  tqDebug("tq recv task check rsp(reqId:0x%" PRIx64 ") %d at node %d check req from task %d at node %d, status %d",
          rsp.reqId, rsp.downstreamTaskId, rsp.downstreamNodeId, rsp.upstreamTaskId, rsp.upstreamNodeId, rsp.status);

  SStreamTask* pTask = streamMetaAcquireTask(pTq->pStreamMeta, rsp.upstreamTaskId);
  if (pTask == NULL) {
    return -1;
  }

  code = streamProcessTaskCheckRsp(pTask, &rsp, sversion);
  streamMetaReleaseTask(pTq->pStreamMeta, pTask);
  return code;
}

int32_t tqProcessTaskDeployReq(STQ* pTq, int64_t sversion, char* msg, int32_t msgLen) {
  int32_t code;
#if 0
  code = streamMetaAddSerializedTask(pTq->pStreamMeta, version, msg, msgLen);
  if (code < 0) return code;
#endif
  if (tsDisableStream) {
    return 0;
  }

  // 1.deserialize msg and build task
  SStreamTask* pTask = taosMemoryCalloc(1, sizeof(SStreamTask));
  if (pTask == NULL) {
    return -1;
  }

  SDecoder decoder;
  tDecoderInit(&decoder, (uint8_t*)msg, msgLen);
  code = tDecodeSStreamTask(&decoder, pTask);
  if (code < 0) {
    tDecoderClear(&decoder);
    taosMemoryFree(pTask);
    return -1;
  }
  tDecoderClear(&decoder);

  // 2.save task
  code = streamMetaAddTask(pTq->pStreamMeta, sversion, pTask);
  if (code < 0) {
    return -1;
  }

  // 3.go through recover steps to fill history
  if (pTask->fillHistory) {
    streamTaskCheckDownstream(pTask, sversion);
  }

  return 0;
}

int32_t tqProcessTaskRecover1Req(STQ* pTq, SRpcMsg* pMsg) {
  int32_t code;
  char*   msg = pMsg->pCont;
  int32_t msgLen = pMsg->contLen;

  SStreamRecoverStep1Req* pReq = (SStreamRecoverStep1Req*)msg;
  SStreamTask*            pTask = streamMetaAcquireTask(pTq->pStreamMeta, pReq->taskId);
  if (pTask == NULL) {
    return -1;
  }

  // check param
  int64_t fillVer1 = pTask->startVer;
  if (fillVer1 <= 0) {
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    return -1;
  }

  // do recovery step 1
  streamSourceRecoverScanStep1(pTask);

  if (atomic_load_8(&pTask->taskStatus) == TASK_STATUS__DROPPING) {
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    return 0;
  }

  // build msg to launch next step
  SStreamRecoverStep2Req req;
  code = streamBuildSourceRecover2Req(pTask, &req);
  if (code < 0) {
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    return -1;
  }

  streamMetaReleaseTask(pTq->pStreamMeta, pTask);

  if (atomic_load_8(&pTask->taskStatus) == TASK_STATUS__DROPPING) {
    return 0;
  }

  // serialize msg
  int32_t len = sizeof(SStreamRecoverStep1Req);

  void* serializedReq = rpcMallocCont(len);
  if (serializedReq == NULL) {
    return -1;
  }

  memcpy(serializedReq, &req, len);

  // dispatch msg
  SRpcMsg rpcMsg = {
      .code = 0,
      .contLen = len,
      .msgType = TDMT_VND_STREAM_RECOVER_BLOCKING_STAGE,
      .pCont = serializedReq,
  };

  tmsgPutToQueue(&pTq->pVnode->msgCb, WRITE_QUEUE, &rpcMsg);

  return 0;
}

int32_t tqProcessTaskRecover2Req(STQ* pTq, int64_t sversion, char* msg, int32_t msgLen) {
  int32_t                 code;
  SStreamRecoverStep2Req* pReq = (SStreamRecoverStep2Req*)msg;
  SStreamTask*            pTask = streamMetaAcquireTask(pTq->pStreamMeta, pReq->taskId);
  if (pTask == NULL) {
    return -1;
  }

  // do recovery step 2
  code = streamSourceRecoverScanStep2(pTask, sversion);
  if (code < 0) {
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    return -1;
  }

  if (atomic_load_8(&pTask->taskStatus) == TASK_STATUS__DROPPING) {
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    return 0;
  }

  // restore param
  code = streamRestoreParam(pTask);
  if (code < 0) {
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    return -1;
  }

  // set status normal
  code = streamSetStatusNormal(pTask);
  if (code < 0) {
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    return -1;
  }

  // dispatch recover finish req to all related downstream task
  code = streamDispatchRecoverFinishReq(pTask);
  if (code < 0) {
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    return -1;
  }

  atomic_store_8(&pTask->fillHistory, 0);
  streamMetaSaveTask(pTq->pStreamMeta, pTask);

  streamMetaReleaseTask(pTq->pStreamMeta, pTask);

  return 0;
}

int32_t tqProcessTaskRecoverFinishReq(STQ* pTq, SRpcMsg* pMsg) {
  char*   msg = POINTER_SHIFT(pMsg->pCont, sizeof(SMsgHead));
  int32_t msgLen = pMsg->contLen - sizeof(SMsgHead);

  // deserialize
  SStreamRecoverFinishReq req;

  SDecoder decoder;
  tDecoderInit(&decoder, (uint8_t*)msg, msgLen);
  tDecodeSStreamRecoverFinishReq(&decoder, &req);
  tDecoderClear(&decoder);

  // find task
  SStreamTask* pTask = streamMetaAcquireTask(pTq->pStreamMeta, req.taskId);
  if (pTask == NULL) {
    return -1;
  }
  // do process request
  if (streamProcessRecoverFinishReq(pTask, req.childId) < 0) {
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    return -1;
  }

  streamMetaReleaseTask(pTq->pStreamMeta, pTask);
  return 0;
}

int32_t tqProcessTaskRecoverFinishRsp(STQ* pTq, SRpcMsg* pMsg) {
  //
  return 0;
}

int32_t tqProcessDelReq(STQ* pTq, void* pReq, int32_t len, int64_t ver) {
  bool        failed = false;
  SDecoder*   pCoder = &(SDecoder){0};
  SDeleteRes* pRes = &(SDeleteRes){0};

  pRes->uidList = taosArrayInit(0, sizeof(tb_uid_t));
  if (pRes->uidList == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    failed = true;
  }

  tDecoderInit(pCoder, pReq, len);
  tDecodeDeleteRes(pCoder, pRes);
  tDecoderClear(pCoder);

  int32_t sz = taosArrayGetSize(pRes->uidList);
  if (sz == 0 || pRes->affectedRows == 0) {
    taosArrayDestroy(pRes->uidList);
    return 0;
  }
  SSDataBlock* pDelBlock = createSpecialDataBlock(STREAM_DELETE_DATA);
  blockDataEnsureCapacity(pDelBlock, sz);
  pDelBlock->info.rows = sz;
  pDelBlock->info.version = ver;

  for (int32_t i = 0; i < sz; i++) {
    // start key column
    SColumnInfoData* pStartCol = taosArrayGet(pDelBlock->pDataBlock, START_TS_COLUMN_INDEX);
    colDataSetVal(pStartCol, i, (const char*)&pRes->skey, false);  // end key column
    SColumnInfoData* pEndCol = taosArrayGet(pDelBlock->pDataBlock, END_TS_COLUMN_INDEX);
    colDataSetVal(pEndCol, i, (const char*)&pRes->ekey, false);
    // uid column
    SColumnInfoData* pUidCol = taosArrayGet(pDelBlock->pDataBlock, UID_COLUMN_INDEX);
    int64_t*         pUid = taosArrayGet(pRes->uidList, i);
    colDataSetVal(pUidCol, i, (const char*)pUid, false);

    colDataSetNULL(taosArrayGet(pDelBlock->pDataBlock, GROUPID_COLUMN_INDEX), i);
    colDataSetNULL(taosArrayGet(pDelBlock->pDataBlock, CALCULATE_START_TS_COLUMN_INDEX), i);
    colDataSetNULL(taosArrayGet(pDelBlock->pDataBlock, CALCULATE_END_TS_COLUMN_INDEX), i);
  }

  taosArrayDestroy(pRes->uidList);

  int32_t* pRef = taosMemoryMalloc(sizeof(int32_t));
  *pRef = 1;

  void* pIter = NULL;
  while (1) {
    pIter = taosHashIterate(pTq->pStreamMeta->pTasks, pIter);
    if (pIter == NULL) break;
    SStreamTask* pTask = *(SStreamTask**)pIter;
    if (pTask->taskLevel != TASK_LEVEL__SOURCE) continue;

    qDebug("delete req enqueue stream task: %d, ver: %" PRId64, pTask->taskId, ver);

    if (!failed) {
      SStreamRefDataBlock* pRefBlock = taosAllocateQitem(sizeof(SStreamRefDataBlock), DEF_QITEM, 0);
      pRefBlock->type = STREAM_INPUT__REF_DATA_BLOCK;
      pRefBlock->pBlock = pDelBlock;
      pRefBlock->dataRef = pRef;
      atomic_add_fetch_32(pRefBlock->dataRef, 1);

      if (streamTaskInput(pTask, (SStreamQueueItem*)pRefBlock) < 0) {
        qError("stream task input del failed, task id %d", pTask->taskId);

        atomic_sub_fetch_32(pRef, 1);
        taosFreeQitem(pRefBlock);
        continue;
      }

      if (streamSchedExec(pTask) < 0) {
        qError("stream task launch failed, task id %d", pTask->taskId);
        continue;
      }

    } else {
      streamTaskInputFail(pTask);
    }
  }

  int32_t ref = atomic_sub_fetch_32(pRef, 1);
  /*A(ref >= 0);*/
  if (ref == 0) {
    blockDataDestroy(pDelBlock);
    taosMemoryFree(pRef);
  }

#if 0
    SStreamDataBlock* pStreamBlock = taosAllocateQitem(sizeof(SStreamDataBlock), DEF_QITEM, 0);
    pStreamBlock->type = STREAM_INPUT__DATA_BLOCK;
    pStreamBlock->blocks = taosArrayInit(0, sizeof(SSDataBlock));
    SSDataBlock block = {0};
    assignOneDataBlock(&block, pDelBlock);
    block.info.type = STREAM_DELETE_DATA;
    taosArrayPush(pStreamBlock->blocks, &block);

    if (!failed) {
      if (streamTaskInput(pTask, (SStreamQueueItem*)pStreamBlock) < 0) {
        qError("stream task input del failed, task id %d", pTask->taskId);
        continue;
      }

      if (streamSchedExec(pTask) < 0) {
        qError("stream task launch failed, task id %d", pTask->taskId);
        continue;
      }
    } else {
      streamTaskInputFail(pTask);
    }
  }
  blockDataDestroy(pDelBlock);
#endif

  return 0;
}

int32_t tqProcessSubmitReq(STQ* pTq, SPackedData submit) {
  void*               pIter = NULL;
  bool                failed = false;
  SStreamDataSubmit2* pSubmit = NULL;

  pSubmit = streamDataSubmitNew(submit);
  if (pSubmit == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    tqError("failed to create data submit for stream since out of memory");
    failed = true;
  }

  while (1) {
    pIter = taosHashIterate(pTq->pStreamMeta->pTasks, pIter);
    if (pIter == NULL) {
      break;
    }

    SStreamTask* pTask = *(SStreamTask**)pIter;
    if (pTask->taskLevel != TASK_LEVEL__SOURCE) continue;
    if (pTask->taskStatus == TASK_STATUS__RECOVER_PREPARE || pTask->taskStatus == TASK_STATUS__WAIT_DOWNSTREAM) {
      tqDebug("skip push task %d, task status %d", pTask->taskId, pTask->taskStatus);
      continue;
    }

    tqDebug("data submit enqueue stream task: %d, ver: %" PRId64, pTask->taskId, submit.ver);

    if (!failed) {
      if (streamTaskInput(pTask, (SStreamQueueItem*)pSubmit) < 0) {
        tqError("stream task input failed, task id %d", pTask->taskId);
        continue;
      }

      if (streamSchedExec(pTask) < 0) {
        tqError("stream task launch failed, task id %d", pTask->taskId);
        continue;
      }
    } else {
      streamTaskInputFail(pTask);
    }
  }

  if (pSubmit) {
    streamDataSubmitRefDec(pSubmit);
    taosFreeQitem(pSubmit);
  }

  return failed ? -1 : 0;
}

int32_t tqProcessTaskRunReq(STQ* pTq, SRpcMsg* pMsg) {
  SStreamTaskRunReq* pReq = pMsg->pCont;
  int32_t            taskId = pReq->taskId;
  SStreamTask*       pTask = streamMetaAcquireTask(pTq->pStreamMeta, taskId);
  if (pTask) {
    streamProcessRunReq(pTask);
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    return 0;
  } else {
    return -1;
  }
}

int32_t tqProcessTaskDispatchReq(STQ* pTq, SRpcMsg* pMsg, bool exec) {
  char*              msgStr = pMsg->pCont;
  char*              msgBody = POINTER_SHIFT(msgStr, sizeof(SMsgHead));
  int32_t            msgLen = pMsg->contLen - sizeof(SMsgHead);
  SStreamDispatchReq req;
  SDecoder           decoder;
  tDecoderInit(&decoder, (uint8_t*)msgBody, msgLen);
  tDecodeStreamDispatchReq(&decoder, &req);
  int32_t taskId = req.taskId;

  SStreamTask* pTask = streamMetaAcquireTask(pTq->pStreamMeta, taskId);
  if (pTask) {
    SRpcMsg rsp = {
        .info = pMsg->info,
        .code = 0,
    };
    streamProcessDispatchReq(pTask, &req, &rsp, exec);
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    return 0;
  } else {
    return -1;
  }
}

int32_t tqProcessTaskDispatchRsp(STQ* pTq, SRpcMsg* pMsg) {
  SStreamDispatchRsp* pRsp = POINTER_SHIFT(pMsg->pCont, sizeof(SMsgHead));
  int32_t             taskId = ntohl(pRsp->upstreamTaskId);
  SStreamTask*        pTask = streamMetaAcquireTask(pTq->pStreamMeta, taskId);
  tqDebug("recv dispatch rsp, code: %x", pMsg->code);
  if (pTask) {
    streamProcessDispatchRsp(pTask, pRsp, pMsg->code);
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    return 0;
  } else {
    return -1;
  }
}

int32_t tqProcessTaskDropReq(STQ* pTq, int64_t sversion, char* msg, int32_t msgLen) {
  SVDropStreamTaskReq* pReq = (SVDropStreamTaskReq*)msg;
  streamMetaRemoveTask(pTq->pStreamMeta, pReq->taskId);
  return 0;
}

int32_t tqProcessTaskRetrieveReq(STQ* pTq, SRpcMsg* pMsg) {
  char*              msgStr = pMsg->pCont;
  char*              msgBody = POINTER_SHIFT(msgStr, sizeof(SMsgHead));
  int32_t            msgLen = pMsg->contLen - sizeof(SMsgHead);
  SStreamRetrieveReq req;
  SDecoder           decoder;
  tDecoderInit(&decoder, (uint8_t*)msgBody, msgLen);
  tDecodeStreamRetrieveReq(&decoder, &req);
  tDecoderClear(&decoder);
  int32_t      taskId = req.dstTaskId;
  SStreamTask* pTask = streamMetaAcquireTask(pTq->pStreamMeta, taskId);
  if (pTask) {
    SRpcMsg rsp = {
        .info = pMsg->info,
        .code = 0,
    };
    streamProcessRetrieveReq(pTask, &req, &rsp);
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    tDeleteStreamRetrieveReq(&req);
    return 0;
  } else {
    return -1;
  }
}

int32_t tqProcessTaskRetrieveRsp(STQ* pTq, SRpcMsg* pMsg) {
  //
  return 0;
}

int32_t vnodeEnqueueStreamMsg(SVnode* pVnode, SRpcMsg* pMsg) {
  STQ*      pTq = pVnode->pTq;
  SMsgHead* msgStr = pMsg->pCont;
  char*     msgBody = POINTER_SHIFT(msgStr, sizeof(SMsgHead));
  int32_t   msgLen = pMsg->contLen - sizeof(SMsgHead);
  int32_t   code = 0;

  SStreamDispatchReq req;
  SDecoder           decoder;
  tDecoderInit(&decoder, (uint8_t*)msgBody, msgLen);
  if (tDecodeStreamDispatchReq(&decoder, &req) < 0) {
    code = TSDB_CODE_MSG_DECODE_ERROR;
    tDecoderClear(&decoder);
    goto FAIL;
  }
  tDecoderClear(&decoder);

  int32_t taskId = req.taskId;

  SStreamTask* pTask = streamMetaAcquireTask(pTq->pStreamMeta, taskId);
  if (pTask) {
    SRpcMsg rsp = {
        .info = pMsg->info,
        .code = 0,
    };
    streamProcessDispatchReq(pTask, &req, &rsp, false);
    streamMetaReleaseTask(pTq->pStreamMeta, pTask);
    rpcFreeCont(pMsg->pCont);
    taosFreeQitem(pMsg);
    return 0;
  }

  code = TSDB_CODE_STREAM_TASK_NOT_EXIST;

FAIL:
  if (pMsg->info.handle == NULL) return -1;

  SMsgHead* pRspHead = rpcMallocCont(sizeof(SMsgHead) + sizeof(SStreamDispatchRsp));
  if (pRspHead == NULL) {
    SRpcMsg rsp = {
        .code = TSDB_CODE_OUT_OF_MEMORY,
        .info = pMsg->info,
    };
    tqDebug("send dispatch error rsp, code: %x", code);
    tmsgSendRsp(&rsp);
    rpcFreeCont(pMsg->pCont);
    taosFreeQitem(pMsg);
    return -1;
  }

  pRspHead->vgId = htonl(req.upstreamNodeId);
  SStreamDispatchRsp* pRsp = POINTER_SHIFT(pRspHead, sizeof(SMsgHead));
  pRsp->streamId = htobe64(req.streamId);
  pRsp->upstreamTaskId = htonl(req.upstreamTaskId);
  pRsp->upstreamNodeId = htonl(req.upstreamNodeId);
  pRsp->downstreamNodeId = htonl(pVnode->config.vgId);
  pRsp->downstreamTaskId = htonl(req.taskId);
  pRsp->inputStatus = TASK_OUTPUT_STATUS__NORMAL;

  SRpcMsg rsp = {
      .code = code,
      .info = pMsg->info,
      .contLen = sizeof(SMsgHead) + sizeof(SStreamDispatchRsp),
      .pCont = pRspHead,
  };
  tqDebug("send dispatch error rsp, code: %x", code);
  tmsgSendRsp(&rsp);
  rpcFreeCont(pMsg->pCont);
  taosFreeQitem(pMsg);
  return -1;
}

int32_t tqCheckLogInWal(STQ* pTq, int64_t sversion) { return sversion <= pTq->walLogLastVer; }
