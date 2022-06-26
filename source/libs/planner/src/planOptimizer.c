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

#include "filter.h"
#include "functionMgt.h"
#include "planInt.h"
#include "ttime.h"

#define OPTIMIZE_FLAG_MASK(n) (1 << n)

#define OPTIMIZE_FLAG_OSD OPTIMIZE_FLAG_MASK(0)
#define OPTIMIZE_FLAG_CPD OPTIMIZE_FLAG_MASK(1)
#define OPTIMIZE_FLAG_OPK OPTIMIZE_FLAG_MASK(2)

#define OPTIMIZE_FLAG_SET_MASK(val, mask)  (val) |= (mask)
#define OPTIMIZE_FLAG_TEST_MASK(val, mask) (((val) & (mask)) != 0)

typedef struct SOptimizeContext {
  SPlanContext* pPlanCxt;
  bool          optimized;
} SOptimizeContext;

typedef int32_t (*FOptimize)(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan);

typedef struct SOptimizeRule {
  char*     pName;
  FOptimize optimizeFunc;
} SOptimizeRule;

typedef struct SOsdInfo {
  SScanLogicNode* pScan;
  SNodeList*      pSdrFuncs;
  SNodeList*      pDsoFuncs;
} SOsdInfo;

typedef struct SCpdIsMultiTableCondCxt {
  SNodeList* pLeftCols;
  SNodeList* pRightCols;
  bool       havaLeftCol;
  bool       haveRightCol;
} SCpdIsMultiTableCondCxt;

typedef enum ECondAction {
  COND_ACTION_STAY = 1,
  COND_ACTION_PUSH_JOIN,
  COND_ACTION_PUSH_LEFT_CHILD,
  COND_ACTION_PUSH_RIGHT_CHILD
  // after supporting outer join, there are other possibilities
} ECondAction;

typedef bool (*FMayBeOptimized)(SLogicNode* pNode);

static SLogicNode* optFindPossibleNode(SLogicNode* pNode, FMayBeOptimized func) {
  if (func(pNode)) {
    return pNode;
  }
  SNode* pChild;
  FOREACH(pChild, pNode->pChildren) {
    SLogicNode* pScanNode = optFindPossibleNode((SLogicNode*)pChild, func);
    if (NULL != pScanNode) {
      return pScanNode;
    }
  }
  return NULL;
}

EDealRes osdHaveNormalColImpl(SNode* pNode, void* pContext) {
  if (QUERY_NODE_COLUMN == nodeType(pNode)) {
    // *((bool*)pContext) = (COLUMN_TYPE_TAG != ((SColumnNode*)pNode)->colType);
    *((bool*)pContext) = true;
    return *((bool*)pContext) ? DEAL_RES_END : DEAL_RES_IGNORE_CHILD;
  }
  return DEAL_RES_CONTINUE;
}

static bool osdHaveNormalCol(SNodeList* pList) {
  bool res = false;
  nodesWalkExprsPostOrder(pList, osdHaveNormalColImpl, &res);
  return res;
}

static bool osdMayBeOptimized(SLogicNode* pNode) {
  if (OPTIMIZE_FLAG_TEST_MASK(pNode->optimizedFlag, OPTIMIZE_FLAG_OSD)) {
    return false;
  }
  if (QUERY_NODE_LOGIC_PLAN_SCAN != nodeType(pNode)) {
    return false;
  }
  if (NULL == pNode->pParent || (QUERY_NODE_LOGIC_PLAN_WINDOW != nodeType(pNode->pParent) &&
                                 QUERY_NODE_LOGIC_PLAN_AGG != nodeType(pNode->pParent) &&
                                 QUERY_NODE_LOGIC_PLAN_PARTITION != nodeType(pNode->pParent))) {
    return false;
  }
  if (QUERY_NODE_LOGIC_PLAN_WINDOW == nodeType(pNode->pParent) ||
      (QUERY_NODE_LOGIC_PLAN_PARTITION == nodeType(pNode->pParent) && pNode->pParent->pParent &&
       QUERY_NODE_LOGIC_PLAN_WINDOW == nodeType(pNode->pParent->pParent))) {
    return true;
  }
  return !osdHaveNormalCol(((SAggLogicNode*)pNode->pParent)->pGroupKeys);
}

static SNodeList* osdGetAllFuncs(SLogicNode* pNode) {
  switch (nodeType(pNode)) {
    case QUERY_NODE_LOGIC_PLAN_WINDOW:
      return ((SWindowLogicNode*)pNode)->pFuncs;
    case QUERY_NODE_LOGIC_PLAN_AGG:
      return ((SAggLogicNode*)pNode)->pAggFuncs;
    default:
      break;
  }
  return NULL;
}

static bool needOptimizeDataRequire(const SFunctionNode* pFunc) {
  if (!fmIsSpecialDataRequiredFunc(pFunc->funcId)) {
    return false;
  }
  SNode* pPara = NULL;
  FOREACH(pPara, pFunc->pParameterList) {
    if (QUERY_NODE_COLUMN != nodeType(pPara) && QUERY_NODE_VALUE != nodeType(pPara)) {
      return false;
    }
  }
  return true;
}

static bool needOptimizeDynamicScan(const SFunctionNode* pFunc) {
  if (!fmIsDynamicScanOptimizedFunc(pFunc->funcId)) {
    return false;
  }
  SNode* pPara = NULL;
  FOREACH(pPara, pFunc->pParameterList) {
    if (QUERY_NODE_COLUMN != nodeType(pPara) && QUERY_NODE_VALUE != nodeType(pPara)) {
      return false;
    }
  }
  return true;
}

static int32_t osdGetRelatedFuncs(SScanLogicNode* pScan, SNodeList** pSdrFuncs, SNodeList** pDsoFuncs) {
  SNodeList* pAllFuncs = osdGetAllFuncs(pScan->node.pParent);
  SNodeList* pTmpSdrFuncs = NULL;
  SNodeList* pTmpDsoFuncs = NULL;
  SNode*     pFunc = NULL;
  bool       otherFunc = false;
  FOREACH(pFunc, pAllFuncs) {
    int32_t code = TSDB_CODE_SUCCESS;
    if (needOptimizeDataRequire((SFunctionNode*)pFunc)) {
      code = nodesListMakeStrictAppend(&pTmpSdrFuncs, nodesCloneNode(pFunc));
    } else if (needOptimizeDynamicScan((SFunctionNode*)pFunc)) {
      code = nodesListMakeStrictAppend(&pTmpDsoFuncs, nodesCloneNode(pFunc));
    } else {
      otherFunc = true;
    }
    if (TSDB_CODE_SUCCESS != code) {
      nodesDestroyList(pTmpSdrFuncs);
      nodesDestroyList(pTmpDsoFuncs);
      return code;
    }
  }
  if (otherFunc) {
    nodesDestroyList(pTmpSdrFuncs);
    nodesDestroyList(pTmpDsoFuncs);
  } else {
    *pSdrFuncs = pTmpSdrFuncs;
    *pDsoFuncs = pTmpDsoFuncs;
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t osdMatch(SOptimizeContext* pCxt, SLogicNode* pLogicNode, SOsdInfo* pInfo) {
  pInfo->pScan = (SScanLogicNode*)optFindPossibleNode(pLogicNode, osdMayBeOptimized);
  if (NULL == pInfo->pScan) {
    return TSDB_CODE_SUCCESS;
  }
  return osdGetRelatedFuncs(pInfo->pScan, &pInfo->pSdrFuncs, &pInfo->pDsoFuncs);
}

static EFuncDataRequired osdPromoteDataRequired(EFuncDataRequired l, EFuncDataRequired r) {
  switch (l) {
    case FUNC_DATA_REQUIRED_DATA_LOAD:
      return l;
    case FUNC_DATA_REQUIRED_STATIS_LOAD:
      return FUNC_DATA_REQUIRED_DATA_LOAD == r ? r : l;
    case FUNC_DATA_REQUIRED_NOT_LOAD:
      return FUNC_DATA_REQUIRED_FILTEROUT == r ? l : r;
    default:
      break;
  }
  return r;
}

static int32_t osdGetDataRequired(SNodeList* pFuncs) {
  if (NULL == pFuncs) {
    return FUNC_DATA_REQUIRED_DATA_LOAD;
  }
  EFuncDataRequired dataRequired = FUNC_DATA_REQUIRED_FILTEROUT;
  SNode*            pFunc = NULL;
  FOREACH(pFunc, pFuncs) {
    dataRequired = osdPromoteDataRequired(dataRequired, fmFuncDataRequired((SFunctionNode*)pFunc, NULL));
  }
  return dataRequired;
}

static void setScanWindowInfo(SScanLogicNode* pScan) {
  SLogicNode* pParent = pScan->node.pParent;
  if (QUERY_NODE_LOGIC_PLAN_PARTITION == nodeType(pParent) && pParent->pParent &&
      QUERY_NODE_LOGIC_PLAN_WINDOW == nodeType(pParent->pParent)) {
    pParent = pParent->pParent;
  }
  if (QUERY_NODE_LOGIC_PLAN_WINDOW == nodeType(pParent)) {
    pScan->interval = ((SWindowLogicNode*)pParent)->interval;
    pScan->offset = ((SWindowLogicNode*)pParent)->offset;
    pScan->sliding = ((SWindowLogicNode*)pParent)->sliding;
    pScan->intervalUnit = ((SWindowLogicNode*)pParent)->intervalUnit;
    pScan->slidingUnit = ((SWindowLogicNode*)pParent)->slidingUnit;
    pScan->triggerType = ((SWindowLogicNode*)pParent)->triggerType;
    pScan->watermark = ((SWindowLogicNode*)pParent)->watermark;
    pScan->tsColId = ((SColumnNode*)((SWindowLogicNode*)pParent)->pTspk)->colId;
    pScan->filesFactor = ((SWindowLogicNode*)pParent)->filesFactor;
  }
}

static int32_t pickScanPathOptimize(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan) {
  SOsdInfo info = {0};
  int32_t  code = osdMatch(pCxt, pLogicSubplan->pNode, &info);
  if (TSDB_CODE_SUCCESS == code && info.pScan) {
    setScanWindowInfo((SScanLogicNode*)info.pScan);
  }
  if (TSDB_CODE_SUCCESS == code && (NULL != info.pDsoFuncs || NULL != info.pSdrFuncs)) {
    info.pScan->dataRequired = osdGetDataRequired(info.pSdrFuncs);
    info.pScan->pDynamicScanFuncs = info.pDsoFuncs;
    OPTIMIZE_FLAG_SET_MASK(info.pScan->node.optimizedFlag, OPTIMIZE_FLAG_OSD);
    pCxt->optimized = true;
  }
  nodesDestroyList(info.pSdrFuncs);
  return code;
}

static int32_t pdcOptMergeCond(SNode** pDst, SNode** pSrc) {
  SLogicConditionNode* pLogicCond = (SLogicConditionNode*)nodesMakeNode(QUERY_NODE_LOGIC_CONDITION);
  if (NULL == pLogicCond) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  pLogicCond->node.resType.type = TSDB_DATA_TYPE_BOOL;
  pLogicCond->node.resType.bytes = tDataTypes[TSDB_DATA_TYPE_BOOL].bytes;
  pLogicCond->condType = LOGIC_COND_TYPE_AND;
  int32_t code = nodesListMakeAppend(&pLogicCond->pParameterList, *pSrc);
  if (TSDB_CODE_SUCCESS == code) {
    *pSrc = NULL;
    code = nodesListMakeAppend(&pLogicCond->pParameterList, *pDst);
  }
  if (TSDB_CODE_SUCCESS == code) {
    *pDst = (SNode*)pLogicCond;
  } else {
    nodesDestroyNode((SNode*)pLogicCond);
  }
  return code;
}

static int32_t pdcOptAppendCond(SNode** pCond, SNode** pAdditionalCond) {
  if (NULL == *pCond) {
    TSWAP(*pCond, *pAdditionalCond);
    return TSDB_CODE_SUCCESS;
  }

  int32_t code = TSDB_CODE_SUCCESS;
  if (QUERY_NODE_LOGIC_CONDITION == nodeType(*pCond)) {
    code = nodesListAppend(((SLogicConditionNode*)*pCond)->pParameterList, *pAdditionalCond);
    if (TSDB_CODE_SUCCESS == code) {
      *pAdditionalCond = NULL;
    }
  } else {
    code = pdcOptMergeCond(pCond, pAdditionalCond);
  }
  return code;
}

static int32_t pdcOptCalcTimeRange(SOptimizeContext* pCxt, SScanLogicNode* pScan, SNode** pPrimaryKeyCond,
                                   SNode** pOtherCond) {
  int32_t code = TSDB_CODE_SUCCESS;
  if (pCxt->pPlanCxt->topicQuery || pCxt->pPlanCxt->streamQuery) {
    code = pdcOptAppendCond(pOtherCond, pPrimaryKeyCond);
  } else {
    bool isStrict = false;
    code = filterGetTimeRange(*pPrimaryKeyCond, &pScan->scanRange, &isStrict);
    if (TSDB_CODE_SUCCESS == code) {
      if (isStrict) {
        nodesDestroyNode(*pPrimaryKeyCond);
      } else {
        code = pdcOptAppendCond(pOtherCond, pPrimaryKeyCond);
      }
      *pPrimaryKeyCond = NULL;
    }
  }
  return code;
}

static int32_t pdcOptDealScan(SOptimizeContext* pCxt, SScanLogicNode* pScan) {
  if (NULL == pScan->node.pConditions || OPTIMIZE_FLAG_TEST_MASK(pScan->node.optimizedFlag, OPTIMIZE_FLAG_CPD) ||
      TSDB_SYSTEM_TABLE == pScan->tableType) {
    return TSDB_CODE_SUCCESS;
  }

  SNode*  pPrimaryKeyCond = NULL;
  SNode*  pOtherCond = NULL;
  int32_t code = nodesPartitionCond(&pScan->node.pConditions, &pPrimaryKeyCond, &pScan->pTagIndexCond, &pScan->pTagCond,
                                    &pOtherCond);
  if (TSDB_CODE_SUCCESS == code && NULL != pPrimaryKeyCond) {
    code = pdcOptCalcTimeRange(pCxt, pScan, &pPrimaryKeyCond, &pOtherCond);
  }
  if (TSDB_CODE_SUCCESS == code) {
    pScan->node.pConditions = pOtherCond;
  }

  if (TSDB_CODE_SUCCESS == code) {
    OPTIMIZE_FLAG_SET_MASK(pScan->node.optimizedFlag, OPTIMIZE_FLAG_CPD);
    pCxt->optimized = true;
  } else {
    nodesDestroyNode(pPrimaryKeyCond);
    nodesDestroyNode(pOtherCond);
  }

  return code;
}

static bool pdcOptBelongThisTable(SNode* pCondCol, SNodeList* pTableCols) {
  SNode* pTableCol = NULL;
  FOREACH(pTableCol, pTableCols) {
    if (nodesEqualNode(pCondCol, pTableCol)) {
      return true;
    }
  }
  return false;
}

static EDealRes pdcOptIsCrossTableCond(SNode* pNode, void* pContext) {
  SCpdIsMultiTableCondCxt* pCxt = pContext;
  if (QUERY_NODE_COLUMN == nodeType(pNode)) {
    if (pdcOptBelongThisTable(pNode, pCxt->pLeftCols)) {
      pCxt->havaLeftCol = true;
    } else if (pdcOptBelongThisTable(pNode, pCxt->pRightCols)) {
      pCxt->haveRightCol = true;
    }
    return pCxt->havaLeftCol && pCxt->haveRightCol ? DEAL_RES_END : DEAL_RES_CONTINUE;
  }
  return DEAL_RES_CONTINUE;
}

static ECondAction pdcOptGetCondAction(EJoinType joinType, SNodeList* pLeftCols, SNodeList* pRightCols, SNode* pNode) {
  SCpdIsMultiTableCondCxt cxt = {
      .pLeftCols = pLeftCols, .pRightCols = pRightCols, .havaLeftCol = false, .haveRightCol = false};
  nodesWalkExpr(pNode, pdcOptIsCrossTableCond, &cxt);
  return (JOIN_TYPE_INNER != joinType
              ? COND_ACTION_STAY
              : (cxt.havaLeftCol && cxt.haveRightCol
                     ? COND_ACTION_PUSH_JOIN
                     : (cxt.havaLeftCol ? COND_ACTION_PUSH_LEFT_CHILD : COND_ACTION_PUSH_RIGHT_CHILD)));
}

static int32_t pdcOptPartitionLogicCond(SJoinLogicNode* pJoin, SNode** pOnCond, SNode** pLeftChildCond,
                                        SNode** pRightChildCond) {
  SLogicConditionNode* pLogicCond = (SLogicConditionNode*)pJoin->node.pConditions;
  if (LOGIC_COND_TYPE_AND != pLogicCond->condType) {
    return TSDB_CODE_SUCCESS;
  }

  SNodeList* pLeftCols = ((SLogicNode*)nodesListGetNode(pJoin->node.pChildren, 0))->pTargets;
  SNodeList* pRightCols = ((SLogicNode*)nodesListGetNode(pJoin->node.pChildren, 1))->pTargets;
  int32_t    code = TSDB_CODE_SUCCESS;

  SNodeList* pOnConds = NULL;
  SNodeList* pLeftChildConds = NULL;
  SNodeList* pRightChildConds = NULL;
  SNodeList* pRemainConds = NULL;
  SNode*     pCond = NULL;
  FOREACH(pCond, pLogicCond->pParameterList) {
    ECondAction condAction = pdcOptGetCondAction(pJoin->joinType, pLeftCols, pRightCols, pCond);
    if (COND_ACTION_PUSH_JOIN == condAction) {
      code = nodesListMakeAppend(&pOnConds, nodesCloneNode(pCond));
    } else if (COND_ACTION_PUSH_LEFT_CHILD == condAction) {
      code = nodesListMakeAppend(&pLeftChildConds, nodesCloneNode(pCond));
    } else if (COND_ACTION_PUSH_RIGHT_CHILD == condAction) {
      code = nodesListMakeAppend(&pRightChildConds, nodesCloneNode(pCond));
    } else {
      code = nodesListMakeAppend(&pRemainConds, nodesCloneNode(pCond));
    }
    if (TSDB_CODE_SUCCESS != code) {
      break;
    }
  }

  SNode* pTempOnCond = NULL;
  SNode* pTempLeftChildCond = NULL;
  SNode* pTempRightChildCond = NULL;
  SNode* pTempRemainCond = NULL;
  if (TSDB_CODE_SUCCESS == code) {
    code = nodesMergeConds(&pTempOnCond, &pOnConds);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = nodesMergeConds(&pTempLeftChildCond, &pLeftChildConds);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = nodesMergeConds(&pTempRightChildCond, &pRightChildConds);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = nodesMergeConds(&pTempRemainCond, &pRemainConds);
  }

  if (TSDB_CODE_SUCCESS == code) {
    *pOnCond = pTempOnCond;
    *pLeftChildCond = pTempLeftChildCond;
    *pRightChildCond = pTempRightChildCond;
    nodesDestroyNode(pJoin->node.pConditions);
    pJoin->node.pConditions = pTempRemainCond;
  } else {
    nodesDestroyList(pOnConds);
    nodesDestroyList(pLeftChildConds);
    nodesDestroyList(pRightChildConds);
    nodesDestroyList(pRemainConds);
    nodesDestroyNode(pTempOnCond);
    nodesDestroyNode(pTempLeftChildCond);
    nodesDestroyNode(pTempRightChildCond);
    nodesDestroyNode(pTempRemainCond);
  }

  return code;
}

static int32_t pdcOptPartitionOpCond(SJoinLogicNode* pJoin, SNode** pOnCond, SNode** pLeftChildCond,
                                     SNode** pRightChildCond) {
  SNodeList*  pLeftCols = ((SLogicNode*)nodesListGetNode(pJoin->node.pChildren, 0))->pTargets;
  SNodeList*  pRightCols = ((SLogicNode*)nodesListGetNode(pJoin->node.pChildren, 1))->pTargets;
  ECondAction condAction = pdcOptGetCondAction(pJoin->joinType, pLeftCols, pRightCols, pJoin->node.pConditions);
  if (COND_ACTION_STAY == condAction) {
    return TSDB_CODE_SUCCESS;
  } else if (COND_ACTION_PUSH_JOIN == condAction) {
    *pOnCond = pJoin->node.pConditions;
  } else if (COND_ACTION_PUSH_LEFT_CHILD == condAction) {
    *pLeftChildCond = pJoin->node.pConditions;
  } else if (COND_ACTION_PUSH_RIGHT_CHILD == condAction) {
    *pRightChildCond = pJoin->node.pConditions;
  }
  pJoin->node.pConditions = NULL;
  return TSDB_CODE_SUCCESS;
}

static int32_t pdcOptPartitionCond(SJoinLogicNode* pJoin, SNode** pOnCond, SNode** pLeftChildCond,
                                   SNode** pRightChildCond) {
  if (QUERY_NODE_LOGIC_CONDITION == nodeType(pJoin->node.pConditions)) {
    return pdcOptPartitionLogicCond(pJoin, pOnCond, pLeftChildCond, pRightChildCond);
  } else {
    return pdcOptPartitionOpCond(pJoin, pOnCond, pLeftChildCond, pRightChildCond);
  }
}

static int32_t pdcOptPushCondToOnCond(SOptimizeContext* pCxt, SJoinLogicNode* pJoin, SNode** pCond) {
  return pdcOptAppendCond(&pJoin->pOnConditions, pCond);
}

static int32_t pdcOptPushCondToScan(SOptimizeContext* pCxt, SScanLogicNode* pScan, SNode** pCond) {
  return pdcOptAppendCond(&pScan->node.pConditions, pCond);
}

static int32_t pdcOptPushCondToChild(SOptimizeContext* pCxt, SLogicNode* pChild, SNode** pCond) {
  switch (nodeType(pChild)) {
    case QUERY_NODE_LOGIC_PLAN_SCAN:
      return pdcOptPushCondToScan(pCxt, (SScanLogicNode*)pChild, pCond);
    default:
      break;
  }
  planError("pdcOptPushCondToChild failed, invalid logic plan node %s", nodesNodeName(nodeType(pChild)));
  return TSDB_CODE_PLAN_INTERNAL_ERROR;
}

static bool pdcOptIsPrimaryKey(SNode* pNode, SNodeList* pTableCols) {
  if (QUERY_NODE_COLUMN != nodeType(pNode)) {
    return false;
  }
  SColumnNode* pCol = (SColumnNode*)pNode;
  if (PRIMARYKEY_TIMESTAMP_COL_ID != pCol->colId) {
    return false;
  }
  return pdcOptBelongThisTable(pNode, pTableCols);
}

static bool pdcOptIsPrimaryKeyEqualCond(SJoinLogicNode* pJoin, SNode* pCond) {
  if (QUERY_NODE_OPERATOR != nodeType(pCond)) {
    return false;
  }

  SOperatorNode* pOper = (SOperatorNode*)pCond;
  if (OP_TYPE_EQUAL != pOper->opType) {
    return false;
  }

  SNodeList* pLeftCols = ((SLogicNode*)nodesListGetNode(pJoin->node.pChildren, 0))->pTargets;
  SNodeList* pRightCols = ((SLogicNode*)nodesListGetNode(pJoin->node.pChildren, 1))->pTargets;
  if (pdcOptIsPrimaryKey(pOper->pLeft, pLeftCols)) {
    return pdcOptIsPrimaryKey(pOper->pRight, pRightCols);
  } else if (pdcOptIsPrimaryKey(pOper->pLeft, pRightCols)) {
    return pdcOptIsPrimaryKey(pOper->pRight, pLeftCols);
  }
  return false;
}

static bool pdcOptContainPriKeyEqualCond(SJoinLogicNode* pJoin, SNode* pCond) {
  if (QUERY_NODE_LOGIC_CONDITION == nodeType(pCond)) {
    SLogicConditionNode* pLogicCond = (SLogicConditionNode*)pCond;
    if (LOGIC_COND_TYPE_AND != pLogicCond->condType) {
      return false;
    }
    bool   hasPrimaryKeyEqualCond = false;
    SNode* pCond = NULL;
    FOREACH(pCond, pLogicCond->pParameterList) {
      if (pdcOptContainPriKeyEqualCond(pJoin, pCond)) {
        hasPrimaryKeyEqualCond = true;
        break;
      }
    }
    return hasPrimaryKeyEqualCond;
  } else {
    return pdcOptIsPrimaryKeyEqualCond(pJoin, pCond);
  }
}

static int32_t pdcOptCheckJoinOnCond(SOptimizeContext* pCxt, SJoinLogicNode* pJoin) {
  if (NULL == pJoin->pOnConditions) {
    return generateUsageErrMsg(pCxt->pPlanCxt->pMsg, pCxt->pPlanCxt->msgLen, TSDB_CODE_PLAN_NOT_SUPPORT_CROSS_JOIN);
  }
  if (!pdcOptContainPriKeyEqualCond(pJoin, pJoin->pOnConditions)) {
    return generateUsageErrMsg(pCxt->pPlanCxt->pMsg, pCxt->pPlanCxt->msgLen, TSDB_CODE_PLAN_EXPECTED_TS_EQUAL);
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t pdcOptDealJoin(SOptimizeContext* pCxt, SJoinLogicNode* pJoin) {
  if (OPTIMIZE_FLAG_TEST_MASK(pJoin->node.optimizedFlag, OPTIMIZE_FLAG_CPD)) {
    return TSDB_CODE_SUCCESS;
  }

  if (NULL == pJoin->node.pConditions) {
    return pdcOptCheckJoinOnCond(pCxt, pJoin);
  }

  SNode*  pOnCond = NULL;
  SNode*  pLeftChildCond = NULL;
  SNode*  pRightChildCond = NULL;
  int32_t code = pdcOptPartitionCond(pJoin, &pOnCond, &pLeftChildCond, &pRightChildCond);
  if (TSDB_CODE_SUCCESS == code && NULL != pOnCond) {
    code = pdcOptPushCondToOnCond(pCxt, pJoin, &pOnCond);
  }
  if (TSDB_CODE_SUCCESS == code && NULL != pLeftChildCond) {
    code = pdcOptPushCondToChild(pCxt, (SLogicNode*)nodesListGetNode(pJoin->node.pChildren, 0), &pLeftChildCond);
  }
  if (TSDB_CODE_SUCCESS == code && NULL != pRightChildCond) {
    code = pdcOptPushCondToChild(pCxt, (SLogicNode*)nodesListGetNode(pJoin->node.pChildren, 1), &pRightChildCond);
  }

  if (TSDB_CODE_SUCCESS == code) {
    OPTIMIZE_FLAG_SET_MASK(pJoin->node.optimizedFlag, OPTIMIZE_FLAG_CPD);
    pCxt->optimized = true;
    code = pdcOptCheckJoinOnCond(pCxt, pJoin);
  } else {
    nodesDestroyNode(pOnCond);
    nodesDestroyNode(pLeftChildCond);
    nodesDestroyNode(pRightChildCond);
  }

  return code;
}

static int32_t pdcOptDealAgg(SOptimizeContext* pCxt, SAggLogicNode* pAgg) {
  // todo
  return TSDB_CODE_SUCCESS;
}

static int32_t pushDownCondOptimizeImpl(SOptimizeContext* pCxt, SLogicNode* pLogicNode) {
  int32_t code = TSDB_CODE_SUCCESS;
  switch (nodeType(pLogicNode)) {
    case QUERY_NODE_LOGIC_PLAN_SCAN:
      code = pdcOptDealScan(pCxt, (SScanLogicNode*)pLogicNode);
      break;
    case QUERY_NODE_LOGIC_PLAN_JOIN:
      code = pdcOptDealJoin(pCxt, (SJoinLogicNode*)pLogicNode);
      break;
    case QUERY_NODE_LOGIC_PLAN_AGG:
      code = pdcOptDealAgg(pCxt, (SAggLogicNode*)pLogicNode);
      break;
    default:
      break;
  }
  if (TSDB_CODE_SUCCESS == code) {
    SNode* pChild = NULL;
    FOREACH(pChild, pLogicNode->pChildren) {
      code = pushDownCondOptimizeImpl(pCxt, (SLogicNode*)pChild);
      if (TSDB_CODE_SUCCESS != code) {
        break;
      }
    }
  }
  return code;
}

static int32_t pushDownCondOptimize(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan) {
  return pushDownCondOptimizeImpl(pCxt, pLogicSubplan->pNode);
}

static bool sortPriKeyOptIsPriKeyOrderBy(SNodeList* pSortKeys) {
  if (1 != LIST_LENGTH(pSortKeys)) {
    return false;
  }
  SNode* pNode = ((SOrderByExprNode*)nodesListGetNode(pSortKeys, 0))->pExpr;
  return (QUERY_NODE_COLUMN == nodeType(pNode) ? (PRIMARYKEY_TIMESTAMP_COL_ID == ((SColumnNode*)pNode)->colId) : false);
}

static bool sortPriKeyOptMayBeOptimized(SLogicNode* pNode) {
  if (QUERY_NODE_LOGIC_PLAN_SORT != nodeType(pNode)) {
    return false;
  }
  SSortLogicNode* pSort = (SSortLogicNode*)pNode;
  if (pSort->groupSort || !sortPriKeyOptIsPriKeyOrderBy(pSort->pSortKeys) || 1 != LIST_LENGTH(pSort->node.pChildren)) {
    return TSDB_CODE_SUCCESS;
  }
  return true;
}

static int32_t sortPriKeyOptGetScanNodesImpl(SLogicNode* pNode, bool* pNotOptimize, SNodeList** pScanNodes) {
  int32_t code = TSDB_CODE_SUCCESS;

  switch (nodeType(pNode)) {
    case QUERY_NODE_LOGIC_PLAN_SCAN:
      if (TSDB_SUPER_TABLE != ((SScanLogicNode*)pNode)->tableType) {
        return nodesListMakeAppend(pScanNodes, (SNode*)pNode);
      }
      break;
    case QUERY_NODE_LOGIC_PLAN_JOIN:
      code =
          sortPriKeyOptGetScanNodesImpl((SLogicNode*)nodesListGetNode(pNode->pChildren, 0), pNotOptimize, pScanNodes);
      if (TSDB_CODE_SUCCESS == code) {
        code =
            sortPriKeyOptGetScanNodesImpl((SLogicNode*)nodesListGetNode(pNode->pChildren, 1), pNotOptimize, pScanNodes);
      }
      return code;
    case QUERY_NODE_LOGIC_PLAN_AGG:
      *pNotOptimize = true;
      return code;
    default:
      break;
  }

  if (1 != LIST_LENGTH(pNode->pChildren)) {
    *pNotOptimize = true;
    return TSDB_CODE_SUCCESS;
  }

  return sortPriKeyOptGetScanNodesImpl((SLogicNode*)nodesListGetNode(pNode->pChildren, 0), pNotOptimize, pScanNodes);
}

static int32_t sortPriKeyOptGetScanNodes(SLogicNode* pNode, SNodeList** pScanNodes) {
  bool    notOptimize = false;
  int32_t code = sortPriKeyOptGetScanNodesImpl(pNode, &notOptimize, pScanNodes);
  if (TSDB_CODE_SUCCESS != code || notOptimize) {
    nodesClearList(*pScanNodes);
  }
  return code;
}

static EOrder sortPriKeyOptGetPriKeyOrder(SSortLogicNode* pSort) {
  return ((SOrderByExprNode*)nodesListGetNode(pSort->pSortKeys, 0))->order;
}

static int32_t sortPriKeyOptApply(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan, SSortLogicNode* pSort,
                                  SNodeList* pScanNodes) {
  EOrder order = sortPriKeyOptGetPriKeyOrder(pSort);
  if (ORDER_DESC == order) {
    SNode* pScan = NULL;
    FOREACH(pScan, pScanNodes) { TSWAP(((SScanLogicNode*)pScan)->scanSeq[0], ((SScanLogicNode*)pScan)->scanSeq[1]); }
  }

  int32_t code =
      replaceLogicNode(pLogicSubplan, (SLogicNode*)pSort, (SLogicNode*)nodesListGetNode(pSort->node.pChildren, 0));
  if (TSDB_CODE_SUCCESS == code) {
    NODES_CLEAR_LIST(pSort->node.pChildren);
    nodesDestroyNode((SNode*)pSort);
  }
  return code;
}

static int32_t sortPrimaryKeyOptimizeImpl(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan, SSortLogicNode* pSort) {
  SNodeList* pScanNodes = NULL;
  int32_t    code = sortPriKeyOptGetScanNodes((SLogicNode*)nodesListGetNode(pSort->node.pChildren, 0), &pScanNodes);
  if (TSDB_CODE_SUCCESS == code && NULL != pScanNodes) {
    code = sortPriKeyOptApply(pCxt, pLogicSubplan, pSort, pScanNodes);
  }
  nodesClearList(pScanNodes);
  return code;
}

static int32_t sortPrimaryKeyOptimize(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan) {
  SSortLogicNode* pSort = (SSortLogicNode*)optFindPossibleNode(pLogicSubplan->pNode, sortPriKeyOptMayBeOptimized);
  if (NULL == pSort) {
    return TSDB_CODE_SUCCESS;
  }
  return sortPrimaryKeyOptimizeImpl(pCxt, pLogicSubplan, pSort);
}

static bool smaOptMayBeOptimized(SLogicNode* pNode) {
  if (QUERY_NODE_LOGIC_PLAN_SCAN != nodeType(pNode) || NULL == pNode->pParent ||
      QUERY_NODE_LOGIC_PLAN_WINDOW != nodeType(pNode->pParent) ||
      WINDOW_TYPE_INTERVAL != ((SWindowLogicNode*)pNode->pParent)->winType) {
    return false;
  }

  SScanLogicNode* pScan = (SScanLogicNode*)pNode;
  if (NULL == pScan->pSmaIndexes || NULL != pScan->node.pConditions) {
    return false;
  }

  return true;
}

static int32_t smaOptCreateMerge(SLogicNode* pChild, SNodeList* pMergeKeys, SNodeList* pTargets, SLogicNode** pOutput) {
  SMergeLogicNode* pMerge = (SMergeLogicNode*)nodesMakeNode(QUERY_NODE_LOGIC_PLAN_MERGE);
  if (NULL == pMerge) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  pMerge->node.precision = pChild->precision;
  pMerge->numOfChannels = 2;
  pMerge->pMergeKeys = pMergeKeys;
  pMerge->node.pTargets = pTargets;
  pMerge->pInputs = nodesCloneList(pChild->pTargets);
  if (NULL == pMerge->pInputs) {
    nodesDestroyNode((SNode*)pMerge);
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  *pOutput = (SLogicNode*)pMerge;
  return TSDB_CODE_SUCCESS;
}

static int32_t smaOptRecombinationNode(SLogicSubplan* pLogicSubplan, SLogicNode* pInterval, SLogicNode* pMerge,
                                       SLogicNode* pSmaScan) {
  int32_t code = nodesListMakeAppend(&pMerge->pChildren, (SNode*)pInterval);
  if (TSDB_CODE_SUCCESS == code) {
    code = nodesListMakeAppend(&pMerge->pChildren, (SNode*)pSmaScan);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = replaceLogicNode(pLogicSubplan, pInterval, pMerge);
    pSmaScan->pParent = pMerge;
    pInterval->pParent = pMerge;
  }
  return code;
}

static int32_t smaOptCreateSmaScan(SScanLogicNode* pScan, STableIndexInfo* pIndex, SNodeList* pCols,
                                   SLogicNode** pOutput) {
  SScanLogicNode* pSmaScan = (SScanLogicNode*)nodesMakeNode(QUERY_NODE_LOGIC_PLAN_SCAN);
  if (NULL == pSmaScan) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  pSmaScan->pScanCols = pCols;
  pSmaScan->tableType = TSDB_SUPER_TABLE;
  pSmaScan->tableId = pIndex->dstTbUid;
  pSmaScan->stableId = pIndex->dstTbUid;
  pSmaScan->scanType = SCAN_TYPE_TABLE;
  pSmaScan->scanSeq[0] = pScan->scanSeq[0];
  pSmaScan->scanSeq[1] = pScan->scanSeq[1];
  pSmaScan->scanRange = pScan->scanRange;
  pSmaScan->dataRequired = FUNC_DATA_REQUIRED_DATA_LOAD;

  pSmaScan->pVgroupList = taosMemoryCalloc(1, sizeof(SVgroupsInfo) + sizeof(SVgroupInfo));
  pSmaScan->node.pTargets = nodesCloneList(pCols);
  if (NULL == pSmaScan->pVgroupList || NULL == pSmaScan->node.pTargets) {
    nodesDestroyNode((SNode*)pSmaScan);
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  pSmaScan->pVgroupList->numOfVgroups = 1;
  pSmaScan->pVgroupList->vgroups[0].vgId = pIndex->dstVgId;
  memcpy(&(pSmaScan->pVgroupList->vgroups[0].epSet), &pIndex->epSet, sizeof(SEpSet));

  *pOutput = (SLogicNode*)pSmaScan;
  return TSDB_CODE_SUCCESS;
}

static bool smaOptEqualInterval(SScanLogicNode* pScan, SWindowLogicNode* pWindow, STableIndexInfo* pIndex) {
  if (pWindow->interval != pIndex->interval || pWindow->intervalUnit != pIndex->intervalUnit ||
      pWindow->offset != pIndex->offset || pWindow->sliding != pIndex->sliding ||
      pWindow->slidingUnit != pIndex->slidingUnit) {
    return false;
  }
  if (IS_TSWINDOW_SPECIFIED(pScan->scanRange)) {
    SInterval interval = {.interval = pIndex->interval,
                          .intervalUnit = pIndex->intervalUnit,
                          .offset = pIndex->offset,
                          .offsetUnit = TIME_UNIT_MILLISECOND,
                          .sliding = pIndex->sliding,
                          .slidingUnit = pIndex->slidingUnit,
                          .precision = pScan->node.precision};
    return (pScan->scanRange.skey == taosTimeTruncate(pScan->scanRange.skey, &interval, pScan->node.precision)) &&
           (pScan->scanRange.ekey + 1 == taosTimeTruncate(pScan->scanRange.ekey + 1, &interval, pScan->node.precision));
  }
  return true;
}

static SNode* smaOptCreateSmaCol(SNode* pFunc, uint64_t tableId, int32_t colId) {
  SColumnNode* pCol = (SColumnNode*)nodesMakeNode(QUERY_NODE_COLUMN);
  if (NULL == pCol) {
    return NULL;
  }
  pCol->tableId = tableId;
  pCol->tableType = TSDB_SUPER_TABLE;
  pCol->colId = colId;
  pCol->colType = COLUMN_TYPE_COLUMN;
  strcpy(pCol->colName, ((SExprNode*)pFunc)->aliasName);
  pCol->node.resType = ((SExprNode*)pFunc)->resType;
  strcpy(pCol->node.aliasName, ((SExprNode*)pFunc)->aliasName);
  return (SNode*)pCol;
}

static int32_t smaOptFindSmaFunc(SNode* pQueryFunc, SNodeList* pSmaFuncs) {
  int32_t index = 0;
  SNode*  pSmaFunc = NULL;
  FOREACH(pSmaFunc, pSmaFuncs) {
    if (nodesEqualNode(pQueryFunc, pSmaFunc)) {
      return index;
    }
    ++index;
  }
  return -1;
}

static int32_t smaOptCreateSmaCols(SNodeList* pFuncs, uint64_t tableId, SNodeList* pSmaFuncs, SNodeList** pOutput,
                                   int32_t* pWStrartIndex) {
  SNodeList* pCols = NULL;
  SNode*     pFunc = NULL;
  int32_t    code = TSDB_CODE_SUCCESS;
  int32_t    index = 0;
  int32_t    smaFuncIndex = -1;
  *pWStrartIndex = -1;
  FOREACH(pFunc, pFuncs) {
    if (FUNCTION_TYPE_WSTARTTS == ((SFunctionNode*)pFunc)->funcType) {
      *pWStrartIndex = index;
    }
    smaFuncIndex = smaOptFindSmaFunc(pFunc, pSmaFuncs);
    if (smaFuncIndex < 0) {
      break;
    } else {
      code = nodesListMakeStrictAppend(&pCols, smaOptCreateSmaCol(pFunc, tableId, smaFuncIndex + 2));
      if (TSDB_CODE_SUCCESS != code) {
        break;
      }
    }
    ++index;
  }

  if (TSDB_CODE_SUCCESS == code && smaFuncIndex >= 0) {
    *pOutput = pCols;
  } else {
    nodesDestroyList(pCols);
  }

  return code;
}

static int32_t smaOptCouldApplyIndex(SScanLogicNode* pScan, STableIndexInfo* pIndex, SNodeList** pCols,
                                     int32_t* pWStrartIndex) {
  SWindowLogicNode* pWindow = (SWindowLogicNode*)pScan->node.pParent;
  if (!smaOptEqualInterval(pScan, pWindow, pIndex)) {
    return TSDB_CODE_SUCCESS;
  }
  SNodeList* pSmaFuncs = NULL;
  int32_t    code = nodesStringToList(pIndex->expr, &pSmaFuncs);
  if (TSDB_CODE_SUCCESS == code) {
    code = smaOptCreateSmaCols(pWindow->pFuncs, pIndex->dstTbUid, pSmaFuncs, pCols, pWStrartIndex);
  }
  nodesDestroyList(pSmaFuncs);
  return code;
}

static SNode* smaOptCreateWStartTs() {
  SFunctionNode* pWStart = (SFunctionNode*)nodesMakeNode(QUERY_NODE_FUNCTION);
  if (NULL == pWStart) {
    return NULL;
  }
  strcpy(pWStart->functionName, "_wstartts");
  snprintf(pWStart->node.aliasName, sizeof(pWStart->node.aliasName), "%s.%p", pWStart->functionName, pWStart);
  if (TSDB_CODE_SUCCESS != fmGetFuncInfo(pWStart, NULL, 0)) {
    nodesDestroyNode((SNode*)pWStart);
    return NULL;
  }
  return (SNode*)pWStart;
}

static int32_t smaOptCreateMergeKey(SNode* pCol, SNodeList** pMergeKeys) {
  SOrderByExprNode* pMergeKey = (SOrderByExprNode*)nodesMakeNode(QUERY_NODE_ORDER_BY_EXPR);
  if (NULL == pMergeKey) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  pMergeKey->pExpr = nodesCloneNode(pCol);
  if (NULL == pMergeKey->pExpr) {
    nodesDestroyNode((SNode*)pMergeKey);
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  pMergeKey->order = ORDER_ASC;
  pMergeKey->nullOrder = NULL_ORDER_FIRST;
  return nodesListMakeStrictAppend(pMergeKeys, (SNode*)pMergeKey);
}

static int32_t smaOptRewriteInterval(SWindowLogicNode* pInterval, int32_t wstrartIndex, SNodeList** pMergeKeys) {
  if (wstrartIndex < 0) {
    SNode* pWStart = smaOptCreateWStartTs();
    if (NULL == pWStart) {
      return TSDB_CODE_OUT_OF_MEMORY;
    }
    int32_t code = createColumnByRewriteExpr(pWStart, &pInterval->node.pTargets);
    if (TSDB_CODE_SUCCESS != code) {
      nodesDestroyNode(pWStart);
      return code;
    }
    wstrartIndex = LIST_LENGTH(pInterval->node.pTargets) - 1;
  }
  return smaOptCreateMergeKey(nodesListGetNode(pInterval->node.pTargets, wstrartIndex), pMergeKeys);
}

static int32_t smaOptApplyIndexExt(SLogicSubplan* pLogicSubplan, SScanLogicNode* pScan, STableIndexInfo* pIndex,
                                   SNodeList* pSmaCols, int32_t wstrartIndex) {
  SWindowLogicNode* pInterval = (SWindowLogicNode*)pScan->node.pParent;
  SNodeList*        pMergeTargets = nodesCloneList(pInterval->node.pTargets);
  if (NULL == pMergeTargets) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  SLogicNode* pSmaScan = NULL;
  SLogicNode* pMerge = NULL;
  SNodeList*  pMergeKeys = NULL;
  int32_t     code = smaOptRewriteInterval(pInterval, wstrartIndex, &pMergeKeys);
  if (TSDB_CODE_SUCCESS == code) {
    code = smaOptCreateSmaScan(pScan, pIndex, pSmaCols, &pSmaScan);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = smaOptCreateMerge(pScan->node.pParent, pMergeKeys, pMergeTargets, &pMerge);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = smaOptRecombinationNode(pLogicSubplan, pScan->node.pParent, pMerge, pSmaScan);
  }
  return code;
}

static int32_t smaOptApplyIndex(SLogicSubplan* pLogicSubplan, SScanLogicNode* pScan, STableIndexInfo* pIndex,
                                SNodeList* pSmaCols, int32_t wstrartIndex) {
  SLogicNode* pSmaScan = NULL;
  int32_t     code = smaOptCreateSmaScan(pScan, pIndex, pSmaCols, &pSmaScan);
  if (TSDB_CODE_SUCCESS == code) {
    code = replaceLogicNode(pLogicSubplan, pScan->node.pParent, pSmaScan);
  }
  return code;
}

static void smaOptDestroySmaIndex(void* p) { taosMemoryFree(((STableIndexInfo*)p)->expr); }

static int32_t smaOptimizeImpl(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan, SScanLogicNode* pScan) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t nindexes = taosArrayGetSize(pScan->pSmaIndexes);
  for (int32_t i = 0; i < nindexes; ++i) {
    STableIndexInfo* pIndex = taosArrayGet(pScan->pSmaIndexes, i);
    SNodeList*       pSmaCols = NULL;
    int32_t          wstrartIndex = -1;
    code = smaOptCouldApplyIndex(pScan, pIndex, &pSmaCols, &wstrartIndex);
    if (TSDB_CODE_SUCCESS == code && NULL != pSmaCols) {
      code = smaOptApplyIndex(pLogicSubplan, pScan, pIndex, pSmaCols, wstrartIndex);
      taosArrayDestroyEx(pScan->pSmaIndexes, smaOptDestroySmaIndex);
      pScan->pSmaIndexes = NULL;
      break;
    }
  }
  return code;
}

static int32_t smaOptimize(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan) {
  SScanLogicNode* pScan = (SScanLogicNode*)optFindPossibleNode(pLogicSubplan->pNode, smaOptMayBeOptimized);
  if (NULL == pScan) {
    return TSDB_CODE_SUCCESS;
  }
  return smaOptimizeImpl(pCxt, pLogicSubplan, pScan);
}

static EDealRes partTagsOptHasColImpl(SNode* pNode, void* pContext) {
  if (QUERY_NODE_COLUMN == nodeType(pNode)) {
    if (COLUMN_TYPE_TAG != ((SColumnNode*)pNode)->colType && COLUMN_TYPE_TBNAME != ((SColumnNode*)pNode)->colType) {
      *(bool*)pContext = true;
      return DEAL_RES_END;
    }
  }
  return DEAL_RES_CONTINUE;
}

static bool partTagsOptHasCol(SNodeList* pPartKeys) {
  bool hasCol = false;
  nodesWalkExprs(pPartKeys, partTagsOptHasColImpl, &hasCol);
  return hasCol;
}

static bool partTagsIsOptimizableNode(SLogicNode* pNode) {
  return ((QUERY_NODE_LOGIC_PLAN_PARTITION == nodeType(pNode) ||
           (QUERY_NODE_LOGIC_PLAN_AGG == nodeType(pNode) && NULL != ((SAggLogicNode*)pNode)->pGroupKeys &&
            NULL != ((SAggLogicNode*)pNode)->pAggFuncs)) &&
          1 == LIST_LENGTH(pNode->pChildren) &&
          QUERY_NODE_LOGIC_PLAN_SCAN == nodeType(nodesListGetNode(pNode->pChildren, 0)));
}

static SNodeList* partTagsGetPartKeys(SLogicNode* pNode) {
  if (QUERY_NODE_LOGIC_PLAN_PARTITION == nodeType(pNode)) {
    return ((SPartitionLogicNode*)pNode)->pPartitionKeys;
  } else {
    return ((SAggLogicNode*)pNode)->pGroupKeys;
  }
}

static bool partTagsOptMayBeOptimized(SLogicNode* pNode) {
  if (!partTagsIsOptimizableNode(pNode)) {
    return false;
  }

  return !partTagsOptHasCol(partTagsGetPartKeys(pNode));
}

static EDealRes partTagsOptRebuildTbanmeImpl(SNode** pNode, void* pContext) {
  if (QUERY_NODE_COLUMN == nodeType(*pNode) && COLUMN_TYPE_TBNAME == ((SColumnNode*)*pNode)->colType) {
    SFunctionNode* pFunc = (SFunctionNode*)nodesMakeNode(QUERY_NODE_FUNCTION);
    if (NULL == pFunc) {
      *(int32_t*)pContext = TSDB_CODE_OUT_OF_MEMORY;
      return DEAL_RES_ERROR;
    }
    strcpy(pFunc->functionName, "tbname");
    pFunc->funcType = FUNCTION_TYPE_TBNAME;
    pFunc->node.resType = ((SColumnNode*)*pNode)->node.resType;
    nodesDestroyNode(*pNode);
    *pNode = (SNode*)pFunc;
    return DEAL_RES_IGNORE_CHILD;
  }
  return DEAL_RES_CONTINUE;
}

static int32_t partTagsOptRebuildTbanme(SNodeList* pPartKeys) {
  int32_t code = TSDB_CODE_SUCCESS;
  nodesRewriteExprs(pPartKeys, partTagsOptRebuildTbanmeImpl, &code);
  return code;
}

static int32_t partTagsOptimize(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan) {
  SLogicNode* pNode = optFindPossibleNode(pLogicSubplan->pNode, partTagsOptMayBeOptimized);
  if (NULL == pNode) {
    return TSDB_CODE_SUCCESS;
  }

  int32_t         code = TSDB_CODE_SUCCESS;
  SScanLogicNode* pScan = (SScanLogicNode*)nodesListGetNode(pNode->pChildren, 0);
  if (QUERY_NODE_LOGIC_PLAN_PARTITION == nodeType(pNode)) {
    TSWAP(((SPartitionLogicNode*)pNode)->pPartitionKeys, pScan->pPartTags);
    int32_t code = replaceLogicNode(pLogicSubplan, pNode, (SLogicNode*)pScan);
    if (TSDB_CODE_SUCCESS == code) {
      NODES_CLEAR_LIST(pNode->pChildren);
      nodesDestroyNode((SNode*)pNode);
    }
  } else {
    SNode* pGroupKey = NULL;
    FOREACH(pGroupKey, ((SAggLogicNode*)pNode)->pGroupKeys) {
      code = nodesListMakeStrictAppend(
          &pScan->pPartTags, nodesCloneNode(nodesListGetNode(((SGroupingSetNode*)pGroupKey)->pParameterList, 0)));
      if (TSDB_CODE_SUCCESS != code) {
        break;
      }
    }
    NODES_DESTORY_LIST(((SAggLogicNode*)pNode)->pGroupKeys);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = partTagsOptRebuildTbanme(pScan->pPartTags);
  }
  return code;
}

static bool eliminateProjOptCheckProjColumnNames(SProjectLogicNode* pProjectNode) {
  SHashObj* pProjColNameHash = taosHashInit(16, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY), true, HASH_NO_LOCK);
  SNode*    pProjection;
  FOREACH(pProjection, pProjectNode->pProjections) {
    char*    projColumnName = ((SColumnNode*)pProjection)->colName;
    int32_t* pExist = taosHashGet(pProjColNameHash, projColumnName, strlen(projColumnName));
    if (NULL != pExist) {
      taosHashCleanup(pProjColNameHash);
      return false;
    } else {
      int32_t exist = 1;
      taosHashPut(pProjColNameHash, projColumnName, strlen(projColumnName), &exist, sizeof(exist));
    }
  }
  taosHashCleanup(pProjColNameHash);
  return true;
}

static bool eliminateProjOptMayBeOptimized(SLogicNode* pNode) {
  // TODO: enable this optimization after new mechanising that map projection and targets of project node
  if (NULL != pNode->pParent) {
    return false;
  }

  if (QUERY_NODE_LOGIC_PLAN_PROJECT != nodeType(pNode) || 1 != LIST_LENGTH(pNode->pChildren)) {
    return false;
  }

  SProjectLogicNode* pProjectNode = (SProjectLogicNode*)pNode;
  if (NULL != pProjectNode->node.pLimit || NULL != pProjectNode->node.pSlimit) {
    return false;
  }

  SNode* pProjection;
  FOREACH(pProjection, pProjectNode->pProjections) {
    SExprNode* pExprNode = (SExprNode*)pProjection;
    if (QUERY_NODE_COLUMN != nodeType(pExprNode)) {
      return false;
    }
  }

  return eliminateProjOptCheckProjColumnNames(pProjectNode);
}

static int32_t eliminateProjOptimizeImpl(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan,
                                         SProjectLogicNode* pProjectNode) {
  SLogicNode* pChild = (SLogicNode*)nodesListGetNode(pProjectNode->node.pChildren, 0);
  SNodeList*  pNewChildTargets = nodesMakeList();

  SNode* pProjection = NULL;
  FOREACH(pProjection, pProjectNode->pProjections) {
    SNode* pChildTarget = NULL;
    FOREACH(pChildTarget, pChild->pTargets) {
      if (strcmp(((SColumnNode*)pProjection)->colName, ((SColumnNode*)pChildTarget)->colName) == 0) {
        nodesListAppend(pNewChildTargets, nodesCloneNode(pChildTarget));
        break;
      }
    }
  }
  nodesDestroyList(pChild->pTargets);
  pChild->pTargets = pNewChildTargets;

  int32_t code = replaceLogicNode(pLogicSubplan, (SLogicNode*)pProjectNode, pChild);
  if (TSDB_CODE_SUCCESS == code) {
    NODES_CLEAR_LIST(pProjectNode->node.pChildren);
    nodesDestroyNode((SNode*)pProjectNode);
  }
  return code;
}

static int32_t eliminateProjOptimize(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan) {
  SProjectLogicNode* pProjectNode =
      (SProjectLogicNode*)optFindPossibleNode(pLogicSubplan->pNode, eliminateProjOptMayBeOptimized);

  if (NULL == pProjectNode) {
    return TSDB_CODE_SUCCESS;
  }

  return eliminateProjOptimizeImpl(pCxt, pLogicSubplan, pProjectNode);
}

static bool rewriteTailOptMayBeOptimized(SLogicNode* pNode) {
  return QUERY_NODE_LOGIC_PLAN_INDEF_ROWS_FUNC == nodeType(pNode) && ((SIndefRowsFuncLogicNode*)pNode)->isTailFunc;
}

static SNode* rewriteTailOptCreateOrderByExpr(SNode* pSortKey) {
  SOrderByExprNode* pOrder = (SOrderByExprNode*)nodesMakeNode(QUERY_NODE_ORDER_BY_EXPR);
  if (NULL == pOrder) {
    return NULL;
  }
  pOrder->order = ORDER_DESC;
  pOrder->pExpr = nodesCloneNode(pSortKey);
  if (NULL == pOrder->pExpr) {
    nodesDestroyNode((SNode*)pOrder);
    return NULL;
  }
  return (SNode*)pOrder;
}

static int32_t rewriteTailOptCreateLimit(SNode* pLimit, SNode* pOffset, SNode** pOutput) {
  SLimitNode* pLimitNode = (SLimitNode*)nodesMakeNode(QUERY_NODE_LIMIT);
  if (NULL == pLimitNode) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  pLimitNode->limit = NULL == pLimit ? -1 : ((SValueNode*)pLimit)->datum.i;
  pLimitNode->offset = NULL == pOffset ? -1 : ((SValueNode*)pOffset)->datum.i;
  *pOutput = (SNode*)pLimitNode;
  return TSDB_CODE_SUCCESS;
}

static bool rewriteTailOptNeedGroupSort(SIndefRowsFuncLogicNode* pIndef) {
  return 1 == LIST_LENGTH(pIndef->node.pChildren) &&
         QUERY_NODE_LOGIC_PLAN_PARTITION == nodeType(nodesListGetNode(pIndef->node.pChildren, 0));
}

static int32_t rewriteTailOptCreateSort(SIndefRowsFuncLogicNode* pIndef, SLogicNode** pOutput) {
  SSortLogicNode* pSort = (SSortLogicNode*)nodesMakeNode(QUERY_NODE_LOGIC_PLAN_SORT);
  if (NULL == pSort) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  pSort->groupSort = rewriteTailOptNeedGroupSort(pIndef);
  TSWAP(pSort->node.pChildren, pIndef->node.pChildren);
  pSort->node.precision = pIndef->node.precision;

  // tail(expr, [limit, offset,] _rowts)
  SFunctionNode* pTail = (SFunctionNode*)nodesListGetNode(pIndef->pFuncs, 0);
  int32_t        rowtsIndex = LIST_LENGTH(pTail->pParameterList) - 1;

  int32_t code = nodesListMakeStrictAppend(
      &pSort->pSortKeys, rewriteTailOptCreateOrderByExpr(nodesListGetNode(pTail->pParameterList, rowtsIndex)));
  if (TSDB_CODE_SUCCESS == code) {
    pSort->node.pTargets = nodesCloneList(((SLogicNode*)nodesListGetNode(pSort->node.pChildren, 0))->pTargets);
    if (NULL == pSort->node.pTargets) {
      code = TSDB_CODE_OUT_OF_MEMORY;
    }
  }

  if (TSDB_CODE_SUCCESS == code) {
    *pOutput = (SLogicNode*)pSort;
  } else {
    nodesDestroyNode((SNode*)pSort);
  }

  return code;
}

static SNode* rewriteTailOptCreateProjectExpr(SFunctionNode* pTail) {
  SNode* pExpr = nodesCloneNode(nodesListGetNode(pTail->pParameterList, 0));
  if (NULL == pExpr) {
    return NULL;
  }
  strcpy(((SExprNode*)pExpr)->aliasName, pTail->node.aliasName);
  return pExpr;
}

static int32_t rewriteTailOptCreateProject(SIndefRowsFuncLogicNode* pIndef, SLogicNode** pOutput) {
  SProjectLogicNode* pProject = (SProjectLogicNode*)nodesMakeNode(QUERY_NODE_LOGIC_PLAN_PROJECT);
  if (NULL == pProject) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  TSWAP(pProject->node.pTargets, pIndef->node.pTargets);
  pProject->node.precision = pIndef->node.precision;

  // tail(expr, [limit, offset,] _rowts)
  SFunctionNode* pTail = (SFunctionNode*)nodesListGetNode(pIndef->pFuncs, 0);
  int32_t        limitIndex = LIST_LENGTH(pTail->pParameterList) > 2 ? 1 : -1;
  int32_t        offsetIndex = LIST_LENGTH(pTail->pParameterList) > 3 ? 2 : -1;

  int32_t code = nodesListMakeStrictAppend(&pProject->pProjections, rewriteTailOptCreateProjectExpr(pTail));
  if (TSDB_CODE_SUCCESS == code) {
    code = rewriteTailOptCreateLimit(limitIndex < 0 ? NULL : nodesListGetNode(pTail->pParameterList, limitIndex),
                                     offsetIndex < 0 ? NULL : nodesListGetNode(pTail->pParameterList, offsetIndex),
                                     &pProject->node.pLimit);
  }
  if (TSDB_CODE_SUCCESS == code) {
    *pOutput = (SLogicNode*)pProject;
  } else {
    nodesDestroyNode((SNode*)pProject);
  }
  return code;
}

static int32_t rewriteTailOptimizeImpl(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan,
                                       SIndefRowsFuncLogicNode* pIndef) {
  SLogicNode* pSort = NULL;
  SLogicNode* pProject = NULL;
  int32_t     code = rewriteTailOptCreateSort(pIndef, &pSort);
  if (TSDB_CODE_SUCCESS == code) {
    code = rewriteTailOptCreateProject(pIndef, &pProject);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = nodesListMakeAppend(&pProject->pChildren, (SNode*)pSort);
    pSort->pParent = pProject;
    pSort = NULL;
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = replaceLogicNode(pLogicSubplan, (SLogicNode*)pIndef, pProject);
  }
  if (TSDB_CODE_SUCCESS == code) {
    nodesDestroyNode((SNode*)pIndef);
  } else {
    nodesDestroyNode((SNode*)pSort);
    nodesDestroyNode((SNode*)pProject);
  }
  return code;
}

static int32_t rewriteTailOptimize(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan) {
  SIndefRowsFuncLogicNode* pIndef =
      (SIndefRowsFuncLogicNode*)optFindPossibleNode(pLogicSubplan->pNode, rewriteTailOptMayBeOptimized);

  if (NULL == pIndef) {
    return TSDB_CODE_SUCCESS;
  }

  return rewriteTailOptimizeImpl(pCxt, pLogicSubplan, pIndef);
}

static bool eliminateSetOpMayBeOptimized(SLogicNode* pNode) {
  SLogicNode* pParent = pNode->pParent;
  if (NULL == pParent ||
      QUERY_NODE_LOGIC_PLAN_AGG != nodeType(pParent) && QUERY_NODE_LOGIC_PLAN_PROJECT != nodeType(pParent) ||
      LIST_LENGTH(pParent->pChildren) < 2) {
    return false;
  }
  if (nodeType(pNode) != nodeType(pNode->pParent) || LIST_LENGTH(pNode->pChildren) < 2) {
    return false;
  }
  return true;
}

static int32_t eliminateSetOpOptimizeImpl(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan,
                                          SLogicNode* pSetOpNode) {
  SNode* pSibling;
  FOREACH(pSibling, pSetOpNode->pParent->pChildren) {
    if (nodesEqualNode(pSibling, (SNode*)pSetOpNode)) {
      SNode* pChild;
      FOREACH(pChild, pSetOpNode->pChildren) { ((SLogicNode*)pChild)->pParent = pSetOpNode->pParent; }
      INSERT_LIST(pSetOpNode->pParent->pChildren, pSetOpNode->pChildren);

      pSetOpNode->pChildren = NULL;
      ERASE_NODE(pSetOpNode->pParent->pChildren);
      return TSDB_CODE_SUCCESS;
    }
  }

  return TSDB_CODE_PLAN_INTERNAL_ERROR;
}

static int32_t eliminateSetOpOptimize(SOptimizeContext* pCxt, SLogicSubplan* pLogicSubplan) {
  SLogicNode* pSetOpNode = optFindPossibleNode(pLogicSubplan->pNode, eliminateSetOpMayBeOptimized);
  if (NULL == pSetOpNode) {
    return TSDB_CODE_SUCCESS;
  }

  return eliminateSetOpOptimizeImpl(pCxt, pLogicSubplan, pSetOpNode);
}

// clang-format off
static const SOptimizeRule optimizeRuleSet[] = {
  {.pName = "PickScanPath",               .optimizeFunc = pickScanPathOptimize},
  {.pName = "PushDownCondition",          .optimizeFunc = pushDownCondOptimize},
  {.pName = "SortPrimaryKey",             .optimizeFunc = sortPrimaryKeyOptimize},
  {.pName = "Sma",                        .optimizeFunc = smaOptimize},
  {.pName = "PartitionTags",              .optimizeFunc = partTagsOptimize},
  {.pName = "EliminateProject",           .optimizeFunc = eliminateProjOptimize},
  {.pName = "EliminateSetOperator",       .optimizeFunc = eliminateSetOpOptimize},
  // {.pName = "RewriteTail",                .optimizeFunc = rewriteTailOptimize}
};
// clang-format on

static const int32_t optimizeRuleNum = (sizeof(optimizeRuleSet) / sizeof(SOptimizeRule));

static int32_t applyOptimizeRule(SPlanContext* pCxt, SLogicSubplan* pLogicSubplan) {
  SOptimizeContext cxt = {.pPlanCxt = pCxt, .optimized = false};
  do {
    cxt.optimized = false;
    for (int32_t i = 0; i < optimizeRuleNum; ++i) {
      int32_t code = optimizeRuleSet[i].optimizeFunc(&cxt, pLogicSubplan);
      if (TSDB_CODE_SUCCESS != code) {
        return code;
      }
    }
  } while (cxt.optimized);
  return TSDB_CODE_SUCCESS;
}

int32_t optimizeLogicPlan(SPlanContext* pCxt, SLogicSubplan* pLogicSubplan) {
  return applyOptimizeRule(pCxt, pLogicSubplan);
}
