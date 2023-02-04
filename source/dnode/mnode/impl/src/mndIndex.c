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

#define _DEFAULT_SOURCE
#include "mndIndex.h"
#include "mndDb.h"
#include "mndDnode.h"
#include "mndInfoSchema.h"
#include "mndMnode.h"
#include "mndPrivilege.h"
#include "mndScheduler.h"
#include "mndShow.h"
#include "mndStb.h"
#include "mndStream.h"
#include "mndTrans.h"
#include "mndUser.h"
#include "mndVgroup.h"
#include "parser.h"
#include "tname.h"

#define TSDB_IDX_VER_NUMBER   1
#define TSDB_IDX_RESERVE_SIZE 64

static SSdbRaw *mndIdxActionEncode(SIdxObj *pSma);
static SSdbRow *mndIdxActionDecode(SSdbRaw *pRaw);
static int32_t  mndIdxActionInsert(SSdb *pSdb, SIdxObj *pIdx);
static int32_t  mndIdxActionDelete(SSdb *pSdb, SIdxObj *pIdx);
static int32_t  mndIdxActionUpdate(SSdb *pSdb, SIdxObj *pOld, SIdxObj *pNew);
static int32_t  mndProcessCreateIdxReq(SRpcMsg *pReq);
static int32_t  mndProcessDropIdxReq(SRpcMsg *pReq);
static int32_t  mndProcessGetIdxReq(SRpcMsg *pReq);
static int32_t  mndProcessGetTbIdxReq(SRpcMsg *pReq);
static int32_t  mndRetrieveIdx(SRpcMsg *pReq, SShowObj *pShow, SSDataBlock *pBlock, int32_t rows);
static void     mndCancelGetNextIdx(SMnode *pMnode, void *pIter);
static void     mndDestroyIdxObj(SIdxObj *pIdxObj);

static int32_t mndAddIndex(SMnode *pMnode, SRpcMsg *pReq, SCreateTagIndexReq *req, SDbObj *pDb, SStbObj *pStb);

int32_t mndInitIdx(SMnode *pMnode) {
  SSdbTable table = {
      .sdbType = SDB_IDX,
      .keyType = SDB_KEY_BINARY,
      .encodeFp = (SdbEncodeFp)mndIdxActionEncode,
      .decodeFp = (SdbDecodeFp)mndIdxActionDecode,
      .insertFp = (SdbInsertFp)mndIdxActionInsert,
      .updateFp = (SdbUpdateFp)mndIdxActionUpdate,
      .deleteFp = (SdbDeleteFp)mndIdxActionDelete,
  };

  mndSetMsgHandle(pMnode, TDMT_MND_CREATE_INDEX, mndProcessCreateIdxReq);
  mndSetMsgHandle(pMnode, TDMT_MND_DROP_INDEX, mndProcessDropIdxReq);
  mndSetMsgHandle(pMnode, TDMT_VND_CREATE_INDEX_RSP, mndTransProcessRsp);
  mndSetMsgHandle(pMnode, TDMT_VND_DROP_INDEX_RSP, mndTransProcessRsp);

  // mndSetMsgHandle(pMnode, TDMT_MND_CREATE_SMA, mndProcessCreateIdxReq);
  // mndSetMsgHandle(pMnode, TDMT_MND_DROP_SMA, mndProcessDropIdxReq);
  // mndSetMsgHandle(pMnode, TDMT_VND_CREATE_SMA_RSP, mndTransProcessRsp);
  // mndSetMsgHandle(pMnode, TDMT_VND_DROP_SMA_RSP, mndTransProcessRsp);
  // mndSetMsgHandle(pMnode, TDMT_MND_GET_INDEX, mndProcessGetIdxReq);
  // mndSetMsgHandle(pMnode, TDMT_MND_GET_TABLE_INDEX, mndProcessGetTbIdxReq);

  // type same with sma
  // mndAddShowRetrieveHandle(pMnode, TSDB_MGMT_TABLE_INDEX, mndRetrieveIdx);
  // mndAddShowFreeIterHandle(pMnode, TSDB_MGMT_TABLE_INDEX, mndCancelGetNextIdx);
  return sdbSetTable(pMnode->pSdb, table);
}

static int32_t mndFindSuperTableTagId(const SStbObj *pStb, const char *tagName) {
  for (int32_t tag = 0; tag < pStb->numOfTags; tag++) {
    if (strcasecmp(pStb->pTags[tag].name, tagName) == 0) {
      return tag;
    }
  }

  return -1;
}

int mndSetCreateIdxRedoActions(SMnode *pMnode, STrans *pTrans, SDbObj *pDb, SStbObj *pStb, SIdxObj *pIdx) {
  SSdb   *pSdb = pMnode->pSdb;
  SVgObj *pVgroup = NULL;
  void   *pIter = NULL;
  int32_t contLen;

  while (1) {
    pIter = sdbFetch(pSdb, SDB_VGROUP, pIter, (void **)&pVgroup);
    if (pIter == NULL) break;
    if (!mndVgroupInDb(pVgroup, pDb->uid)) {
      sdbRelease(pSdb, pVgroup);
      continue;
    }

    void *pReq = mndBuildVCreateStbReq(pMnode, pVgroup, pStb, &contLen, NULL, 0);
    if (pReq == NULL) {
      sdbCancelFetch(pSdb, pIter);
      sdbRelease(pSdb, pVgroup);
      return -1;
    }
    STransAction action = {0};
    action.epSet = mndGetVgroupEpset(pMnode, pVgroup);
    action.pCont = pReq;
    action.contLen = contLen;
    action.msgType = TDMT_VND_CREATE_INDEX;
    if (mndTransAppendRedoAction(pTrans, &action) != 0) {
      taosMemoryFree(pReq);
      sdbCancelFetch(pSdb, pIter);
      sdbRelease(pSdb, pVgroup);
      return -1;
    }
    sdbRelease(pSdb, pVgroup);
  }

  return 0;
}
static void *mndBuildDropIdxReq(SMnode *pMnode, SVgObj *pVgroup, SStbObj *pStbObj, SIdxObj *pIdx, int32_t *contLen) {
  // TODO
  SDropIndexReq req = {0};
  memcpy(req.colName, pIdx->colName, sizeof(pIdx->colName));
  memcpy(req.stb, pIdx->stb, sizeof(pIdx->stb));
  req.dbUid = pIdx->dbUid;
  req.stbUid = pIdx->stbUid;

  mInfo("idx: %s start to build drop index req", pIdx->name);
  int32_t len = tSerializeSDropIdxReq(NULL, 0, &req);
  if (len < 0) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  void *pCont = taosMemoryCalloc(1, len);
  if (pCont == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }
  tSerializeSDropIdxReq(pCont, len, &req);
  *contLen = len;
  return pCont;
}
int mndSetDropIdxRedoActions(SMnode *pMnode, STrans *pTrans, SDbObj *pDb, SStbObj *pStb, SIdxObj *pIdx) {
  SSdb   *pSdb = pMnode->pSdb;
  SVgObj *pVgroup = NULL;
  void   *pIter = NULL;
  int32_t contLen;

  while (1) {
    pIter = sdbFetch(pSdb, SDB_VGROUP, pIter, (void **)&pVgroup);
    if (pIter == NULL) break;
    if (!mndVgroupInDb(pVgroup, pDb->uid)) {
      sdbRelease(pSdb, pVgroup);
      continue;
    }

    int32_t len;
    void   *pReq = mndBuildDropIdxReq(pMnode, pVgroup, pStb, pIdx, &len);
    if (pReq == NULL) {
      sdbCancelFetch(pSdb, pIter);
      sdbRelease(pSdb, pVgroup);
      return -1;
    }
    STransAction action = {0};
    action.epSet = mndGetVgroupEpset(pMnode, pVgroup);
    action.pCont = pReq;
    action.contLen = len;
    action.msgType = TDMT_VND_DROP_INDEX;
    if (mndTransAppendRedoAction(pTrans, &action) != 0) {
      taosMemoryFree(pReq);
      sdbCancelFetch(pSdb, pIter);
      sdbRelease(pSdb, pVgroup);
      return -1;
    }
    sdbRelease(pSdb, pVgroup);
  }

  return 0;
}

void mndCleanupIdx(SMnode *pMnode) {
  // do nothing
  return;
}

static SSdbRaw *mndIdxActionEncode(SIdxObj *pIdx) {
  terrno = TSDB_CODE_OUT_OF_MEMORY;

  // int32_t size =
  //     sizeof(SSmaObj) + pSma->exprLen + pSma->tagsFilterLen + pSma->sqlLen + pSma->astLen + TSDB_IDX_RESERVE_SIZE;
  int32_t size = sizeof(SIdxObj) + TSDB_IDX_RESERVE_SIZE;

  SSdbRaw *pRaw = sdbAllocRaw(SDB_IDX, TSDB_IDX_VER_NUMBER, size);
  if (pRaw == NULL) goto _OVER;

  int32_t dataPos = 0;
  SDB_SET_BINARY(pRaw, dataPos, pIdx->name, TSDB_TABLE_FNAME_LEN, _OVER)
  SDB_SET_BINARY(pRaw, dataPos, pIdx->stb, TSDB_TABLE_FNAME_LEN, _OVER)
  SDB_SET_BINARY(pRaw, dataPos, pIdx->db, TSDB_DB_FNAME_LEN, _OVER)
  SDB_SET_BINARY(pRaw, dataPos, pIdx->dstTbName, TSDB_DB_FNAME_LEN, _OVER)
  SDB_SET_BINARY(pRaw, dataPos, pIdx->colName, TSDB_COL_NAME_LEN, _OVER)
  SDB_SET_INT64(pRaw, dataPos, pIdx->createdTime, _OVER)
  SDB_SET_INT64(pRaw, dataPos, pIdx->uid, _OVER)
  SDB_SET_INT64(pRaw, dataPos, pIdx->stbUid, _OVER)
  SDB_SET_INT64(pRaw, dataPos, pIdx->dbUid, _OVER)

  SDB_SET_RESERVE(pRaw, dataPos, TSDB_IDX_RESERVE_SIZE, _OVER)
  SDB_SET_DATALEN(pRaw, dataPos, _OVER)

  terrno = 0;

_OVER:
  if (terrno != 0) {
    mError("idx:%s, failed to encode to raw:%p since %s", pIdx->name, pRaw, terrstr());
    sdbFreeRaw(pRaw);
    return NULL;
  }

  mTrace("idx:%s, encode to raw:%p, row:%p", pIdx->name, pRaw, pIdx);
  return pRaw;
}

static SSdbRow *mndIdxActionDecode(SSdbRaw *pRaw) {
  terrno = TSDB_CODE_OUT_OF_MEMORY;
  SSdbRow *pRow = NULL;
  SIdxObj *pIdx = NULL;

  int8_t sver = 0;
  if (sdbGetRawSoftVer(pRaw, &sver) != 0) goto _OVER;

  if (sver != TSDB_IDX_VER_NUMBER) {
    terrno = TSDB_CODE_SDB_INVALID_DATA_VER;
    goto _OVER;
  }

  pRow = sdbAllocRow(sizeof(SIdxObj));
  if (pRow == NULL) goto _OVER;

  pIdx = sdbGetRowObj(pRow);
  if (pIdx == NULL) goto _OVER;

  int32_t dataPos = 0;

  SDB_GET_BINARY(pRaw, dataPos, pIdx->name, TSDB_TABLE_FNAME_LEN, _OVER)
  SDB_GET_BINARY(pRaw, dataPos, pIdx->stb, TSDB_TABLE_FNAME_LEN, _OVER)
  SDB_GET_BINARY(pRaw, dataPos, pIdx->db, TSDB_DB_FNAME_LEN, _OVER)
  SDB_GET_BINARY(pRaw, dataPos, pIdx->dstTbName, TSDB_DB_FNAME_LEN, _OVER)
  SDB_GET_BINARY(pRaw, dataPos, pIdx->colName, TSDB_COL_NAME_LEN, _OVER)
  SDB_GET_INT64(pRaw, dataPos, &pIdx->createdTime, _OVER)
  SDB_GET_INT64(pRaw, dataPos, &pIdx->uid, _OVER)
  SDB_GET_INT64(pRaw, dataPos, &pIdx->stbUid, _OVER)
  SDB_GET_INT64(pRaw, dataPos, &pIdx->dbUid, _OVER)

  SDB_GET_RESERVE(pRaw, dataPos, TSDB_IDX_RESERVE_SIZE, _OVER)

  terrno = 0;

_OVER:
  if (terrno != 0) {
    taosMemoryFreeClear(pRow);
    return NULL;
  }

  mTrace("sma:%s, decode from raw:%p, row:%p", pIdx->name, pRaw, pIdx);
  return pRow;
}

static int32_t mndIdxActionInsert(SSdb *pSdb, SIdxObj *pIdx) {
  mTrace("idx:%s, perform insert action, row:%p", pIdx->name, pIdx);
  return 0;
}

static int32_t mndIdxActionDelete(SSdb *pSdb, SIdxObj *pIdx) {
  mTrace("sma:%s, perform delete action, row:%p", pIdx->name, pIdx);
  return 0;
}

static int32_t mndIdxActionUpdate(SSdb *pSdb, SIdxObj *pOld, SIdxObj *pNew) {
  mTrace("sma:%s, perform update action, old row:%p new row:%p", pOld->name, pOld, pNew);
  return 0;
}

SIdxObj *mndAcquireIdx(SMnode *pMnode, char *idxName) {
  SSdb    *pSdb = pMnode->pSdb;
  SIdxObj *pIdx = sdbAcquire(pSdb, SDB_IDX, idxName);
  if (pIdx == NULL && terrno == TSDB_CODE_SDB_OBJ_NOT_THERE) {
    terrno = TSDB_CODE_MND_TAG_INDEX_NOT_EXIST;
  }
  return pIdx;
}

void mndReleaseIdx(SMnode *pMnode, SIdxObj *pIdx) {
  SSdb *pSdb = pMnode->pSdb;
  sdbRelease(pSdb, pIdx);
}

SDbObj *mndAcquireDbByIdx(SMnode *pMnode, const char *idxName) {
  SName name = {0};
  tNameFromString(&name, idxName, T_NAME_ACCT | T_NAME_DB | T_NAME_TABLE);

  char db[TSDB_TABLE_FNAME_LEN] = {0};
  tNameGetFullDbName(&name, db);

  return mndAcquireDb(pMnode, db);
}

static int32_t mndSetCreateIdxRedoLogs(SMnode *pMnode, STrans *pTrans, SIdxObj *pIdx) {
  SSdbRaw *pRedoRaw = mndIdxActionEncode(pIdx);
  if (pRedoRaw == NULL) return -1;
  if (mndTransAppendRedolog(pTrans, pRedoRaw) != 0) return -1;
  if (sdbSetRawStatus(pRedoRaw, SDB_STATUS_CREATING) != 0) return -1;

  return 0;
}

static int32_t mndSetCreateIdxCommitLogs(SMnode *pMnode, STrans *pTrans, SIdxObj *pIdx) {
  SSdbRaw *pCommitRaw = mndIdxActionEncode(pIdx);
  if (pCommitRaw == NULL) return -1;
  if (mndTransAppendCommitlog(pTrans, pCommitRaw) != 0) return -1;
  if (sdbSetRawStatus(pCommitRaw, SDB_STATUS_READY) != 0) return -1;

  return 0;
}

static int32_t mndSetCreateIdxVgroupRedoLogs(SMnode *pMnode, STrans *pTrans, SVgObj *pVgroup) {
  SSdbRaw *pVgRaw = mndVgroupActionEncode(pVgroup);
  if (pVgRaw == NULL) return -1;
  if (mndTransAppendRedolog(pTrans, pVgRaw) != 0) return -1;
  if (sdbSetRawStatus(pVgRaw, SDB_STATUS_CREATING) != 0) return -1;
  return 0;
}

static int32_t mndSetCreateIdxVgroupCommitLogs(SMnode *pMnode, STrans *pTrans, SVgObj *pVgroup) {
  SSdbRaw *pVgRaw = mndVgroupActionEncode(pVgroup);
  if (pVgRaw == NULL) return -1;
  if (mndTransAppendCommitlog(pTrans, pVgRaw) != 0) return -1;
  if (sdbSetRawStatus(pVgRaw, SDB_STATUS_READY) != 0) return -1;
  return 0;
}

// static int32_t mndSetUpdateIdxStbCommitLogs(SMnode *pMnode, STrans *pTrans, SStbObj *pStb) {
//   SStbObj stbObj = {0};
//   taosRLockLatch(&pStb->lock);
//   memcpy(&stbObj, pStb, sizeof(SStbObj));
//   taosRUnLockLatch(&pStb->lock);
//   stbObj.numOfColumns = 0;
//   stbObj.pColumns = NULL;
//   stbObj.numOfTags = 0;
//  stbObj.pTags = NULL;
//   stbObj.numOfFuncs = 0;
//   stbObj.pFuncs = NULL;
//   stbObj.updateTime = taosGetTimestampMs();
//   stbObj.lock = 0;
//   stbObj.tagVer++;

// SSdbRaw *pCommitRaw = mndStbActionEncode(&stbObj);
// if (pCommitRaw == NULL) return -1;
// if (mndTransAppendCommitlog(pTrans, pCommitRaw) != 0) return -1;
// if (sdbSetRawStatus(pCommitRaw, SDB_STATUS_READY) != 0) return -1;
//
// return 0;
//}

static void mndDestroyIdxObj(SIdxObj *pIdxObj) {
  if (pIdxObj) {
    // do nothing
  }
}

static int32_t mndProcessCreateIdxReq(SRpcMsg *pReq) {
  SMnode  *pMnode = pReq->info.node;
  int32_t  code = -1;
  SStbObj *pStb = NULL;
  SIdxObj *pIdx = NULL;

  SDbObj            *pDb = NULL;
  SCreateTagIndexReq createReq = {0};

  if (tDeserializeSCreateTagIdxReq(pReq->pCont, pReq->contLen, &createReq) != 0) {
    terrno = TSDB_CODE_INVALID_MSG;
    goto _OVER;
  }

  mInfo("idx:%s start to create", createReq.idxName);
  // if (mndCheckCreateIdxReq(&createReq) != 0) {
  //   goto _OVER;
  // }

  pDb = mndAcquireDbByStb(pMnode, createReq.stbName);
  if (pDb == NULL) {
    terrno = TSDB_CODE_MND_INVALID_DB;
    goto _OVER;
  }

  pStb = mndAcquireStb(pMnode, createReq.stbName);
  if (pStb == NULL) {
    mError("idx:%s, failed to create since stb:%s not exist", createReq.idxName, createReq.stbName);
    goto _OVER;
  }

  pIdx = mndAcquireIdx(pMnode, createReq.idxName);
  if (pIdx != NULL) {
    terrno = TSDB_CODE_MND_SMA_ALREADY_EXIST;
    goto _OVER;
  }

  if (mndCheckDbPrivilege(pMnode, pReq->info.conn.user, MND_OPER_WRITE_DB, pDb) != 0) {
    goto _OVER;
  }

  code = mndAddIndex(pMnode, pReq, &createReq, pDb, pStb);
  if (terrno == TSDB_CODE_MND_TAG_INDEX_ALREADY_EXIST || terrno == TSDB_CODE_MND_TAG_NOT_EXIST) {
    return terrno;
  } else {
    if (code == 0) code = TSDB_CODE_ACTION_IN_PROGRESS;
  }

_OVER:
  if (code != 0 && code != TSDB_CODE_ACTION_IN_PROGRESS) {
    mError("stb:%s, failed to create since %s", createReq.idxName, terrstr());
  }

  mndReleaseStb(pMnode, pStb);
  mndReleaseIdx(pMnode, pIdx);
  mndReleaseDb(pMnode, pDb);

  return code;
}

static int32_t mndSetDropIdxRedoLogs(SMnode *pMnode, STrans *pTrans, SIdxObj *pIdx) {
  SSdbRaw *pRedoRaw = mndIdxActionEncode(pIdx);
  if (pRedoRaw == NULL) return -1;
  if (mndTransAppendRedolog(pTrans, pRedoRaw) != 0) return -1;
  if (sdbSetRawStatus(pRedoRaw, SDB_STATUS_DROPPING) != 0) return -1;

  return 0;
}

static int32_t mndSetDropIdxCommitLogs(SMnode *pMnode, STrans *pTrans, SIdxObj *pIdx) {
  SSdbRaw *pCommitRaw = mndIdxActionEncode(pIdx);
  if (pCommitRaw == NULL) return -1;
  if (mndTransAppendCommitlog(pTrans, pCommitRaw) != 0) return -1;
  if (sdbSetRawStatus(pCommitRaw, SDB_STATUS_DROPPED) != 0) return -1;

  return 0;
}

int32_t mndDropIdxsByStb(SMnode *pMnode, STrans *pTrans, SDbObj *pDb, SStbObj *pStb) {
  // stb
  return 0;
}

int32_t mndDropIdxsByDb(SMnode *pMnode, STrans *pTrans, SDbObj *pDb) {
  // by db name
  return 0;
}

/*static int32_t mndGetIdx(SMnode *pMnode, SUserIndexReq *indexReq, SUserIndexRsp *rsp, bool *exist) {
  int32_t  code = -1;
  SSmaObj *pSma = NULL;

  pSma = mndAcquireIdx(pMnode, indexReq->indexFName);
  if (pSma == NULL) {
    *exist = false;
    return 0;
  }

  memcpy(rsp->dbFName, pSma->db, sizeof(pSma->db));
  memcpy(rsp->tblFName, pSma->stb, sizeof(pSma->stb));
  strcpy(rsp->indexType, TSDB_INDEX_TYPE_SMA);

  SNodeList *pList = NULL;
  int32_t    extOffset = 0;
  code = nodesStringToList(pSma->expr, &pList);
  if (0 == code) {
    SNode *node = NULL;
    FOREACH(node, pList) {
      SFunctionNode *pFunc = (SFunctionNode *)node;
      extOffset += snprintf(rsp->indexExts + extOffset, sizeof(rsp->indexExts) - extOffset - 1, "%s%s",
                            (extOffset ? "," : ""), pFunc->functionName);
    }

    *exist = true;
  }

  mndReleaseIdx(pMnode, pSma);
  return code;
}

/*int32_t mndGetTableIdx(SMnode *pMnode, char *tbFName, STableIndexRsp *rsp, bool *exist) {
  int32_t         code = 0;
  SSmaObj        *pSma = NULL;
  SSdb           *pSdb = pMnode->pSdb;
  void           *pIter = NULL;
  STableIndexInfo info;

  SStbObj *pStb = mndAcquireStb(pMnode, tbFName);
  if (NULL == pStb) {
    *exist = false;
    return TSDB_CODE_SUCCESS;
  }

  strcpy(rsp->dbFName, pStb->db);
  strcpy(rsp->tbName, pStb->name + strlen(pStb->db) + 1);
  rsp->suid = pStb->uid;
  rsp->version = pStb->smaVer;
  mndReleaseStb(pMnode, pStb);

  while (1) {
    pIter = sdbFetch(pSdb, SDB_SMA, pIter, (void **)&pSma);
    if (pIter == NULL) break;

    if (pSma->stb[0] != tbFName[0] || strcmp(pSma->stb, tbFName)) {
      continue;
    }

    info.intervalUnit = pSma->intervalUnit;
    info.slidingUnit = pSma->slidingUnit;
    info.interval = pSma->interval;
    info.offset = pSma->offset;
    info.sliding = pSma->sliding;
    info.dstTbUid = pSma->dstTbUid;
    info.dstVgId = pSma->dstVgId;

    SVgObj *pVg = mndAcquireVgroup(pMnode, pSma->dstVgId);
    if (pVg == NULL) {
      code = -1;
      sdbRelease(pSdb, pSma);
      return code;
    }
    info.epSet = mndGetVgroupEpset(pMnode, pVg);

    info.expr = taosMemoryMalloc(pSma->exprLen + 1);
    if (info.expr == NULL) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      code = -1;
      sdbRelease(pSdb, pSma);
      return code;
    }

    memcpy(info.expr, pSma->expr, pSma->exprLen);
    info.expr[pSma->exprLen] = 0;

    if (NULL == taosArrayPush(rsp->pIndex, &info)) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      code = -1;
      taosMemoryFree(info.expr);
      sdbRelease(pSdb, pSma);
      return code;
    }

    *exist = true;

    sdbRelease(pSdb, pSma);
  }

  return code;
}*/

/*static int32_t mndProcessGetIdxReq(SRpcMsg *pReq) {
  SUserIndexReq indexReq = {0};
  SMnode       *pMnode = pReq->info.node;
  int32_t       code = -1;
  SUserIndexRsp rsp = {0};
  bool          exist = false;

  if (tDeserializeSUserIndexReq(pReq->pCont, pReq->contLen, &indexReq) != 0) {
    terrno = TSDB_CODE_INVALID_MSG;
    goto _OVER;
  }

  code = mndGetIdx(pMnode, &indexReq, &rsp, &exist);
  if (code) {
    goto _OVER;
  }

  if (!exist) {
    // TODO GET INDEX FROM FULLTEXT
    code = -1;
    terrno = TSDB_CODE_MND_DB_INDEX_NOT_EXIST;
  } else {
    int32_t contLen = tSerializeSUserIndexRsp(NULL, 0, &rsp);
    void   *pRsp = rpcMallocCont(contLen);
    if (pRsp == NULL) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      code = -1;
      goto _OVER;
    }

    tSerializeSUserIndexRsp(pRsp, contLen, &rsp);

    pReq->info.rsp = pRsp;
    pReq->info.rspLen = contLen;

    code = 0;
  }

_OVER:
  if (code != 0) {
    mError("failed to get index %s since %s", indexReq.indexFName, terrstr());
  }

  return code;
}*/

static int32_t mndProcessGetTbIdxReq(SRpcMsg *pReq) {
  //
  return 0;
}

int32_t mndRetrieveTagIdx(SRpcMsg *pReq, SShowObj *pShow, SSDataBlock *pBlock, int32_t rows) {
  SMnode  *pMnode = pReq->info.node;
  SSdb    *pSdb = pMnode->pSdb;
  int32_t  numOfRows = 0;
  SIdxObj *pIdx = NULL;
  int32_t  cols = 0;

  SDbObj *pDb = NULL;
  if (strlen(pShow->db) > 0) {
    pDb = mndAcquireDb(pMnode, pShow->db);
    if (pDb == NULL) return 0;
  }
  int invalid = -1;
  while (numOfRows < rows) {
    pShow->pIter = sdbFetch(pSdb, SDB_IDX, pShow->pIter, (void **)&pIdx);
    if (pShow->pIter == NULL) break;

    if (NULL != pDb && pIdx->dbUid != pDb->uid) {
      sdbRelease(pSdb, pIdx);
      continue;
    }

    cols = 0;

    SName idxName = {0};
    tNameFromString(&idxName, pIdx->name, T_NAME_ACCT | T_NAME_DB | T_NAME_TABLE);
    char n1[TSDB_TABLE_FNAME_LEN + VARSTR_HEADER_SIZE] = {0};

    STR_TO_VARSTR(n1, (char *)tNameGetTableName(&idxName));

    char n2[TSDB_DB_FNAME_LEN + VARSTR_HEADER_SIZE] = {0};
    STR_TO_VARSTR(n2, (char *)mndGetDbStr(pIdx->db));

    SName stbName = {0};
    tNameFromString(&stbName, pIdx->stb, T_NAME_ACCT | T_NAME_DB | T_NAME_TABLE);
    char n3[TSDB_TABLE_FNAME_LEN + VARSTR_HEADER_SIZE] = {0};
    STR_TO_VARSTR(n3, (char *)tNameGetTableName(&stbName));

    SColumnInfoData *pColInfo = taosArrayGet(pBlock->pDataBlock, cols++);
    colDataAppend(pColInfo, numOfRows, (const char *)n1, false);

    pColInfo = taosArrayGet(pBlock->pDataBlock, cols++);
    colDataAppend(pColInfo, numOfRows, (const char *)n2, false);

    pColInfo = taosArrayGet(pBlock->pDataBlock, cols++);
    colDataAppend(pColInfo, numOfRows, (const char *)n3, false);

    pColInfo = taosArrayGet(pBlock->pDataBlock, cols++);

    colDataAppend(pColInfo, numOfRows, (const char *)&invalid, false);

    pColInfo = taosArrayGet(pBlock->pDataBlock, cols++);
    colDataAppend(pColInfo, numOfRows, (const char *)&pIdx->createdTime, false);

    char col[TSDB_TABLE_FNAME_LEN + VARSTR_HEADER_SIZE] = {0};
    STR_TO_VARSTR(col, (char *)pIdx->colName);

    pColInfo = taosArrayGet(pBlock->pDataBlock, cols++);
    colDataAppend(pColInfo, numOfRows, (const char *)col, false);

    pColInfo = taosArrayGet(pBlock->pDataBlock, cols++);

    char tag[TSDB_TABLE_FNAME_LEN + VARSTR_HEADER_SIZE] = {0};
    STR_TO_VARSTR(tag, (char *)"tag_index");
    colDataAppend(pColInfo, numOfRows, (const char *)tag, false);

    numOfRows++;
    sdbRelease(pSdb, pIdx);
  }

  mndReleaseDb(pMnode, pDb);
  pShow->numOfRows += numOfRows;
  return numOfRows;
}

static void mndCancelGetNextIdx(SMnode *pMnode, void *pIter) {
  SSdb *pSdb = pMnode->pSdb;
  sdbCancelFetch(pSdb, pIter);
}
static int32_t mndCheckIndexReq(SCreateTagIndexReq *pReq) {
  // impl
  return TSDB_CODE_SUCCESS;
}

static int32_t mndSetUpdateIdxStbCommitLogs(SMnode *pMnode, STrans *pTrans, SStbObj *pOld, SStbObj *pNew,
                                            char *tagName) {
  taosRLockLatch(&pOld->lock);
  memcpy(pNew, pOld, sizeof(SStbObj));
  taosRUnLockLatch(&pOld->lock);

  // pNew->numOfColumns = 0;
  // pNew->pColumns = NULL;
  // pNew->numOfTags = 0;
  pNew->pTags = NULL;
  // pNew->numOfFuncs = 0;
  // pNew->pFuncs = NULL;
  pNew->updateTime = taosGetTimestampMs();
  pNew->lock = 0;

  int32_t tag = mndFindSuperTableTagId(pOld, tagName);
  if (tag < 0) {
    terrno = TSDB_CODE_MND_TAG_NOT_EXIST;
    return -1;
  }
  col_id_t colId = pOld->pTags[tag].colId;
  if (mndCheckColAndTagModifiable(pMnode, pOld->name, pOld->uid, colId) != 0) {
    return -1;
  }
  if (mndAllocStbSchemas(pOld, pNew) != 0) {
    return -1;
  }

  SSchema *pTag = pNew->pTags + tag;
  if (IS_IDX_ON(pTag)) {
    terrno = TSDB_CODE_MND_TAG_INDEX_ALREADY_EXIST;
    return -1;
  } else {
    pTag->flags |= COL_IDX_ON;
  }
  pNew->tagVer++;

  SSdbRaw *pCommitRaw = mndStbActionEncode(pNew);
  if (pCommitRaw == NULL) return -1;
  if (mndTransAppendCommitlog(pTrans, pCommitRaw) != 0) return -1;
  if (sdbSetRawStatus(pCommitRaw, SDB_STATUS_READY) != 0) return -1;

  return 0;
}
int32_t mndAddIndexImpl(SMnode *pMnode, SRpcMsg *pReq, SDbObj *pDb, SStbObj *pStb, SIdxObj *pIdx) {
  // impl later
  int32_t code = 0;
  SStbObj newStb = {0};
  STrans *pTrans = mndTransCreate(pMnode, TRN_POLICY_RETRY, TRN_CONFLICT_DB_INSIDE, pReq, "create-stb-index");
  if (pTrans == NULL) goto _OVER;

  // mInfo("trans:%d, used to add index to stb:%s", pTrans->id, pStb->name);
  mndTransSetDbName(pTrans, pDb->name, pStb->name);
  if (mndTrancCheckConflict(pMnode, pTrans) != 0) goto _OVER;

  mndTransSetSerial(pTrans);

  if (mndSetCreateIdxRedoLogs(pMnode, pTrans, pIdx) != 0) goto _OVER;
  if (mndSetCreateIdxCommitLogs(pMnode, pTrans, pIdx) != 0) goto _OVER;

  if (mndSetUpdateIdxStbCommitLogs(pMnode, pTrans, pStb, &newStb, pIdx->colName) != 0) goto _OVER;
  if (mndSetCreateIdxRedoActions(pMnode, pTrans, pDb, &newStb, pIdx) != 0) goto _OVER;

  // if (mndSetAlterStbRedoLogs(pMnode, pTrans, pDb, pStb) != 0) goto _OVER;
  // if (mndSetAlterStbCommitLogs(pMnode, pTrans, pDb, pStb) != 0) goto _OVER;
  // if (mndSetAlterStbRedoActions2(pMnode, pTrans, pDb, pStb, sql, len) != 0) goto _OVER;
  if (mndTransPrepare(pMnode, pTrans) != 0) goto _OVER;

  return code;

_OVER:
  // mndDestoryIdxObj(pIdx);
  mndTransDrop(pTrans);
  return code;
}

static int32_t mndAddIndex(SMnode *pMnode, SRpcMsg *pReq, SCreateTagIndexReq *req, SDbObj *pDb, SStbObj *pStb) {
  SIdxObj idxObj = {0};
  memcpy(idxObj.name, req->idxName, TSDB_TABLE_FNAME_LEN);
  memcpy(idxObj.stb, pStb->name, TSDB_TABLE_FNAME_LEN);
  memcpy(idxObj.db, pDb->name, TSDB_DB_FNAME_LEN);
  memcpy(idxObj.colName, req->colName, TSDB_COL_NAME_LEN);

  idxObj.createdTime = taosGetTimestampMs();
  idxObj.uid = mndGenerateUid(req->idxName, TSDB_TABLE_FNAME_LEN);
  idxObj.stbUid = pStb->uid;
  idxObj.dbUid = pStb->dbUid;

  int32_t code = -1;
  // SField *pField0 = NULL;

  // SStbObj  stbObj = {0};
  // SStbObj *pNew = &stbObj;

  // taosRLockLatch(&pOld->lock);
  // memcpy(&stbObj, pOld, sizeof(SStbObj));
  // taosRUnLockLatch(&pOld->lock);

  // stbObj.pColumns = NULL;
  // stbObj.pTags = NULL;
  // stbObj.updateTime = taosGetTimestampMs();
  // stbObj.lock = 0;

  int32_t tag = mndFindSuperTableTagId(pStb, req->colName);
  if (tag < 0) {
    terrno = TSDB_CODE_MND_TAG_NOT_EXIST;
    return -1;
  }
  col_id_t colId = pStb->pTags[tag].colId;
  if (mndCheckColAndTagModifiable(pMnode, pStb->name, pStb->uid, colId) != 0) {
    return -1;
  }

  SSchema *pTag = pStb->pTags + tag;
  if (IS_IDX_ON(pTag)) {
    terrno = TSDB_CODE_MND_TAG_INDEX_ALREADY_EXIST;
    return -1;
  }
  code = mndAddIndexImpl(pMnode, pReq, pDb, pStb, &idxObj);

  return code;
}

static int32_t mndDropIdx(SMnode *pMnode, SRpcMsg *pReq, SDbObj *pDb, SIdxObj *pIdx) {
  int32_t  code = -1;
  SStbObj *pStb = NULL;
  STrans  *pTrans = NULL;

  SStbObj newObj = {0};

  pStb = mndAcquireStb(pMnode, pIdx->stb);
  if (pStb == NULL) goto _OVER;

  pTrans = mndTransCreate(pMnode, TRN_POLICY_RETRY, TRN_CONFLICT_DB, pReq, "drop-index");
  if (pTrans == NULL) goto _OVER;

  mInfo("trans:%d, used to drop idx:%s", pTrans->id, pIdx->name);
  mndTransSetDbName(pTrans, pDb->name, NULL);
  if (mndTrancCheckConflict(pMnode, pTrans) != 0) goto _OVER;

  mndTransSetSerial(pTrans);
  if (mndSetDropIdxRedoLogs(pMnode, pTrans, pIdx) != 0) goto _OVER;
  if (mndSetDropIdxCommitLogs(pMnode, pTrans, pIdx) != 0) goto _OVER;

  if (mndSetUpdateIdxStbCommitLogs(pMnode, pTrans, pStb, &newObj, pIdx->colName) != 0) goto _OVER;
  if (mndSetDropIdxRedoActions(pMnode, pTrans, pDb, &newObj, pIdx) != 0) goto _OVER;

_OVER:
  mndTransDrop(pTrans);
  mndReleaseStb(pMnode, pStb);
  return code;
}
static int32_t mndProcessDropIdxReq(SRpcMsg *pReq) {
  SMnode  *pMnode = pReq->info.node;
  int32_t  code = -1;
  SDbObj  *pDb = NULL;
  SIdxObj *pIdx = NULL;

  SDropTagIndexReq req = {0};
  if (tDeserializeSDropTagIdxReq(pReq->pCont, pReq->contLen, &req) != 0) {
    terrno = TSDB_CODE_INVALID_MSG;
    goto _OVER;
  }
  mInfo("idx:%s, start to drop", req.name);

  pIdx = mndAcquireIdx(pMnode, req.name);
  if (pIdx == NULL) {
    if (req.igNotExists) {
      mInfo("idx:%s, not exist, ignore not exist is set", req.name);
      code = 0;
      goto _OVER;
    } else {
      terrno = TSDB_CODE_MND_SMA_NOT_EXIST;
      goto _OVER;
    }
  }

  pDb = mndAcquireDbByIdx(pMnode, req.name);
  if (pDb == NULL) {
    terrno = TSDB_CODE_MND_DB_NOT_SELECTED;
    goto _OVER;
  }

  if (mndCheckDbPrivilege(pMnode, pReq->info.conn.user, MND_OPER_WRITE_DB, pDb) != 0) {
    goto _OVER;
  }

  code = mndDropIdx(pMnode, pReq, pDb, pIdx);
  if (code == 0) code = TSDB_CODE_ACTION_IN_PROGRESS;
  return code;

_OVER:
  if (code != 0 && code != TSDB_CODE_ACTION_IN_PROGRESS) {
    mError("idx:%s, failed to drop since %s", req.name, terrstr());
  }
  return code;
}
static int32_t mndProcessGetIdxReq(SRpcMsg *pReq) {
  // do nothing
  return 0;
}
