//==============================================================================
//
// Copyright (c) 2020-2021 Qualcomm Technologies, Inc.
// All Rights Reserved.
// Confidential and Proprietary - Qualcomm Technologies, Inc.
//
//==============================================================================

#ifndef OP_REGISTER_H
#define OP_REGISTER_H 1

#include "c_tricks.h"
#include "op_registry.h"
#include "serialize_register.h"
#include "cost_funcs.h"
#include "op_info.h"
#include "op_register_types.h"
#include "op_package_name.h"
#include "template_help.h"
#include "weak_linkage.h"

#include <memory>
#include <string>
#include <utility>

namespace hnnx {
PUSH_VISIBILITY(default)

// Op Versioning version numbers
enum OpVersionNumber { V1 = 1, V2 = 2, V3 = 3 };

// Operation versioning extension strings.
// IMPORTANT: Do NOT modify any existing version string literals to preserve backward compatibility
#define OpVersionExt_V1 "^1"
#define OpVersionExt_V2 "^2"
#define OpVersionExt_V3 "^3"

API_EXPORT OpFactory make_op_custom_internal(const std::string_view op_name_in, const std::string_view type_tag,
                                             op_reg_parms const &opreg_parms, bool is_external = false,
                                             const std::string_view file_name = "",
                                             const std::string_view target_reg = "core");

API_EXPORT OpFactory make_op_custom(const std::string_view op_name_in, std::string_view const type_tag,
                                    op_reg_parms const &opreg_parmsm, bool is_legacy,
                                    std::string_view const file_name = "", const std::string_view target_reg = "core");

POP_VISIBILITY()

struct item_return {
    typedef op_reg_parms type;
};

// parms_for is wrapped in this class to avoid if constexpr implementation since
// the AUTOSAR checker doesn't evaluate if constexpr blocks properly
// LCOV_EXCL_START [SAFTYSWCCB-1736] constexprs resolved during compile time
// used in pub/impl/ops_opts_registration_defs.h for internal ops with constexpr lvalue
class GetParms {
  public:
    template <typename Derived, int I> constexpr static typename item_return::type get()
    {
        return op_reg_parms::parms_for<Derived, FlagCounter<Derived, I>::get()>();
    }

    template <auto FP, int I> constexpr static typename item_return::type get()
    {
        using Derived = typename DerivedType<FP>::type;
        return op_reg_parms::parms_for<Derived, FlagCounter<Derived, I>::get()>();
    }
};

//LCOV_EXCL_STOP

} // namespace hnnx

/** ModifiedDerivedType is used to perform a transformation from
 * Tensor_TCM -> Tensor for different tensor types. Both FLAGS_FOR and
 * APPEND_REG_OP_ELEM use this metafunction to implement TCM folding for execute.
 * For more details, see docs/register-op-tcm-folding.md
 */
namespace fold {
template <auto, int> struct ModifiedDerivedType;
} //namespace fold
// Need the line number to avoid making the same template specialization
// multiple times
#define MDT(W, LINE)                                                                                                   \
    namespace fold {                                                                                                   \
    template <> struct ModifiedDerivedType<&W, LINE> : public ModifiedDerivedTypeParent {                              \
        using Modified = typename DerivedType<&W>::type;                                                               \
    };                                                                                                                 \
    } //namespace fold

#define COUNT_ARGS_IMPL(_1, _2, _3, count, ...) count
#define COUNT_ARGS(...)                         COUNT_ARGS_IMPL(__VA_ARGS__, 3, 2, 1)

#define PROCESS_NMVRT_VERSION_IMPL(count, ...) PROCESS_NMVRT_VERSION_IMPL2(count, __VA_ARGS__)

#define PROCESS_NMVRT_VERSION_IMPL2(count, ...) PROCESS_NMVRT_VERSION_IMPL2_##count(__VA_ARGS__)

#define PROCESS_NMVRT_VERSION_IMPL2_1(nm)      nm
#define PROCESS_NMVRT_VERSION_IMPL2_2(nm, ver) nm ver

#define PROCESS_NMVRT_VERSION(...) PROCESS_NMVRT_VERSION_IMPL(COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)

/** @brief Create an Op type's type suffix from an optional name variant and argument types */
#define TYPE_SUFFIX(OP, NMVRT, ARGS, ...)                                                                              \
    (hnnx::ConcatStr<hnnx::ConstexprStrLen(OP), hnnx::ConstexprStrLen(NMVRT) + hnnx::ConstexprStrLen(ARGS)>(           \
            OP, (hnnx::ConcatStr<hnnx::ConstexprStrLen(NMVRT), hnnx::ConstexprStrLen(ARGS)>(NMVRT, ARGS).data())))

#ifndef OP_REG_COMPILE
#define DEF_NATIVE_OP(F, OP, LINE, ...)        DEF_NATIVE_OP_NMVRT(F, F, OP, LINE, false, "", __VA_ARGS__)
#define DEF_NATIVE_OP_LEGACY(F, OP, LINE, ...) DEF_NATIVE_OP_NMVRT(F, F, OP, LINE, true, "", __VA_ARGS__)

#define DEF_NATIVE_OP_NO_TCM_FOLDING(F, OP, ...) DEF_NATIVE_OP_NMVRT_NO_TCM_FOLDING(&F, &F, OP, false, "", __VA_ARGS__)
#define DEF_NATIVE_OP_NO_TCM_FOLDING_LEGACY(F, OP, ...)                                                                \
    DEF_NATIVE_OP_NMVRT_NO_TCM_FOLDING(&F, &F, OP, true, "", __VA_ARGS__)

#define DEF_NATIVE_OP_NMVRT(F, W, OP, LINE, IS_LEGACY, ...)                                                            \
    DEF_NATIVE_OP_NMVRT_IMPL(F, W, OP, LINE, IS_LEGACY, PROCESS_NMVRT_VERSION(__VA_ARGS__))

#define DEF_NATIVE_OP_NMVRT_NO_TCM_FOLDING(F, W, OP, IS_LEGACY, ...)                                                   \
    DEF_NATIVE_OP_NMVRT_NO_TCM_FOLDING_IMPL(F, W, OP, IS_LEGACY, PROCESS_NMVRT_VERSION(__VA_ARGS__))

#define DEF_NATIVE_OP_NMVRT_IMPL(F, W, OP, LINE, IS_LEGACY, NMVRT)                                                     \
    MDT(F, LINE)                                                                                                       \
    APPEND_REG_OP_ELEM(&W, THIS_PKG_NAME_STR "::" OP NMVRT,                                                            \
                       TYPE_SUFFIX(OP, NMVRT, hnnx::ArgsTuples2<&F>::inputTypeNames), LINE, IS_LEGACY)

/** __VA_ARGS__ contains versioning info, and passing into TYPE_SUFFIX as name variant */
#define DEF_NATIVE_OP_NMVRT_NO_TCM_FOLDING_IMPL(F, W, OP, IS_LEGACY, NMVRT)                                            \
    APPEND_REG_OP_ELEM_NO_TCM_FOLDING(W, THIS_PKG_NAME_STR "::" OP,                                                    \
                                      TYPE_SUFFIX(OP, NMVRT, hnnx::ArgsTuples2<F>::inputTypeNames), false, IS_LEGACY)

#define DEF_NATIVE_OP_NMVRT_NO_TCM_FOLDING_REG(F, W, OP, IS_LEGACY, REG, ...)                                          \
    DEF_NATIVE_OP_NMVRT_NO_TCM_FOLDING_REG_IMPL(F, W, OP, IS_LEGACY, REG, PROCESS_NMVRT_VERSION(__VA_ARGS__))

#define DEF_NATIVE_OP_NMVRT_NO_TCM_FOLDING_REG_IMPL(F, W, OP, IS_LEGACY, REG, NMVRT)                                   \
    APPEND_REG_OP_ELEM_NO_TCM_FOLDING_REG(W, THIS_PKG_NAME_STR "::" OP,                                                \
                                          TYPE_SUFFIX(OP, NMVRT, hnnx::ArgsTuples2<F>::inputTypeNames), false,         \
                                          IS_LEGACY, REG)

#else
#define DEF_NATIVE_OP(F, OP, LINE)                          __reg_op__(F, OP)<<<__FILE__, __LINE__>>>
#define DEF_NATIVE_OP_NO_TCM_FOLDING(F, OP)                 __reg_op__(F, OP)<<<__FILE__, __LINE__>>>
#define DEF_NATIVE_OP_NMVRT(F, W, OP, LINE, NMVRT)          __reg_op__(F, OP)<<<__FILE__, __LINE__>>>
#define DEF_NATIVE_OP_NMVRT_NO_TCM_FOLDING(F, W, OP, NMVRT) __reg_op__(F, OP)<<<__FILE__, __LINE__>>>
#endif

// TCM folding is an optimization to reduce skel size, so we only need it for execute.
#if defined(PREPARE_DISABLED) && !defined(TCM_FOLDING_DISABLED)
#define REGISTER_OP(F, STR, ...)        DEF_NATIVE_OP(F, STR, __LINE__, __VA_ARGS__)
#define REGISTER_OP_LEGACY(F, STR, ...) DEF_NATIVE_OP_LEGACY(F, STR, __LINE__, __VA_ARGS__)
#else
#define REGISTER_OP(F, STR, ...)        DEF_NATIVE_OP_NO_TCM_FOLDING(F, STR, __VA_ARGS__)
#define REGISTER_OP_LEGACY(F, STR, ...) DEF_NATIVE_OP_NO_TCM_FOLDING_LEGACY(F, STR, __VA_ARGS__)
#endif

// see register-op-tcm-folding.md
#define REGISTER_OP_NO_TCM_FOLDING(F, STR, ...)        DEF_NATIVE_OP_NO_TCM_FOLDING(F, STR, __VA_ARGS__)
#define REGISTER_OP_NO_TCM_FOLDING_LEGACY(F, STR, ...) DEF_NATIVE_OP_NO_TCM_FOLDING_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_WRAPPER(F, W, STR, NMVRT, ...)                                                                     \
    DEF_NATIVE_OP_NMVRT_NO_TCM_FOLDING(&F, W, STR, false, NMVRT, __VA_ARGS__)
#define REGISTER_OP_WRAPPER_LEGACY(F, W, STR, NMVRT, ...)                                                              \
    DEF_NATIVE_OP_NMVRT_NO_TCM_FOLDING(&F, W, STR, true, NMVRT, __VA_ARGS__)

#define REGISTER_OP_WRAPPER_REG(F, W, STR, REG, NMVRT, ...)                                                            \
    DEF_NATIVE_OP_NMVRT_NO_TCM_FOLDING_REG(&F, W, STR, false, REG, NMVRT, __VA_ARGS__)

#define REGISTER_OP_WRAPPER_REG_LEGACY(F, W, STR, REG, NMVRT, ...)                                                     \
    DEF_NATIVE_OP_NMVRT_NO_TCM_FOLDING_REG(&F, W, STR, true, REG, NMVRT, __VA_ARGS__)

#define REGISTER_OP_EXT(F, STR, NMVRT, ...)        REGISTER_OP_WRAPPER(F, &F, STR, NMVRT, __VA_ARGS__)
#define REGISTER_OP_EXT_LEGACY(F, STR, NMVRT, ...) REGISTER_OP_WRAPPER_LEGACY(F, &F, STR, NMVRT, __VA_ARGS__)

#define REGISTER_OP_EXT_REG(F, STR, REG, NMVRT, ...) REGISTER_OP_WRAPPER_REG(F, &F, STR, REG, NMVRT, __VA_ARGS__)
#define REGISTER_OP_EXT_REG_LEGACY(F, STR, REG, NMVRT, ...)                                                            \
    REGISTER_OP_WRAPPER_REG_LEGACY(F, &F, STR, REG, NMVRT, __VA_ARGS__)

#define REGISTER_OP_HVX_EXT(F, STR, NMVRT, ...)                                                                        \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HVX)                                                                \
    REGISTER_OP_EXT(F, STR, NMVRT, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HVX_EXT_LEGACY(F, STR, NMVRT, ...)                                                                 \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HVX)                                                                \
    REGISTER_OP_EXT_LEGACY(F, STR, NMVRT, __VA_ARGS__)

#define REGISTER_OP_HVX(F, STR, ...)                                                                                   \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HVX)                                                                               \
    REGISTER_OP(F, STR, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HVX_LEGACY(F, STR, ...)                                                                            \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HVX)                                                                               \
    REGISTER_OP_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_HVX_NO_TCM_FOLDING(F, STR, ...)                                                                    \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HVX)                                                                \
    REGISTER_OP_NO_TCM_FOLDING(F, STR, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HVX_NO_TCM_FOLDING_LEGACY(F, STR, ...)                                                             \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HVX)                                                                \
    REGISTER_OP_NO_TCM_FOLDING_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_HVX_COPY(F, STR, ...)                                                                              \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HVX, Flags::IS_COPY);                                                              \
    REGISTER_OP(F, STR, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HVX_COPY_LEGACY(F, STR, ...)                                                                       \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HVX, Flags::IS_COPY);                                                              \
    REGISTER_OP_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_HVX_COPY_NO_TCM_FOLDING(F, STR, ...)                                                               \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HVX, Flags::IS_COPY)                                                \
    REGISTER_OP_NO_TCM_FOLDING(F, STR, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HVX_COPY_NO_TCM_FOLDING_LEGACY(F, STR, ...)                                                        \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HVX, Flags::IS_COPY)                                                \
    REGISTER_OP_NO_TCM_FOLDING_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_HLX_EXT(F, STR, NMVRT, ...)                                                                        \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HLX)                                                                \
    REGISTER_OP_EXT(F, STR, NMVRT, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HLX_EXT_LEGACY(F, STR, NMVRT, ...)                                                                 \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HLX)                                                                \
    REGISTER_OP_EXT_LEGACY(F, STR, NMVRT, __VA_ARGS__)

#define REGISTER_OP_HLX(F, STR, ...)                                                                                   \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HLX)                                                                               \
    REGISTER_OP(F, STR, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HLX_LEGACY(F, STR, ...)                                                                            \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HLX)                                                                               \
    REGISTER_OP_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_HLX_NO_TCM_FOLDING(F, STR, ...)                                                                    \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HLX)                                                                \
    REGISTER_OP_NO_TCM_FOLDING(F, STR, __VA_ARGS__)
//Legacy version
#define REGISTER_OP_HLX_NO_TCM_FOLDING_LEGACY(F, STR, ...)                                                             \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HLX)                                                                \
    REGISTER_OP_NO_TCM_FOLDING_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_HLX_COPY(F, STR, ...)                                                                              \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HLX, Flags::IS_COPY);                                                              \
    REGISTER_OP(F, STR, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HLX_COPY_LEGACY(F, STR, ...)                                                                       \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HLX, Flags::IS_COPY);                                                              \
    REGISTER_OP_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_HLX_COPY_NO_TCM_FOLDING(F, STR, ...)                                                               \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HLX, Flags::IS_COPY)                                                \
    REGISTER_OP_NO_TCM_FOLDING(F, STR, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HLX_COPY_NO_TCM_FOLDING_LEGACY(F, STR, ...)                                                        \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HLX, Flags::IS_COPY)                                                \
    REGISTER_OP_NO_TCM_FOLDING_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_HMX(F, STR, ...)                                                                                   \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HMX);                                                                              \
    REGISTER_OP(F, STR, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HMX_LEGACY(F, STR, ...)                                                                            \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HMX);                                                                              \
    REGISTER_OP_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_HMX_NO_TCM_FOLDING(F, STR, ...)                                                                    \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HMX);                                                               \
    REGISTER_OP_NO_TCM_FOLDING(F, STR, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HMX_NO_TCM_FOLDING_LEGACY(F, STR, ...)                                                             \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HMX);                                                               \
    REGISTER_OP_NO_TCM_FOLDING_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_HVX_SRC_DESTRUCTIVE(F, STR, ...)                                                                   \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HVX, Flags::CAN_BE_SRC_DESTRUCTIVE);                                               \
    REGISTER_OP(F, STR, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HVX_SRC_DESTRUCTIVE_LEGACY(F, STR, ...)                                                            \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HVX, Flags::CAN_BE_SRC_DESTRUCTIVE);                                               \
    REGISTER_OP_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_HLX_SRC_DESTRUCTIVE(F, STR, ...)                                                                   \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HLX, Flags::CAN_BE_SRC_DESTRUCTIVE);                                               \
    REGISTER_OP(F, STR, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HLX_SRC_DESTRUCTIVE_LEGACY(F, STR, ...)                                                            \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HLX, Flags::CAN_BE_SRC_DESTRUCTIVE);                                               \
    REGISTER_OP_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_HVX_SRC_DESTRUCTIVE_NO_TCM_FOLDING(F, STR, ...)                                                    \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HVX, Flags::CAN_BE_SRC_DESTRUCTIVE);                                \
    REGISTER_OP_NO_TCM_FOLDING(F, STR, __VA_ARGS__)

// Legacy version
#define REGISTER_OP_HVX_SRC_DESTRUCTIVE_NO_TCM_FOLDING_LEGACY(F, STR, ...)                                             \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HVX, Flags::CAN_BE_SRC_DESTRUCTIVE);                                \
    REGISTER_OP_NO_TCM_FOLDING_LEGACY(F, STR, __VA_ARGS__)

#define REGISTER_OP_HLX_SRC_DESTRUCTIVE_NO_TCM_FOLDING(F, STR)                                                         \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HLX, Flags::CAN_BE_SRC_DESTRUCTIVE);                                \
    REGISTER_OP_NO_TCM_FOLDING(F, STR)

// Legacy version
#define REGISTER_OP_HLX_SRC_DESTRUCTIVE_NO_TCM_FOLDING_LEGACY(F, STR)                                                  \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HLX, Flags::CAN_BE_SRC_DESTRUCTIVE);                                \
    REGISTER_OP_NO_TCM_FOLDING_LEGACY(F, STR)

//Register Ops which are never serialized, because they will be removed in const propagation
#define REGISTER_OP_CONST_HVX(F, STR)                                                                                  \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HVX, Flags::IS_CONST);                                                             \
    REGISTER_OP(F, STR)

#define REGISTER_OP_CONST_HVX_NO_TCM_FOLDING(F, STR)                                                                   \
    FLAGS_FOR_DT_NO_TCM_FOLDING(F, Flags::RESOURCE_HVX, Flags::IS_CONST);                                              \
    REGISTER_OP_NO_TCM_FOLDING(F, STR)

#define REGISTER_OP_CONST(F, STR)                                                                                      \
    FLAGS_FOR_DT(F, Flags::IS_CONST);                                                                                  \
    REGISTER_OP(F, STR)

#define REGISTER_OP_HVX_EXT_REG(F, STR, REG, NMVRT, ...)                                                               \
    FLAGS_FOR_DT(F, Flags::RESOURCE_HVX);                                                                              \
    REGISTER_OP_EXT_REG(F, STR, REG, NMVRT, __VA_ARGS__)

#endif
