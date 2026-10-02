// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#ifndef EG_COMPACT_GC_ROOTS_H
#define EG_COMPACT_GC_ROOTS_H

typedef struct eg_codegen eg_codegen_t;
typedef struct LLVMOpaqueTargetMachine* LLVMTargetMachineRef;

void eg_compact_gc_roots(eg_codegen_t* cg);
int eg_optimize_scalar_ir(eg_codegen_t* cg, LLVMTargetMachineRef tm);

#endif
