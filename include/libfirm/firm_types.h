/*
 * Copyright (C) 1995-2007 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief      Definition of opaque firm types
 * @author     Michael Beck
 * @version    $Id$
 */
#ifndef FIRM_COMMON_FIRM_TYPES_H
#define FIRM_COMMON_FIRM_TYPES_H

#ifdef _MSC_VER
typedef          __int64 long64;
typedef unsigned __int64 ulong64;

#define LL_FMT	"i64"
#define ULL_FMT	"ui64"

#else
typedef          long long long64;
typedef unsigned long long ulong64;

#define LL_FMT	"ll"
#define ULL_FMT	"llu"

#endif /* _MSC_VER */

typedef struct dbg_info             dbg_info,            *dbg_info_ptr;
typedef const struct _ident         ident,               *ir_ident_ptr;
typedef struct ir_node              ir_node,             *ir_node_ptr;
typedef struct ir_op                ir_op,               *ir_op_ptr;
typedef struct ir_mode              ir_mode,             *ir_mode_ptr;
typedef struct _ir_edge_t           ir_edge_t,           *ir_edge_ptr;
typedef struct tarval               tarval,              *ir_tarval_ptr;
typedef struct ir_enum_const        ir_enum_const,       *ir_enum_const_ptr;
typedef struct ir_type              ir_type,             *ir_type_ptr;
typedef struct ir_graph             ir_graph,            *ir_graph_ptr;
typedef struct ir_loop              ir_loop,             *ir_loop_ptr;
typedef struct ir_region            ir_region,           *ir_region_ptr;
typedef struct ir_reg_tree          ir_reg_tree,         *ir_reg_tree_ptr;
typedef struct ir_entity            ir_entity,           *ir_entity_ptr;
typedef struct compound_graph_path  compound_graph_path, *ir_compound_graph_path_ptr;
typedef struct _ir_phase            ir_phase,            *ir_phase_ptr;
typedef struct _ir_extblk           ir_extblk,           *ir_extblk_ptr;
typedef struct ir_exec_freq         ir_exec_freq,        *ir_exec_freq_ptr;
typedef struct sn_entry             *seqno_t;


typedef void irg_walk_func(ir_node *, void *);
typedef void irg_reg_walk_func(ir_region *, void *);

#endif
