//==============================================================================
//
// Copyright (c) 2020 Qualcomm Technologies, Inc.
// All Rights Reserved.
// Confidential and Proprietary - Qualcomm Technologies, Inc.
//
//==============================================================================

#ifndef UNIQUE_TYPES_H
#define UNIQUE_TYPES_H 1

// simpler way ... generates smaller code
#define DEFINE_UNIQ_TY()                                                                                               \
    namespace {                                                                                                        \
    template <int K> struct UniqTy {};                                                                                 \
    } // namespace
#define UNIQUE_TYPE UniqTy<__LINE__>
#endif
