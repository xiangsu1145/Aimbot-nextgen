//==============================================================================
//
//  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
//  All rights reserved.
//  Confidential and Proprietary - Qualcomm Technologies, Inc.
//
//==============================================================================

/** @file
 *  @brief QNN LPAI Graph components
 */

#ifndef QNN_LPAI_GRAPH_H
#define QNN_LPAI_GRAPH_H

#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#endif

#include "QnnLpaiTypes.h"
#include "QnnLpaiMem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  // see QnnLpaiGraph_Mem_t
  QNN_LPAI_GRAPH_SET_CFG_SCRATCH_MEM = QNN_LPAI_GRAPH_SET_CFG_SCRATCH_MEM_DEFAULT,
  // see QnnLpaiGraph_Mem_t
  QNN_LPAI_GRAPH_SET_CFG_PERSISTENT_MEM = QNN_LPAI_GRAPH_SET_CFG_PERSISTENT_MEM_DEFAULT,
  // see QnnLpaiGraph_PerfCfg_t
  QNN_LPAI_GRAPH_SET_CFG_PERF_CFG = QNN_LPAI_GRAPH_SET_CFG_PERF_CFG_DEFAULT,
  // see QnnLpaiGraph_CoreAffinity_t
  QNN_LPAI_GRAPH_SET_CFG_CORE_AFFINITY = QNN_LPAI_GRAPH_SET_CFG_CORE_AFFINITY_DEFAULT,
  // Unused, present to ensure 32 bits.
  QNN_LPAI_GRAPH_SET_CFG_UNDEFINED = 0x7fffffff
} QnnLpaiGraph_SetConfigOption_t;

typedef enum {
  // get the size requirement of scratch memory, return uint32_t
  QNN_LPAI_GRAPH_GET_PROP_SCRATCH_MEM_SIZE = QNN_LPAI_GRAPH_GET_PROP_SCRATCH_MEM_SIZE_DEFAULT,
  // get the size requirement of persistent memory, return uint32_t
  QNN_LPAI_GRAPH_GET_PROP_PERSISTENT_MEM_SIZE = QNN_LPAI_GRAPH_GET_PROP_PERSISTENT_MEM_SIZE_DEFAULT,
  // Unused, present to ensure 32 bits.
  QNN_LPAI_GRAPH_GET_PROP_UNDEFINED = 0x7fffffff
} QnnLpaiGraph_GetPropertyOption_t;

typedef enum {
  QNN_LPAI_GRAPH_CLIENT_PERF_TYPE_REAL_TIME     = 1,
  QNN_LPAI_GRAPH_CLIENT_PERF_TYPE_NON_REAL_TIME = 2,
  // Unused, present to ensure 32 bits.
  QNN_LPAI_GRAPH_CLIENT_PERF_TYPE_UNDEFINED = 0x7fffffff
} QnnLpaiGraph_ClientPerfType_t;

typedef struct {
  uint32_t fps;
  uint32_t ftrtRatio;
  QnnLpaiGraph_ClientPerfType_t clientType;
} QnnLpaiGraph_PerfCfg_t;

// clang-format off
/// QnnLpaiGraph_PerfCfg_t initializer macro
#define QNN_LPAI_GRAPH_PERF_CFG_INIT                             \
  {                                                              \
    1u,                                        /*fps*/           \
    10u,                                       /*ftrtRatio*/     \
    QNN_LPAI_GRAPH_CLIENT_PERF_TYPE_REAL_TIME  /*clientType*/    \
  }
// clang-format on

typedef enum {
  QNN_LPAI_GRAPH_CORE_AFFINITY_SOFT = 1,
  QNN_LPAI_GRAPH_CORE_AFFINITY_HARD = 2,
  // Unused, present to ensure 32 bits.
  QNN_LPAI_GRAPH_CORE_AFFINITY_UNDEFINED = 0x7fffffff
} QnnLpaiGraph_CoreAffinityType_t;

typedef struct {
  QnnLpaiGraph_CoreAffinityType_t affinity;
  // `coreSelection` is a bitmask that specifies which core(s) to select.
  // Each bit represents a core: if the bit is set (1), the corresponding core is selected.
  //
  // Examples:
  //   - coreSelection = 0x01 → selects core 0
  //   - coreSelection = 0x02 → selects core 1
  //   - coreSelection = 0x00 → no specific preference; any available core may be selected
  //
  // Note:
  //   - A mix of cores (e.g., coreSelection = 0x03 to select both core 0 and core 1) is **not yet supported**.
  //   - If multiple bits are set, the behavior is undefined and may result in an error.
  uint32_t coreSelection;
} QnnLpaiGraph_CoreAffinity_t;

// clang-format off
/// QnnLpaiGraph_CoreAffinity_t initializer macro
#define QNN_LPAI_GRAPH_CORE_AFFINITY_INIT                          \
  {                                                                \
    QNN_LPAI_GRAPH_CORE_AFFINITY_SOFT,       /*affinity*/          \
    0u                                       /*core_selection*/    \
  }
/// helper macro to set affinity to a particular core
#define QNN_LPAI_CORE_AFFINITY(core_id, affinity) \
  {                                               \
    (affinity),                                   \
    (1U << (core_id))                             \
  }
// clang-format on

typedef struct {
  QnnLpaiMem_MemType_t memType;
  uint32_t size;
  void* addr;
} QnnLpaiGraph_Mem_t;

// clang-format off
/// QnnLpaiGraph_Mem_t initializer macro
#define QNN_LPAI_GRAPH_MEM_INIT                            \
  {                                                        \
    QNN_LPAI_MEM_TYPE_UNDEFINED,         /*memType*/       \
    0u,                                  /*size*/          \
    NULL                                 /*addr*/          \
  }
// clang-format on

// used by QnnGraph_setConfig
typedef struct {
  uint32_t option;
  void* config;
} QnnLpaiGraph_CustomConfig_t;

// clang-format off
/// QnnLpaiGraph_CustomConfig_t initializer macro
#define QNN_LPAI_GRAPH_CUSTOM_CONFIG_INIT                          \
  {                                                                \
    QNN_LPAI_GRAPH_SET_CFG_UNDEFINED,                /*option*/    \
    NULL                                             /*config*/    \
  }
// clang-format on

// used by QnnGraph_getProperty
typedef struct {
  uint32_t option;
  void* property;
} QnnLpaiGraph_CustomProperty_t;

// clang-format off
/// QnnLpaiGraph_CustomProperty_t initializer macro
#define QNN_LPAI_GRAPH_CUSTOM_PROPERTY_INIT                          \
  {                                                                  \
    QNN_LPAI_GRAPH_GET_PROP_UNDEFINED,                 /*option*/    \
    NULL                                               /*property*/  \
  }
// clang-format on

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // QNN_LPAI_GRAPH_H
