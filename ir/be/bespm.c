#include "be_t.h"
#include "belive.h"
#include "bemodule.h"
#include "besched.h"
#include "bespill.h"
#include "bespm.h"
#include "bessaconstr.h"
#include "bestack.h"
#include "callgraph.h"
#include "cgana.h"
#include "execfreq.h"
#include "gen_ia32_regalloc_if.h"
#include "ia32_bearch_t.h"
#include "ia32_new_nodes.h"
#include "ia32_transform.h"
#include "iredges.h"
#include "irgraph_t.h"
#include "irgwalk.h"
#include "irloop.h"
#include "irnode.h"
#include "irnode_t.h"
#include "irouts.h"
#include "irprintf.h"
#include "irprog_t.h"
#include "irtools.h"
#include "pdeq.h"
#include "pmap.h"
#include "pset_new.h"
#include "target_t.h"
#include "util.h"
#include "xmalloc.h"

#include <math.h>
#include <stdbool.h>

typedef struct timestamp timestamp;

typedef struct alloc_result alloc_result;

typedef enum pred_states {
	UNKNOWN_STATUS, PRED_NOT_DONE, PREDS_DONE, UNFINISHED_LOOP, FINISHED_LOOP, COND_JOIN,
} pred_states;

struct timestamp {
	ir_node *last_block;
	alloc_result *last_alloc;
	timestamp *caller_timestamp;
	ir_node *block;
	int finished_callees;
	pred_states finished_preds;
	double irg_exec_freq;

	ir_loop **cur_loops;
};

typedef enum node_data_type {
	CALLEE, STACK_ACCESS, NON_STACK_ACCESS
} node_data_type;

typedef struct spm_var_info {
	node_data_type data_type;
	void *identifier;
	int size;
} spm_var_info;

typedef struct spm_content {
	list_head list;
	int addr;
	int next_use; //How many blocks away basically
	int gap_size;
	spm_var_info *content;
} spm_content;

typedef enum transfer_direction {
	IN, OUT, MOV,
} transfer_direction;

typedef struct spm_transfer {
	transfer_direction direction;
	ir_entity *identifier;
	node_data_type type;
	int spm_addr_from;
	int spm_addr_to;

} spm_transfer;

struct alloc_result {
	int free_space;
	pset_new_t *spm_set;
	pset_new_t *modified_set; //spm content (set contains spm_var_info elmnts) which has to be written back on eviction
	pset_new_t *write_first_set;
	pmap *copy_in;
	pmap *swapout_set;
	//At end of block compensation code may be added
	spm_transfer **compensation_transfers;
	//Set only temporarily needed:
	pset_new_t *retain_set;
	list_head spm_content_head;
};

struct node_data {
	node_data_type data_type;
	list_head list;
	void *identifier; //TODO: change?
	int size; //in bytes
	bool modified;
	bool write_first; //TODO: write happens before every read in region
	int access_cnt;
	double freq_per_byte; // = access_cnt / size
};

typedef struct block_data {
	list_head *node_lists;
	int callee_cnt;
	double max_exec_freq;
	alloc_result **allocation_results;
	pset_new_t *compensation_callees; //callees which have to be surrounded by compensation code
	pset_new_t *dead_set; //Vars last accessed in this block //TODO: Array?
} block_data;

typedef struct loop_data {
	ir_node *loop_header;
	ir_node **blocks;
	pset_new_t *mem_accesses;
	//Transfers needed before loop
	spm_transfer **transfers;
} loop_data;

typedef struct drpg_walk_env {
	deq_t workqueue;
	pmap *block_data_map;
	timestamp *cur_branch;
	pmap *loop_info;
} dprg_walk_env;

struct spm_properties_t {
	int start_addr;
	int size;
	int latency_diff; //ram_lat - spm_lat
	double throughput_ram;
	double throughput_spm;
};
#define ONE_MB 1024 * 1024
//TODO: better initial values
static struct spm_properties_t spm_properties = {
	.start_addr = 512 * ONE_MB,
	.size = ONE_MB,
	.latency_diff = 20,
	.throughput_ram = 1.0f,
	.throughput_spm = 1.0f,
};

static pmap *spm_var_infos;


node_data *(*retrieve_spm_node_data)(ir_node *);

#define get_last_block_allocation(blk_data) blk_data->allocation_results[blk_data->callee_cnt];


static node_data *spm_get_node_data_by_type(node_data_type type, void *id, int size, bool modified)
{
	node_data *data = XMALLOC(node_data);
	data->data_type = type;
	data->identifier = id;
	data->size = size;
	data->modified = modified;
	data->access_cnt = 1;
	data->write_first = modified;
	return data;
}

static node_data *spm_get_var_node_data(node_data_type type, void *id, int size, bool modified)
{
	if (!pmap_contains(spm_var_infos, id)) {
		spm_var_info *info = XMALLOC(spm_var_info);
		info->identifier = id;
		info->size = size;
		info->data_type = type;
		pmap_insert(spm_var_infos, id, info);
	}
	return spm_get_node_data_by_type(type, id, size, modified);
}

node_data *spm_get_mem_read_node_data_stack(void *id, int size)
{
	return spm_get_var_node_data(STACK_ACCESS, id, size, false);
}

node_data *spm_get_mem_write_node_data_stack(void *id, int size)
{
	return spm_get_var_node_data(STACK_ACCESS, id, size, true);
}

node_data *spm_get_mem_read_node_data(void *id, int size)
{
	return spm_get_var_node_data(NON_STACK_ACCESS, id, size, false);
}

node_data *spm_get_mem_write_node_data(void *id, int size)
{
	return spm_get_var_node_data(NON_STACK_ACCESS, id, size, true);
}

node_data *spm_get_callee_node_data(ir_entity *ent)
{
	return spm_get_node_data_by_type(CALLEE, ent, 0, false);
}

static spm_content *list_next_spm_content(spm_content *element, list_head *head)
{
	list_head *next_el = element->list.next;
	if (next_el != head)
		return list_entry(next_el, spm_content, list);
	return NULL;
}

static void print_spm_content_list(list_head *head)
{
	list_for_each_entry(spm_content, var, head, list) {
		printf("(%d, s:%d, g:%d),", var->addr, var->content->size, var->gap_size);
	}
	printf("\n");
}

static pmap *middleend_irgs;

void spm_calculate_dprg_info()
{
	analyse_loop_nesting_depth();
	/*
		//store middleend irg -> entity mapping so
		//we can later do a mapping from middleend irg to beirg.
		//Necessary as callgraph data is stored with pointers to irgs
		middleend_irgs = pmap_create();
		foreach_irp_irg(i, irg) {
			if(get_irg_entity(irg)) {
				pmap_insert(middleend_irgs, irg, get_irg_entity(irg));
				printf("Insterted: (%p,%p) %s\n", irg, get_irg_entity(irg), gdb_node_helper(get_irg_entity(irg)));
			}
		}*/
}
/*
static void fix_call_graph_data(void)
{
	foreach_irp_irg(i, irg) {
		printf("graph %s\n", gdb_node_helper(irg));
		printf("caller\n");
		for(unsigned i = 0; i < ARR_LEN(irg->callers); i++) {
			ir_graph *old_irg = irg->callers[i];
			printf("Looking up: %p\n", old_irg);
			if(pmap_contains(middleend_irgs, old_irg))
				irg->callers[i] =  get_entity_irg(pmap_get(ir_entity, middleend_irgs, old_irg));
		}
		printf("callee\n");
		for(unsigned i = 0; i < ARR_LEN(irg->callees); i++) {
			ir_graph *old_irg = irg->callees[i]->irg;
			printf("Looking up: %p\n", old_irg);
			if(pmap_contains(middleend_irgs, old_irg))
				irg->callees[i]->irg =  get_entity_irg(pmap_get(ir_entity, middleend_irgs, old_irg));
		}

	}
	pmap_destroy(middleend_irgs);
}
*/
static node_data *spm_collect_node_data(ir_node *node, block_data *b_data)
{
	node_data *n_data = retrieve_spm_node_data(node);
	if (n_data) {
		if (n_data->data_type == CALLEE) {
			b_data->callee_cnt++;
			list_add(&n_data->list, b_data->node_lists);
		}
		else {
			//sort non callee node data by size (between two callee nodes)
			list_for_each_entry(node_data, n_data_iter, b_data->node_lists, list) {
				if (n_data_iter->data_type == CALLEE) {
					list_add_tail(&n_data->list, &n_data_iter->list);
				}
				if (n_data_iter->size < n_data->size) { //does node always gets insterted here?
					list_add_tail(&n_data->list, &n_data_iter->list);
				}
				if (n_data_iter->identifier == n_data->identifier) {
					n_data_iter->access_cnt++;
					n_data_iter->modified |= n_data->modified;
					free(n_data);
				}
			}
			if (list_empty(b_data->node_lists)) {
				list_add(&n_data->list, b_data->node_lists);
			}
		}
	}
	return n_data;
}

typedef struct block_walk_env {
	pmap *block_data_map;
	pset_new_t *stack_vars;
} block_walk_env;

static void spm_collect_block_data(ir_node *block, void *env)
{
	block_walk_env *bw_env = env;

	block_data *b_data = XMALLOC(block_data);
	b_data->node_lists = NEW_ARR_F(list_head, 1);
	INIT_LIST_HEAD(b_data->node_lists);
	b_data->callee_cnt = 0;
	b_data->dead_set = NULL;
	b_data->max_exec_freq = 0.0;
	b_data->allocation_results = NULL;
	b_data->compensation_callees = NULL;
	pmap_insert(bw_env->block_data_map, block, b_data);

	sched_foreach(block, irn) {
		node_data *n_data = spm_collect_node_data(irn, b_data);

		if (n_data && n_data->data_type == STACK_ACCESS) {
			pset_new_insert(bw_env->stack_vars, pmap_get(spm_var_info, spm_var_infos, n_data->identifier));
		}
	}
}

static void pset_insert_set(pset_new_t *a, pset_new_t *b)
{
	pset_new_iterator_t iter;
	void *el;
	foreach_pset_new(b, void *, el, iter) {
		pset_new_insert(a, el);
	}
}

static void spm_collect_irg_data(ir_graph *irg, void *env)
{
	block_walk_env blk_env = {
		.block_data_map = env,
		.stack_vars = XMALLOC(pset_new_t),
	};
	pset_new_init(blk_env.stack_vars);
	irg_block_walk_graph(irg, spm_collect_block_data, NULL, &blk_env);

	ir_node *end_block = get_irg_end_block(irg);
	for (int i = 0; i < get_Block_n_cfgpreds(end_block); i++) {
		ir_node *pred_block = get_Block_cfgpred_block(end_block, i);
		block_data *b_data = pmap_get(block_data, blk_env.block_data_map, pred_block);
		pset_new_t *dead_set = XMALLOC(pset_new_t);
		pset_new_init(dead_set);
		b_data->dead_set = dead_set;
		pset_insert_set(dead_set, blk_env.stack_vars);
	}


	pset_new_destroy(blk_env.stack_vars);
	free(blk_env.stack_vars);
}

typedef struct callgraph_walk_env {
	double irg_freq;
	pmap *block_data_map;
	pmap *call_blocks;
	bool is_in_recursion;
} callgraph_walk_env;

static void spm_calc_glb_exec_freq(ir_graph *irg, void *env);

static void spm_calc_glb_blk_exec_freq(ir_node *block, void *env)
{
	callgraph_walk_env *c_env = env;
	double freq = c_env->irg_freq * get_block_execfreq(block);
	block_data *b_data = pmap_get(block_data, c_env->block_data_map, block);
	if (freq > b_data->max_exec_freq)
		b_data->max_exec_freq = freq;

	list_head *node_list = &b_data->node_lists[0];

	list_for_each_entry(node_data, n_data, node_list, list) {
		ir_graph *irg = get_entity_irg((ir_entity *) n_data->identifier);
		if (!irg)
			continue;
		ir_node *start_block = get_irg_start_block(irg);
		block_data *callee_data = pmap_get(block_data, c_env->block_data_map, start_block);
		if (freq > callee_data->max_exec_freq) {
			callee_data->max_exec_freq = freq;
			//pmap_insert(c_env->call_blocks, irg, callee_data);
			callgraph_walk_env callg_env = {
				.irg_freq = freq,
				.block_data_map = c_env->block_data_map,
				.call_blocks = NULL,//pmap_create(),
				.is_in_recursion = false,//is_irg_callee_backedge(irg, i),
			};
			spm_calc_glb_exec_freq(irg, &callg_env);
		}
	}

	if (!c_env->is_in_recursion)
		return;

	//in recursion we delete all node_data in access lists.
	//Very conservative, as code executed in recursive function won't access spm.
	for (int i = 1; i < b_data->callee_cnt + 2; i++) {
		node_list = &b_data->node_lists[i];
		list_for_each_entry_safe(node_data, n_data, tmp, node_list, list) {
			free(n_data);
		}
		INIT_LIST_HEAD(node_list);
	}
}

static void spm_calc_glb_exec_freq(ir_graph *irg, void *env)
{
	irg_block_walk_graph(irg, spm_calc_glb_blk_exec_freq, NULL, env);
}

/* Next two functions copied from callgraph.c and edited */
static void glb_exec_freq_walk(ir_graph *irg, void *env)
{
	callgraph_walk_env *callg_env = env;
	spm_calc_glb_exec_freq(irg, env);
	/*
		for (size_t i = 0, n_callees = get_irg_n_callees(irg); i < n_callees; i++) {
			ir_graph *m = get_irg_callee(irg, i);
			if (pmap_contains(callg_env->call_blocks, m)) {
				callgraph_walk_env c_env = {
					.irg_freq = pmap_get(block_data, callg_env->call_blocks, m)->max_exec_freq,
					.block_data_map = callg_env->block_data_map,
					.call_blocks = NULL;//pmap_create(),
					.is_in_recursion = false;//is_irg_callee_backedge(irg, i),
				};
				glb_exec_freq_walk(m, &c_env);
				//pmap_destroy(c_env.call_blocks);
			}
		}*/
}

static void spm_glb_exec_freq_walk(pmap *block_data_map)
{
	/* roots are methods which have no callers in the current program */
	foreach_irp_irg(i, irg) {
		if (get_irg_n_callers(irg) == 0) { //Note: callerinfo correct, only callee wrong
			callgraph_walk_env c_env = {
				.irg_freq = 1.0,
				.block_data_map = block_data_map,
				.call_blocks = NULL,//pmap_create(),
				.is_in_recursion = false,
			};
			spm_calc_glb_exec_freq(irg, &c_env);
			//pmap_destroy(c_env.call_blocks);
		}
	}

	/* in case of unreachable call loops we haven't visited some irgs yet */
	/*foreach_irp_irg(i, irg) {
		callgraph_walk_env c_env = {
			.irg_freq = 1.0,
			.block_data_map = block_data_map,
			.call_blocks = pmap_create(),
			.is_in_recursion = false,
		};
		glb_exec_freq_walk(irg, &c_env);
		pmap_destroy(c_env.call_blocks);
	}*/
}
//TODO: Rename function. does a bit more than accessfreq
static void spm_calc_blocks_access_freq(pmap *block_data_map)
{
	foreach_pmap(block_data_map, cur_entry) {
		block_data *blk_data = cur_entry->value;
		//TODO: alloc_results init doesn't fit in here?
		blk_data->allocation_results = NEW_ARR_FZ(alloc_result *, blk_data->callee_cnt + 1);
		//idx 0: call nodes, idx 1 to x are mem_access in between
		bool node_list_empty = list_empty(blk_data->node_lists);
		ARR_RESIZE(list_head, blk_data->node_lists, blk_data->callee_cnt + 2);
		//As arr_resize can change addr of node_lists, pointers have to be adjusted accordingly
		list_head *callee_list = blk_data->node_lists;
		if (node_list_empty) {
			INIT_LIST_HEAD(callee_list);
		}
		else {
			callee_list->prev->next = callee_list;
			callee_list->next->prev = callee_list;
		}
		for (int i = 1; i < blk_data->callee_cnt + 2; i++) {
			INIT_LIST_HEAD(&blk_data->node_lists[i]);
		}
		int cur_callee_cnt = 0;
		list_for_each_entry_safe(node_data, n_data, tmp, callee_list, list) {
			if (n_data->data_type == CALLEE) {
				cur_callee_cnt++;
				continue;
			}
			list_head *node_list = &blk_data->node_lists[cur_callee_cnt + 1];
			n_data->freq_per_byte = (float) n_data->access_cnt / n_data->size;
			if (list_empty(node_list)) {
				list_move(&n_data->list, node_list);
			}
			else {
				list_for_each_entry(node_data, n_data_iter, node_list, list) {
					if (n_data_iter->freq_per_byte < n_data->freq_per_byte) {
						list_move_tail(&n_data->list, &n_data_iter->list);
						break;
					}
					//adding el to tail of list, if end of list is reached
					if (n_data_iter->list.next == node_list)
						list_move_tail(&n_data->list, node_list);
				}
			}

		}
	}
}

static int get_set_size_in_bytes(pset_new_t *node_data_set)
{
	pset_new_iterator_t iter;
	node_data *el;
	int size = 0;
	foreach_pset_new(node_data_set, node_data *, el, iter) {
		size += el->size;
	}
	return size;
}

static double get_spm_benefit(dprg_walk_env *env, alloc_result *alloc_res, node_data *n_data, node_data *swapout_candidate)
{
	timestamp *branch = env->cur_branch;
	block_data *blk_data = pmap_get(block_data, env->block_data_map, branch->block);
	double block_exec_freq = blk_data->max_exec_freq;
	double latency_gain = block_exec_freq * n_data->access_cnt * spm_properties.latency_diff;

	double migration_overhead = spm_properties.throughput_spm * n_data->size; //TODO: find approximation
	//Access_cnt of swapout candidate in this block
	int swapout_acc_cnt = 0;
	if (swapout_candidate) {
		if (pset_new_contains(alloc_res->modified_set, swapout_candidate))
			migration_overhead += spm_properties.throughput_ram * swapout_candidate->size;

		list_head *node_list = &blk_data->node_lists[branch->finished_callees + 1];
		list_for_each_entry(node_data, n_data_iter, node_list, list) {
			if (n_data_iter->identifier == swapout_candidate->identifier) {
				swapout_acc_cnt = n_data_iter->access_cnt;
				break;
			}
		}
	}
	double latency_loss = swapout_acc_cnt * block_exec_freq * spm_properties.latency_diff;
	return latency_gain - latency_loss - migration_overhead;
}

static node_data *get_node_data_by_spm_var(dprg_walk_env *env, spm_var_info *var_info)
{
	block_data *blk_data = pmap_get(block_data, env->block_data_map, env->cur_branch->block);
	list_head *node_list = &blk_data->node_lists[env->cur_branch->finished_callees + 1];

	list_for_each_entry(node_data, n_data, node_list, list) {
		if (n_data->identifier == var_info->identifier)
			return n_data;
	}
	return NULL; //Should really never happen
}

static int spm_var_cmp(const void *p1, const void *p2)
{
	return QSORT_CMP((*(spm_var_info * const *) p1)->size, (*(spm_var_info * const *) p2)->size);
}

static spm_transfer *create_spm_transfer_in(spm_content *content)
{
	spm_transfer *transfer = XMALLOC(spm_transfer);
	transfer->spm_addr_to = content->addr;
	transfer->identifier = (ir_entity *) content->content->identifier; //TODO: should always be ir_entity
	transfer->direction = IN;
	transfer->type = content->content->data_type;
	return transfer;
}

static spm_transfer *create_spm_transfer_out(spm_content *content)
{
	spm_transfer *transfer = XMALLOC(spm_transfer);
	transfer->spm_addr_from = content->addr;
	transfer->identifier = (ir_entity *) content->content->identifier;
	transfer->direction = OUT;
	transfer->type = content->content->data_type;
	return transfer;
}

static spm_transfer *create_spm_transfer_mov(spm_content *from, spm_content *to)
{
	spm_transfer *transfer = XMALLOC(spm_transfer);
	transfer->spm_addr_from = from->addr;
	transfer->spm_addr_to = to->addr;
	transfer->identifier = (ir_entity *) from->content->identifier;
	transfer->direction = MOV;
	transfer->type = from->content->data_type;
	return transfer;
}

static spm_content *spm_content_insert(spm_var_info *var_info, int gap, spm_content *head)
{
	spm_content *new_spm_content = XMALLOC(spm_content);
	list_add(&new_spm_content->list, &head->list);
	new_spm_content->addr = head->addr + head->content->size;
	new_spm_content->content = var_info;
	new_spm_content->gap_size = gap;
	head->gap_size = 0;
	return new_spm_content;
}

#define no_candidate(alloc_res, candidate) (pset_new_contains(alloc_res->retain_set, candidate) \
		|| pmap_contains(alloc_res->copy_in, candidate))

//How to use spm_content allocation:
//next use is stored for every var
//window of spwapin size is dragged over spm_list to get the element(s) furthest away from use
//
//or: (this alternative is implemented)
//get possible sites where gap later will be minimal and then calculate next use values on the fly x call sites deep (or 10 blocks or so)
//This will lead to eviction of possible more vars then necessary, if write to (var + gap) leaves small gap, whereas (var + var) leaves smaller/no gap
//
//new idea: calc benefit for each window start and include compaction/other movements into evaluation
static void spm_force_insert(dprg_walk_env *env, alloc_result *result, node_data *swapin)
{
	(void) env;

	//TODO: better walk of list. right now it doesn't work. list_entry use not safe. use safe version
	spm_content **best_fit_gap_el = NEW_ARR_FZ(spm_content *, 1);
	int cur_min_gap_size = INT_MAX;
	//start at first real element (after sentinel)
	spm_content *prev = list_entry(&result->spm_content_head.next, spm_content, list);
	spm_content *sentinel = prev;
	printf("swapin size: %d\n", swapin->size);
	print_spm_content_list(&result->spm_content_head);
	list_for_each_entry(spm_content, var, &result->spm_content_head, list) {
		//list starts with sentinel, but we want to start at first elmnt after it
		if (var == sentinel || no_candidate(result, var))
			continue;
		int swapout_size = prev->gap_size + var->content->size + var->gap_size;
		spm_content *next = var;
		while (swapout_size < swapin->size) {
			next = list_next_spm_content(next, &result->spm_content_head);
			if (!next || no_candidate(result, next))
				break;
			swapout_size += next->content->size + next->gap_size;
		}
		int new_gap = swapout_size - swapin->size;
		if (new_gap < 0)
			continue;
		if (new_gap < cur_min_gap_size) {
			DEL_ARR_F(best_fit_gap_el);
			best_fit_gap_el = NEW_ARR_F(spm_content *, 1);
			best_fit_gap_el[0] = var;
			cur_min_gap_size = new_gap;
		}
		else if (new_gap == cur_min_gap_size) {
			ARR_APP1(spm_content *, best_fit_gap_el, var);
		}
		prev = var;
	}

	//TODO: if best_fit_gap_el size > 1 calculate next uses
	//TODO: find benefit has to be integrated as well here
	spm_content *best_swapout_candidate = best_fit_gap_el[0];
	if (!best_swapout_candidate) //TODO: maybe have to do more than that
		return;
	//TODO: dont go by sizes, instead define a swapout region in the list
	prev = list_entry(&best_swapout_candidate->list.prev, spm_content, list);
	int swapout_size = prev->gap_size + best_swapout_candidate->content->size + best_swapout_candidate->gap_size;
	spm_content *next = list_entry(&best_swapout_candidate->list.next, spm_content, list);
	list_del(&best_swapout_candidate->list);
	pmap_insert(result->swapout_set, best_swapout_candidate->content, create_spm_transfer_out(best_swapout_candidate));
	free(best_swapout_candidate);
	while (swapout_size < swapin->size) {
		swapout_size += next->content->size + next->gap_size;
		spm_content *tmp = next;
		next = list_entry(&next->list.next, spm_content, list);
		list_del(&tmp->list);
		pmap_insert(result->swapout_set, tmp->content, create_spm_transfer_out(tmp));
		free(tmp);
	}
	int new_gap = swapout_size - swapin->size;
	spm_var_info *swapin_info = pmap_get(spm_var_info, spm_var_infos, swapin->identifier);
	spm_content *new_content = spm_content_insert(swapin_info, new_gap, prev);
	pmap_insert(result->copy_in, swapin_info, create_spm_transfer_in(new_content));
	if (swapin->modified)
		pset_new_insert(result->modified_set, swapin_info);
	if (swapin->write_first)
		pset_new_insert(result->write_first_set, swapin_info);
	result->free_space += new_gap;
}

static bool spm_try_best_fit_insert(alloc_result *result, node_data *var)
{
	spm_var_info *el_info = pmap_get(spm_var_info, spm_var_infos, var->identifier);
	spm_content *best_fit_gap_el = NULL;
	int best_fit_gap = INT_MAX;
	list_for_each_entry(spm_content, var, &result->spm_content_head, list) {
		int new_gap = var->gap_size - el_info->size;
		if (new_gap >= 0 && new_gap < best_fit_gap) {
			best_fit_gap = new_gap;
			best_fit_gap_el = var;
			if (new_gap == 0)
				break;
		}
	}
	//If space found insert el
	if (best_fit_gap != INT_MAX) {
		spm_content *new = spm_content_insert(el_info, best_fit_gap, best_fit_gap_el);
		pmap_insert(result->copy_in, el_info, create_spm_transfer_in(new));
		result->free_space -= el_info->size;
		if (var->modified)
			pset_new_insert(result->modified_set, el_info);
		if (var->write_first)
			pset_new_insert(result->write_first_set, el_info);
		printf("best fit insert: ");
		print_spm_content_list(&result->spm_content_head);
		return true;
	}
	return false;
}

static alloc_result *create_alloc_result(void)
{
	alloc_result *result = XMALLOC(alloc_result);
	result->spm_set = XMALLOC(pset_new_t);
	result->modified_set = XMALLOC(pset_new_t);
	result->write_first_set = XMALLOC(pset_new_t);
	result->copy_in = pmap_create();
	result->swapout_set = pmap_create();
	result->free_space = spm_properties.size;
	pset_new_init(result->spm_set);
	pset_new_init(result->modified_set);
	pset_new_init(result->write_first_set);
	result->compensation_transfers = NULL;
	INIT_LIST_HEAD(&result->spm_content_head);
	return result;
}

static alloc_result *spm_calc_alloc_block(dprg_walk_env *env)
{
	timestamp *branch = env->cur_branch;
	ir_node *block = branch->block;
	block_data *blk_data = pmap_get(block_data, env->block_data_map, block);
	printf("calc alloc block %s\n", gdb_node_helper(block));

	alloc_result *result = create_alloc_result();
	result->retain_set = XMALLOC(pset_new_t);
	pset_new_init(result->retain_set);

	block_data *prev_blk_data = NULL;
	alloc_result *pred_result = branch->last_alloc;
	if (branch->last_block) {
		prev_blk_data = pmap_get(block_data, env->block_data_map, branch->last_block);
	}

	//Handle deadset at beginning of block (TODO: deadset also per region?!)
	//TODO: how to handle end of call? (do deadset handling also at end of function)
	bool handle_deadset = prev_blk_data && branch->finished_callees == 0 && prev_blk_data->dead_set;
	int deadset_size = 0;
	//fill result with pred_result values
	if (pred_result) {
		printf("pred content: ");
		print_spm_content_list(&pred_result->spm_content_head);
		pset_insert_set(result->modified_set, pred_result->modified_set);
		//build spm_content
		list_for_each_entry(spm_content, var, &pred_result->spm_content_head, list) {
			if (handle_deadset && pset_new_contains(prev_blk_data->dead_set, var->content)) {
				spm_content *prev_el = list_entry(var->list.prev, spm_content, list);
				prev_el->gap_size += var->content->size + var->gap_size;
				deadset_size += var->content->size;
				continue;
			}
			spm_content *spm_content_el = XMALLOC(spm_content);
			spm_content_el->addr = var->addr;
			spm_content_el->gap_size = var->gap_size;
			spm_content_el->content = var->content;
			list_add_tail(&spm_content_el->list, &result->spm_content_head);
			pset_new_insert(result->spm_set, spm_content_el->content);
		}
		result->free_space = pred_result->free_space + deadset_size;
	}
	else {
		spm_content *sentinel = XMALLOC(spm_content);
		sentinel->addr = 0;
		sentinel->gap_size = spm_properties.size;
		spm_var_info *null_info = XMALLOC(spm_var_info);
		null_info->identifier = NULL;
		null_info->size = 0;
		null_info->data_type = NON_STACK_ACCESS;
		sentinel->content = null_info;
		list_add_tail(&sentinel->list, &result->spm_content_head);
	}

	//TODO: spm_var_info pointer in node_data to avoid hashmap
	pset_new_iterator_t iter;
	node_data *el;
	if (prev_blk_data && branch->finished_callees == 0 && prev_blk_data->dead_set) {
		foreach_pset_new(prev_blk_data->dead_set, node_data *, el, iter) {
			spm_var_info *el_info = pmap_get(spm_var_info, spm_var_infos, el->identifier);
			if (pset_new_contains(result->spm_set, el_info)) {
				pset_new_remove(result->spm_set, el_info);
				result->free_space += el->size;
			}
		}
	}

	list_head *node_list = &blk_data->node_lists[branch->finished_callees + 1];

	list_for_each_entry(node_data, n_data, node_list, list) {
		if (n_data->size > spm_properties.size)
			continue;

		spm_var_info *el_info = pmap_get(spm_var_info, spm_var_infos, n_data->identifier);
		if (pset_new_contains(result->spm_set, el_info)) {
			if (!pmap_contains(result->swapout_set, el_info)) {
				pset_new_insert(result->retain_set, el_info);
				if (n_data->modified)
					pset_new_insert(result->modified_set, el_info);
			}
		}
		else {
			if (n_data->size <= result->free_space) {
				if (get_spm_benefit(env, result, n_data, NULL) > 0.0f)
					if (!spm_try_best_fit_insert(result, n_data))
						spm_force_insert(env, result, n_data);
			}
			else {
				spm_force_insert(env, result, n_data);
			}
		}
	}

	foreach_pmap(result->copy_in, entry) {
		pset_new_insert(result->spm_set, (void *) entry->key);
	}
	foreach_pmap(result->swapout_set, entry) {
		pset_new_remove(result->spm_set, (void *) entry->key);
		if (!pset_new_contains(result->modified_set, (void *) entry->key)) {
			free(entry->value);
			entry->value = NULL;
		}
	}

	//Retain set only temporarily needed
	pset_new_destroy(result->retain_set);
	free(result->retain_set);

	return result;
}

static ir_entity *get_next_call_from_block(dprg_walk_env *env)
{
	timestamp *branch = env->cur_branch;
	ir_node *block = branch->block;
	block_data *blk_data = pmap_get(block_data, env->block_data_map, block);
	int callee_cnt = blk_data->callee_cnt;
	if (!callee_cnt || callee_cnt == branch->finished_callees)
		return NULL;

	int i = 0;
	list_for_each_entry(node_data, n_data, blk_data->node_lists, list) {
		if (i == branch->finished_callees)
			return (ir_entity *) n_data->identifier;
		i++;
	}
	return NULL;
}

static spm_content *find_var_in_spm_content(list_head *spm_list, spm_var_info *var)
{
	list_for_each_entry(spm_content, var_in_list, spm_list, list) {
		if (var_in_list->content == var)
			return var_in_list;
	}
	return NULL;
}

static spm_transfer **join_allocations(alloc_result *base_alloc, alloc_result *adj_alloc)
{
	//TODO: Clean-up
	spm_transfer **transfers = NEW_ARR_F(spm_transfer *, 0);

	spm_transfer *transfer;
	list_head *next_list_el_adj = adj_alloc->spm_content_head.next;
	spm_content *next_el_adj_alloc = list_entry(next_list_el_adj, spm_content, list);
	list_for_each_entry(spm_content, var, &base_alloc->spm_content_head, list) {
		while (next_el_adj_alloc->addr < var->addr) {
			if (!pset_new_contains(base_alloc->spm_set, next_el_adj_alloc->content)
			        && pset_new_contains(adj_alloc->modified_set, next_el_adj_alloc->content)) {
				transfer = create_spm_transfer_out(next_el_adj_alloc);
				ARR_APP1(spm_transfer *, transfers, transfer);
			}
			if (next_list_el_adj->next != &adj_alloc->spm_content_head) {
				next_list_el_adj = next_list_el_adj->next;
				next_el_adj_alloc = list_entry(next_list_el_adj, spm_content, list);
			}
			else {
				break;
			}
		}

		if (next_el_adj_alloc->content == var->content
		        && next_el_adj_alloc->addr == var->addr) {
			if (next_list_el_adj->next != &adj_alloc->spm_content_head) {
				next_list_el_adj = next_list_el_adj->next;
				next_el_adj_alloc = list_entry(next_list_el_adj, spm_content, list);
			}
			continue;
		}

		if (pset_new_contains(adj_alloc->spm_set, var->content)) {
			spm_content *el_in_adj_alloc = find_var_in_spm_content(&adj_alloc->spm_content_head, var->content);
			transfer = create_spm_transfer_mov(el_in_adj_alloc, var);
		}
		else {
			transfer = create_spm_transfer_in(var);
		}
		ARR_APP1(spm_transfer *, transfers, transfer);
	}

	//There might be more elemenst in adj_alloc spm which have to be written back
	while (next_list_el_adj != &adj_alloc->spm_content_head) {
		next_el_adj_alloc = list_entry(next_list_el_adj, spm_content, list);
		if (!pset_new_contains(base_alloc->spm_set, next_el_adj_alloc->content)
		        && pset_new_contains(adj_alloc->modified_set, next_el_adj_alloc->content)) {
			transfer = create_spm_transfer_out(next_el_adj_alloc);
			ARR_APP1(spm_transfer *, transfers, transfer);
		}
		next_list_el_adj = next_list_el_adj->next;
	}

	return transfers;
}

static void join_pred_allocations(dprg_walk_env *env, ir_node *base_block)
{
	timestamp *branch = env->cur_branch;
	ir_node *block = branch->block;
	block_data *base_block_data = pmap_get(block_data, env->block_data_map, base_block);
	alloc_result *base_block_res = get_last_block_allocation(base_block_data);
	//Create comp alloc for all blocks except base_block
	for (int i = 1; i < get_Block_n_cfgpreds(block); i++) {
		ir_node *pred_block = get_Block_cfgpred_block(block, i);
		if (pred_block == base_block)
			continue;

		block_data *pred_block_data = pmap_get(block_data, env->block_data_map, pred_block);
		alloc_result *pred_block_res = get_last_block_allocation(pred_block_data);
		spm_transfer **transfers = join_allocations(base_block_res, pred_block_res);
		if (ARR_LEN(transfers) > 0)
			pred_block_res->compensation_transfers = transfers;
		else
			DEL_ARR_F(transfers);
	}
}

static ir_node *spm_join_return_blocks(dprg_walk_env *env)
{
	timestamp *branch = env->cur_branch;
	ir_node *block = branch->block;
	//We select block with highest exec freq
	ir_node *base_block = get_Block_cfgpred_block(block, 0);
	for (int i = 1; i < get_Block_n_cfgpreds(block); i++) {
		ir_node *pred_block = get_Block_cfgpred_block(block, i);
		if (get_block_execfreq(pred_block) > get_block_execfreq(base_block))
			base_block = pred_block;
	}
	join_pred_allocations(env, base_block);
	return base_block;
}

static void spm_join_cond_blocks(dprg_walk_env *env)
{
	timestamp *branch = env->cur_branch;
	//ir_node *block = branch->block;
	//We select block of last timestamp as base block TODO: Better choice?
	//Idea: select allocation which enhances perf of block the most (iterate var_accesses of block)
	join_pred_allocations(env, branch->last_block);
}

static void pmap_del_and_free(pmap *map, void *key)
{
	void *element = pmap_get(void, map, key);
	if (element) {
		pmap_insert(map, key, NULL);
		free(element);
	}
}

static int spm_content_cmp(const void *p1, const void *p2)
{
	return QSORT_CMP((*(spm_content * const *) p1)->addr, (*(spm_content * const *) p2)->addr);
}

static void spm_join_loop(dprg_walk_env *env)
{
	//TODO: handling of modified set?
	//TODO: needs a bit of cleanup
	timestamp *branch = env->cur_branch;
	ir_node *block = branch->block;
	//1. get last alloc inside loop
	ir_node *last_loop_block;
	for (int i = 0; i < get_Block_n_cfgpreds(block); i++) {
		if (is_backedge(block, i))
			last_loop_block = get_Block_cfgpred_block(block, i);
	}
	assert(last_loop_block);
	ir_loop *cur_loop = branch->cur_loops[ARR_LEN(branch->cur_loops) - 1];
	loop_data *l_data = pmap_get(loop_data, env->loop_info, cur_loop);
	assert(l_data);

	block_data *last_loop_b_data = pmap_get(block_data, env->block_data_map, last_loop_block);
	alloc_result *last_loop_alloc = get_last_block_allocation(last_loop_b_data);
	//2. Check which vars accessed inside loop are still in spm
	spm_content **loop_vars = NEW_ARR_F(spm_content *, 0);
	list_for_each_entry(spm_content, var, &last_loop_alloc->spm_content_head, list) {
		if (pset_new_contains(l_data->mem_accesses, var->content))
			ARR_APP1(spm_content *, loop_vars, var);
	}
	//3. For those vars space will be made in all loop allocations
	//Var could have been evicted and then reinserted at another place
	//probably best solution is to just change allocation to match end of loop alloc
	size_t loop_var_cnt = ARR_LEN(loop_vars);
	if (loop_var_cnt == 0)
		return;
	QSORT_ARR(loop_vars, spm_content_cmp);

	pset_new_t vars_to_evict;
	pset_new_init(&vars_to_evict);
	for (size_t i = 0; i < ARR_LEN(l_data->blocks); i++) {
		ir_node *loop_block = l_data->blocks[i];
		block_data *loop_b_data = pmap_get(block_data, env->block_data_map, loop_block);
		for (int j = 0; j <= loop_b_data->callee_cnt; j++) {
			alloc_result *alloc = loop_b_data->allocation_results[j];
			bool handled_all_loop_vars = false;
			if (alloc == last_loop_alloc)
				continue;

			//Loop vars will be transferred into spm before loop header
			for (size_t k  = 0; k < loop_var_cnt; k++) {
				pmap_del_and_free(alloc->copy_in, loop_vars[k]->content);
				pmap_del_and_free(alloc->swapout_set, loop_vars[k]->content);
			}
			//For each loop var we make sure it at the right place in alloc
			size_t k = 0;
			spm_content *loop_var = loop_vars[k];
			spm_content *sentinel = list_entry(alloc->spm_content_head.next, spm_content, list);
			//TODO: check code again. jump over sentinel is ugly
			list_for_each_entry_safe(spm_content, var, tmp, &alloc->spm_content_head, list) {
				if (var == sentinel)
					continue;
				if (var->addr != loop_var->addr) {
					for (size_t l  = 0; l < loop_var_cnt; l++) {
						//Loop var is at wrong place, we delete it here from alloc
						if (var->content == loop_vars[l]->content) {
							spm_content *prev_spm_element = list_entry(var->list.prev, spm_content, list);
							prev_spm_element->gap_size += var->content->size;
							alloc->free_space += var->content->size;
							list_del(&var->list);
							free(var);
						}
					}
				}
				if (handled_all_loop_vars)
					continue; //TODO: why not break?
				//Currently looked at loop var should be further back in list
				if (var->addr < loop_var->addr)
					continue;
				if (var->addr == loop_var->addr && var->content == loop_var->content) {
					loop_var = loop_vars[++k];
					if (k == loop_var_cnt)
						handled_all_loop_vars = true;
					continue;
				}
				//var is now at address directly at/after loop_var should be
				//we make space for loop_var here
				spm_content *cur_eviction_el = var;
				spm_content *prev_spm_element = list_entry(var->list.prev, spm_content, list);
				//checks wether loop_var starts in gap or wether prev element as to be evicted as well
				int prev_el_size = 0;
				if (var->addr != 0) //special sentinel element handling
					prev_el_size = prev_spm_element->content->size;
				if (prev_spm_element->addr + prev_el_size > loop_var->addr) {
					cur_eviction_el = prev_spm_element;
					prev_spm_element = list_entry(prev_spm_element->list.prev, spm_content, list);
				}
				spm_content *next_spm_element = list_next_spm_content(cur_eviction_el, &alloc->spm_content_head);
				int content_end = loop_var->addr + loop_var->content->size;
				while (cur_eviction_el->addr < content_end) {
					list_del(&cur_eviction_el->list);
					pset_new_remove(alloc->spm_set, cur_eviction_el->content);
					pmap_del_and_free(alloc->copy_in, cur_eviction_el->content);
					pmap_del_and_free(alloc->swapout_set, cur_eviction_el->content);
					if (pset_new_contains(alloc->modified_set, cur_eviction_el->content))
						pset_new_insert(&vars_to_evict, cur_eviction_el);
					//free(cur_eviction_el); free after vars_to_evict. SIGABRT here... why?
					alloc->free_space += cur_eviction_el->content->size;
					if (next_spm_element == NULL) {
						break;//Even if end of list is reached, enough is space is in spm
					}
					cur_eviction_el = next_spm_element;
					next_spm_element = list_next_spm_content(cur_eviction_el, &alloc->spm_content_head);
				}
				//insert element in list
				spm_content *new_spm_content = XMALLOC(spm_content);
				list_add(&new_spm_content->list, &prev_spm_element->list);
				new_spm_content->addr = loop_var->addr;
				new_spm_content->content = loop_var->content;
				new_spm_content->gap_size = cur_eviction_el->addr - content_end;
				prev_spm_element->gap_size = loop_var->addr - (prev_spm_element->addr + prev_spm_element->content->size);
				alloc->free_space -= loop_var->content->size;
				//Setting loop iteration variables to correct values
				var = new_spm_content;
				tmp = list_entry(var->list.next, spm_content, list);
				loop_var = loop_vars[++k];
				if (k == loop_var_cnt)
					handled_all_loop_vars = true;
				//handles also case where one of evicted vars is loop var
				//if so, evict as well, as replacing var is calculated to be more beneficial

			}
			printf("Loopvar adjustment %s:%d\n", gdb_node_helper(loop_block), j);
			print_spm_content_list(&alloc->spm_content_head);
		}
	}
	//4. Before loop header loop_vars have to be transferred to spm and vars_to_evict possibly written back
	//use loop_data transfers
	//TODO: make sure eviction transfers happen before transfer in
	//TODO: check whether vars_to_evict are really in spm at moment of eviction!?
	pset_new_iterator_t iter;
	spm_content *loop_var;
	foreach_pset_new(&vars_to_evict, spm_content *, loop_var, iter) {
		spm_transfer *transfer_out = create_spm_transfer_out(loop_var);
		ARR_APP1(spm_transfer *, l_data->transfers, transfer_out);
		free(loop_var);
	}
	for (size_t k  = 0; k < ARR_LEN(loop_vars); k++) {
		spm_content *loop_var = loop_vars[k];
		spm_transfer *transfer_in = create_spm_transfer_in(loop_var);
		ARR_APP1(spm_transfer *, l_data->transfers, transfer_in);
	}
}

static void ensure_pred_blocks_visited(dprg_walk_env *env)
{
	timestamp *branch = env->cur_branch;
	ir_node *block = branch->block;
	//printf("%s\n", gdb_node_helper(block));
	for (int i = 0; i < get_Block_n_cfgpreds(block); i++) {
		bool loop_detected = false;
		//Loop detection here:
		if (is_backedge(block, i))
			loop_detected = true;

		ir_node *pred_block = get_Block_cfgpred_block(block, i);
		block_data *pred_block_data = pmap_get(block_data, env->block_data_map, pred_block);
		alloc_result *pred_block_res = get_last_block_allocation(pred_block_data);
		if (pred_block_res) {
			if (loop_detected)
				branch->finished_preds = FINISHED_LOOP;
		}
		else {
			if (loop_detected) {
				branch->finished_preds = UNFINISHED_LOOP;
			}
			else {
				branch->finished_preds = PRED_NOT_DONE;
				return;
			}
		}
	}
	if (branch->finished_preds == UNKNOWN_STATUS)
		branch->finished_preds = get_Block_n_cfgpreds(block) > 1 ? COND_JOIN : PREDS_DONE;
}

static void spm_mem_alloc_block(dprg_walk_env *env)
{
	timestamp *branch = env->cur_branch;
	ir_node *block = branch->block;

	//Skip block if not all predecessors have been visited (exception: backedge loop)
	if (branch->finished_preds == UNKNOWN_STATUS)
		ensure_pred_blocks_visited(env);
	if (branch->finished_preds == PRED_NOT_DONE)
		return;
	//Cond branches may push join block twice on queue, with pred blocks done before
	block_data *blk_data = pmap_get(block_data, env->block_data_map, block);
	if (branch->finished_callees == 0 && blk_data->allocation_results[0] != NULL && branch->finished_preds != FINISHED_LOOP) {
		return;
	}
	if (branch->finished_preds == FINISHED_LOOP) {
		spm_join_loop(env);

		alloc_result *cur_alloc = get_last_block_allocation(blk_data);
		int n_outs = get_Block_n_cfg_outs(block);
		for (int i = 0; i < n_outs; i++) {
			//TODO: avoid code duplication
			ir_node *succ_block = get_Block_cfg_out(block, i);
			ir_loop *innerst_loop = branch->cur_loops[ARR_LEN(branch->cur_loops) - 1];
			//only push block outside current loop onto queue
			ir_loop *succ_loop = get_irn_loop(succ_block);
			if (succ_loop) {
				if (succ_loop == innerst_loop)
					continue;
			}

			timestamp *out_branch = XMALLOC(timestamp);
			out_branch->finished_preds = UNKNOWN_STATUS;
			out_branch->block = succ_block;
			out_branch->last_block = block;
			out_branch->last_alloc = cur_alloc;
			out_branch->caller_timestamp = branch->caller_timestamp;
			out_branch->irg_exec_freq = branch->irg_exec_freq;
			out_branch->finished_callees = 0;
			//Delete innerst loop
			loop_data *l_data = pmap_get(loop_data, env->loop_info, innerst_loop);
			DEL_ARR_F(l_data->blocks);
			pset_new_destroy(l_data->mem_accesses);
			free(l_data->mem_accesses);
			//free(l_data);
			out_branch->cur_loops = ARR_SETLEN(ir_loop *, branch->cur_loops, ARR_LEN(branch->cur_loops) - 1);
			branch->cur_loops = NEW_ARR_F(ir_loop *, 0);
			deq_push_pointer_right(&env->workqueue, out_branch);
		}
		return;
	}
	else if (branch->finished_preds == UNFINISHED_LOOP) {
		ir_loop *cur_loop = get_irn_loop(block);
		set_loop_link(cur_loop, get_irn_irg(block));
		assert(cur_loop);
		ARR_APP1(ir_loop *, branch->cur_loops, cur_loop);
		loop_data *l_data = XMALLOC(loop_data);
		l_data->loop_header = block;
		l_data->blocks = NEW_ARR_F(ir_node *, 0);
		l_data->mem_accesses = XMALLOC(pset_new_t);
		pset_new_init(l_data->mem_accesses);
		l_data->transfers = NEW_ARR_F(spm_transfer *, 0);
		pmap_insert(env->loop_info, cur_loop, l_data);
	}
	else if (branch->finished_preds == COND_JOIN) {
		spm_join_cond_blocks(env);
	}

	//At end of irg next block is caller block again
	if (get_irg_end_block(get_irn_irg(block)) == block) {
		timestamp *caller = branch->caller_timestamp;
		if (!caller)
			return;
		ir_node *return_block = spm_join_return_blocks(env);
		block_data *return_block_data = pmap_get(block_data, env->block_data_map, return_block);
		alloc_result *return_block_res = get_last_block_allocation(return_block_data);
		caller->last_block = return_block;
		caller->last_alloc = return_block_res;
		caller->finished_callees++;
		deq_push_pointer_right(&env->workqueue, caller);
		return;
	}
	double block_exec_freq = blk_data->max_exec_freq;
	//calc allocation
	alloc_result *cur_alloc = spm_calc_alloc_block(env);
	blk_data->allocation_results[branch->finished_callees] = cur_alloc;
	printf("Finished block alloc for %s:%d:\n", gdb_node_helper(block), branch->finished_callees);
	print_spm_content_list(&cur_alloc->spm_content_head);
	//inside loop-handling
	for (size_t i = 0; i < ARR_LEN(branch->cur_loops); i++) {
		ir_loop *loop = branch->cur_loops[i];
		assert(loop);
		ir_graph *loop_irg = get_loop_link(loop);
		assert(loop_irg);
		loop_data *l_data = pmap_get(loop_data, env->loop_info, loop);
		assert(l_data);
		ARR_APP1(ir_node *, l_data->blocks, block);
		//TODO: possibly insert whole entry
		foreach_pmap(cur_alloc->copy_in, entry) {
			spm_var_info *var_info = (spm_var_info *) entry->key;
			if (var_info->data_type == NON_STACK_ACCESS  || loop_irg == get_irn_irg(block))
				pset_new_insert(l_data->mem_accesses, var_info);
		}
	}

	//handle next call or successor blocks when end of block is reached
	ir_entity *callee = get_next_call_from_block(env);
	if (callee) {
		ir_graph *irg = get_entity_irg(callee);
		ir_node *start_block = get_irg_start_block(irg);
		//Skip irg, if already visited with higher block_freq
		block_data *start_block_data = pmap_get(block_data, env->block_data_map, start_block);
		if (fabs(start_block_data->max_exec_freq - block_exec_freq) < 0.01f ) { //TODO: float compare okay? (does freq has to be float?)
			timestamp *new_branch = XMALLOC(timestamp);
			timestamp *branch_copy = XMALLOC(timestamp);
			*branch_copy = *branch;
			branch_copy->cur_loops = DUP_ARR_F(ir_loop *, branch->cur_loops);
			new_branch->finished_preds = UNKNOWN_STATUS;
			new_branch->caller_timestamp = branch_copy;
			new_branch->finished_callees = 0;
			new_branch->last_block = block;
			new_branch->last_alloc = cur_alloc;
			new_branch->block = start_block;
			new_branch->irg_exec_freq = block_exec_freq;
			new_branch->cur_loops = DUP_ARR_F(ir_loop *, branch->cur_loops);
			deq_push_pointer_right(&env->workqueue, new_branch);
		}
		else {
			//Add compensation code
			if (blk_data->compensation_callees == NULL) {
				blk_data->compensation_callees = XMALLOC(pset_new_t);
				pset_new_init(blk_data->compensation_callees);
			}
			pset_new_insert(blk_data->compensation_callees, callee);
			//Add current block again, just as we would after handling the callee
			branch->finished_callees++;
			timestamp *branch_copy = XMALLOC(timestamp);
			*branch_copy = *branch;
			branch_copy->cur_loops = DUP_ARR_F(ir_loop *, branch->cur_loops);
			deq_push_pointer_right(&env->workqueue, branch_copy);
		}
		return;
	}

	int n_outs = get_Block_n_cfg_outs(block);
	for (int i = 0; i < n_outs; i++) {
		ir_node *succ_block = get_Block_cfg_out(block, i);
		if (branch->finished_preds == UNFINISHED_LOOP) {
			//only push loop block onto queue
			ir_loop *succ_loop = get_irn_loop(succ_block);
			if (succ_loop) {
				ir_loop *innerst_loop = branch->cur_loops[ARR_LEN(branch->cur_loops) - 1];
				if (succ_loop != innerst_loop)
					continue;
			}
			else {
				continue;
			}
		}
		timestamp *out_branch = XMALLOC(timestamp);
		out_branch->finished_preds = UNKNOWN_STATUS;
		out_branch->block = succ_block;
		out_branch->last_block = block;
		out_branch->last_alloc = cur_alloc;
		out_branch->caller_timestamp = branch->caller_timestamp;
		out_branch->irg_exec_freq = branch->irg_exec_freq;
		out_branch->finished_callees = 0;
		out_branch->cur_loops = DUP_ARR_F(ir_loop *, branch->cur_loops);
		deq_push_pointer_right(&env->workqueue, out_branch);
	}
}

static const arch_register_t *mov_register = &ia32_registers[REG_EAX];
static ir_node       **register_values;

static ir_node *create_spproj(ir_node *const pred, unsigned const pos)
{
	return be_new_Proj_reg(pred, pos, &ia32_registers[REG_ESP]);
}

static ir_node *create_regpush(ir_node *schedpoint, ir_node **mem, ir_node *val, ir_node **sp)
{
	dbg_info *dbgi  = NULL;//get_irn_dbg_info(node);
	ir_node  *block = get_nodes_block(schedpoint);
	ir_graph *irg   = get_irn_irg(schedpoint);
	ir_node  *noreg = ia32_new_NoReg_gp(irg);

	ir_node *push = new_bd_ia32_Push(dbgi, block, noreg, noreg, *mem, val, *sp, X86_SIZE_32);
	//arch_set_irn_register(push, &ia32_registers[REG_ESP]);
	*sp = create_spproj(push, pn_ia32_Push_stack);
	*mem = be_new_Proj(push, pn_ia32_Push_M);

	sched_add_before(schedpoint, push);
	ir_printf("Create %+F\n", push);
	return push;
}

static ir_node *create_regpop(ir_node *schedpoint, ir_node **sp, ir_node *mem)
{
	dbg_info *dbgi  = NULL;//get_irn_dbg_info(node);
	ir_node  *block = get_nodes_block(schedpoint);

	ir_node *pop = new_bd_ia32_Pop(dbgi, block,
	                               mem, *sp, X86_SIZE_32);
	*sp = create_spproj(pop, pn_ia32_Pop_stack);

	sched_add_before(schedpoint, pop);
	ir_node *const val  = be_new_Proj_reg(pop, pn_ia32_Pop_res, mov_register);
	ir_node *const keep = be_new_Keep_one(val);
	sched_add_before(schedpoint, keep);
	ir_printf("Create %+F\n", pop);
	return val;
}

static ir_node *create_store(ir_node *schedpoint, ir_node *mem,
                             ir_node *val, x86_addr_t addr, x86_insn_size_t const size)
{
	dbg_info *dbgi  = NULL;//get_irn_dbg_info(node);
	ir_node  *block = get_nodes_block(schedpoint);
	ir_graph *irg   = get_irn_irg(schedpoint);
	ir_node  *noreg = ia32_new_NoReg_gp(irg);
	ir_node  *frame = get_irg_frame(irg);

	ir_node *store;
	if (addr.immediate.kind == X86_IMM_FRAMEENT)
		store = new_bd_ia32_Store(dbgi, block, frame, noreg, mem,
		                          val, size);

	else
		store = new_bd_ia32_Store(dbgi, block, noreg, noreg, mem,
		                          val, size);
	ia32_attr_t *const attr = get_ia32_attr(store);
	attr->addr = addr;
	set_ia32_op_type(store, ia32_AddrModeD);

	sched_add_before(schedpoint, store);
	return store;
}

static ir_node *create_load(ir_node *schedpoint, ir_node *mem,
                            x86_addr_t addr, x86_insn_size_t const size)
{
	dbg_info *dbgi  = NULL;//get_irn_dbg_info(node);
	ir_node  *block = get_nodes_block(schedpoint);
	ir_graph *irg   = get_irn_irg(schedpoint);
	ir_node  *noreg = ia32_new_NoReg_gp(irg);
	ir_node  *frame = get_irg_frame(irg);

	ir_node *load;
	if (addr.immediate.kind == X86_IMM_FRAMEENT)
		load = new_bd_ia32_Load(dbgi, block, frame, noreg, mem,
		                        size, false);

	else
		load = new_bd_ia32_Load(dbgi, block, noreg, noreg, mem,
		                        size, false);
	ia32_attr_t *const attr = get_ia32_attr(load);
	attr->addr = addr;
	set_ia32_op_type(load, ia32_AddrModeS);

	sched_add_before(schedpoint, load);
	return load;
}

static x86_insn_size_t entsize2insnsize(unsigned const entsize)
{
	return
	    entsize % 2 == 1 ? X86_SIZE_8  :
	    entsize % 4 == 2 ? X86_SIZE_16 :
	    (assert(entsize % 4 == 0), X86_SIZE_32);
}

static void spm_create_transfer_movs(ir_node *before, ir_node **before_mem, spm_transfer *transfer)
{
	ir_entity *entity = transfer->identifier;
	bool frame_use = transfer->type == STACK_ACCESS && transfer->direction != MOV;
	ia32_frame_use_t frame_use_t = frame_use ? IA32_FRAME_USE_AUTO : IA32_FRAME_USE_NONE;
	unsigned entsize = get_type_size(get_entity_type(entity));

	int spm_addr_load = 0;
	int spm_addr_store = 0;
	x86_addr_t addr_load;
	x86_addr_t addr_store;
	if (transfer->direction != IN) {
		//transfer from spm addr
		addr_load = (x86_addr_t) {
			.immediate = (x86_imm32_t) {
				.kind   = X86_IMM_VALUE,
				.entity = NULL,
			},
			.variant = X86_ADDR_JUST_IMM,
		};
		spm_addr_load = spm_properties.start_addr + transfer->spm_addr_from;
		if (transfer->direction == OUT) {
			addr_store = (x86_addr_t) {
				.immediate = (x86_imm32_t) {
					.kind   = frame_use ? X86_IMM_FRAMEENT : X86_IMM_ADDR,
					.entity = entity,
				},
				.variant = X86_ADDR_JUST_IMM,
			};
		}
		else {
			addr_store = addr_load;
			spm_addr_store = spm_properties.start_addr + transfer->spm_addr_to;
		}
	}
	else {
		//transfer from ram entity
		addr_load = (x86_addr_t) {
			.immediate = (x86_imm32_t) {
				.kind   = frame_use ? X86_IMM_FRAMEENT : X86_IMM_ADDR,
				.entity = entity,
			},
			.variant = X86_ADDR_JUST_IMM,
		};
		addr_store = (x86_addr_t) {
			.immediate = (x86_imm32_t) {
				.kind   = X86_IMM_VALUE,
				.entity = NULL,
			},
			.variant = X86_ADDR_JUST_IMM,
		};
		spm_addr_store = spm_properties.start_addr + transfer->spm_addr_to;
	}
	int offset = 0;
	ir_node *load, *store, *load_res;
	do {
		x86_insn_size_t const size = entsize2insnsize(entsize);
		addr_load.immediate.offset = spm_addr_load + offset;
		addr_store.immediate.offset = spm_addr_store + offset;

		load = create_load(before, *before_mem, addr_load, size);
		//arch_set_irn_register(load, mov_register);
		arch_set_irn_register_out(load, pn_ia32_Load_res, mov_register);
		*before_mem = be_new_Proj(load, pn_ia32_Load_M);
		load_res = be_new_Proj_reg(load, pn_ia32_Load_res, mov_register);
		store = create_store(before, *before_mem, load_res, addr_store, size);
		*before_mem = be_new_Proj(store, pn_ia32_Store_M);
		if (transfer->direction == OUT)
			set_ia32_frame_use(store, frame_use_t);
		else if (transfer->direction == IN)
			set_ia32_frame_use(load, frame_use_t);
		ir_printf("Create %+F and %+F for %s %d:%d+%d (%d)\n", load, store, get_entity_name(entity), spm_addr_load, spm_addr_store, offset, entsize);

		unsigned size_bytes = x86_bytes_from_size(size);
		offset  += size_bytes;
		entsize -= size_bytes;
	}
	while (entsize > 0);
}

static void spm_insert_copy_instrs_before_irn(ir_node *irn, spm_transfer **transfers)
{
	assert(ARR_LEN(transfers) > 0);
	ir_node *before = irn;
	printf("before: %s\n", gdb_node_helper(irn));
	ir_graph *irg = get_irn_irg(irn);
	ir_node  *sp = be_get_Start_proj(irg, &ia32_registers[REG_ESP]);
	ir_node *mem = get_irg_no_mem(irg);
	assert(mem);
	ir_node  *val   = register_values[mov_register->global_index];
	printf("eax node: %s\n", gdb_node_helper(val));
	if (val)
		create_regpush(before, &mem, val, &sp);
	for (size_t i = 0; i < ARR_LEN(transfers); i++) {
		spm_transfer *transfer = transfers[i];

		spm_create_transfer_movs(before, &mem, transfer);
		free(transfer);
	}
	if (val) {
		ir_node *pop_proj = create_regpop(before, &sp, mem);

		be_ssa_construction_env_t ssa_constr_env;
		be_ssa_construction_init(&ssa_constr_env, irg);
		be_ssa_construction_add_copy(&ssa_constr_env, val);
		be_ssa_construction_add_copy(&ssa_constr_env, pop_proj);
		be_ssa_construction_fix_users(&ssa_constr_env, val);

		ir_node **new_phis = be_ssa_construction_get_new_phis(&ssa_constr_env);
		for (size_t i = 0; i < ARR_LEN(new_phis); i++) {
			arch_set_irn_register(new_phis[i], mov_register);
		}

		be_ssa_construction_destroy(&ssa_constr_env);
	}
	else {
		ir_node *keep = be_new_Keep_one(mem);
		sched_add_before(before, keep);
	}
}

static void spm_insert_allocation_copy_instrs(ir_node *irn, alloc_result *alloc_res)
{
	spm_transfer **transfers = NEW_ARR_F(spm_transfer *, 0);
	foreach_pmap(alloc_res->copy_in, cur_entry) {
		spm_transfer *transfer = cur_entry->value;
		if (transfer) {
			spm_var_info *var_info = (spm_var_info *) cur_entry->key;
			if (pset_new_contains(alloc_res->write_first_set, var_info)) {
				free(transfer);
			}
			else {
				ARR_APP1(spm_transfer *, transfers, transfer);
			}
		}
	}
	foreach_pmap(alloc_res->swapout_set, cur_entry) {
		spm_transfer *transfer = cur_entry->value;
		if (transfer)
			ARR_APP1(spm_transfer *, transfers, transfer);
	}
	if (ARR_LEN(transfers) > 0)
		spm_insert_copy_instrs_before_irn(irn, transfers);
	DEL_ARR_F(transfers);
}

/* Copy of bepeephole code */
static void clear_reg_value(ir_node *node)
{
	if (!mode_is_data(get_irn_mode(node)))
		return;

	const arch_register_t *reg = arch_get_irn_register(node);
	if (reg == NULL) {
		panic("no register assigned at %+F", node);
	}
	if (reg->is_virtual)
		return;

	unsigned reg_idx = reg->global_index;
	register_values[reg_idx] = NULL;
}

static void set_reg_value(ir_node *node)
{
	if (!mode_is_data(get_irn_mode(node)))
		return;

	const arch_register_t *reg = arch_get_irn_register(node);
	if (reg == NULL) {
		panic("no register assigned at %+F", node);
	}
	if (reg->is_virtual)
		return;

	unsigned reg_idx = reg->global_index;
	register_values[reg_idx] = node;
}

static void clear_defs(ir_node *node)
{
	/* clear values defined */
	be_foreach_value(node, value,
	                 clear_reg_value(value);
	                );
}

static void set_uses(ir_node *node)
{
	/* set values used */
	foreach_irn_in(node, i, in) {
		set_reg_value(in);
	}
}

static void spm_insert_block_copy_instrs(ir_node *blk, block_data *blk_data, pmap *block_data_map)
{
	ir_graph *irg = get_irn_irg(blk);
	list_head *callees = blk_data->node_lists;
	int alloc_idx = blk_data->callee_cnt;
	node_data *next_callee = NULL;
	if (!list_empty(callees))
		next_callee = list_entry(callees->next, node_data, list);
	/* liveness analysis as we need one free register and ensure SSA afterwards */
	be_assure_live_sets(irg);
	be_lv_t *lv = be_get_irg_liveness(irg);
	/* construct initial register assignment */
	register_values = XMALLOCN(ir_node *, ir_target.isa->n_registers);
	memset(register_values, 0, sizeof(ir_node *) * ir_target.isa->n_registers);
	be_lv_foreach(lv, blk, be_lv_state_end, node) {
		set_reg_value(node);
	}

	/* we insert code at the end of block, then moving upwards */
	alloc_result *alloc_res = blk_data->allocation_results[alloc_idx];
	//insert compensation code for joins
	if (alloc_res->compensation_transfers) {
		spm_insert_copy_instrs_before_irn(sched_last(blk), alloc_res->compensation_transfers);//TODO: jump over keeps (see be_spill add reload)?
	}
	if (next_callee) {
		sched_foreach_reverse(blk, irn) {
			/* update registers */
			clear_defs(irn);
			set_uses(irn);


			//TODO: method should not depend on ia32 specific code
			if (!is_ia32_Call(irn))
				continue;

			ir_node *callee = get_irn_n(irn, n_ia32_Call_callee);
			ir_entity *callee_ent = get_ia32_immediate_attr_const(callee)->imm.entity;
			if (callee_ent == next_callee->identifier) {
				//insert compensation code before call node
				if (blk_data->compensation_callees != NULL) {
					if (pset_new_contains(blk_data->compensation_callees, next_callee->identifier)) {
						ir_entity *callee = (ir_entity *) next_callee->identifier;
						ir_node *callee_start = get_irg_start_block(get_entity_irg(callee));
						block_data *callee_data = pmap_get(block_data, block_data_map, callee_start);
						alloc_result *callee_alloc = callee_data->allocation_results[0];
						spm_transfer **transfers = join_allocations(callee_alloc, alloc_res);
						spm_insert_copy_instrs_before_irn(irn, transfers);
						DEL_ARR_F(transfers);
					}
				}

				//insert copies after each call node
				next_callee = list_entry(next_callee->list.next, node_data, list); //TODO: At end of list id is garbage
				alloc_res = blk_data->allocation_results[alloc_idx];
				alloc_idx--;
				spm_insert_allocation_copy_instrs(sched_next(irn), alloc_res); //TODO:sched_next at end is block. does add_before work here?
			}
		}
	}
	//insert copies at start of block
	assert(alloc_idx == 0);
	alloc_res = blk_data->allocation_results[alloc_idx];
	ir_node *before = sched_next(blk);
	before = get_irg_start_block(irg) == blk ? sched_next(before) : before;
	spm_insert_allocation_copy_instrs(before, alloc_res);

	free(register_values);
}

typedef struct node_perm_info {
	ir_node *irn;
	spm_var_info *var_info;
} node_perm_info;

//TODO: move ia32 code to ia32 folder
static void ia32_set_spm_addr(ir_node *irn, int addr)
{
	if (is_ia32_Store(irn)) {
		ir_node *immediate = get_irn_n(irn, 3);
		if (is_ia32_Immediate(immediate)) {
			ia32_immediate_attr_t *attr
			    = get_ia32_immediate_attr(immediate);
			attr->imm.entity = NULL;
			attr->imm.offset += spm_properties.start_addr + addr;
			attr->imm.kind = X86_IMM_VALUE;
		}
	}
	else {
		ia32_attr_t *attr = get_ia32_attr(irn);
		uint64_t old_offset = attr->addr.immediate.offset;
		attr->addr = (x86_addr_t) {
			.immediate = (x86_imm32_t) {
				.kind   = X86_IMM_VALUE,
				.entity = NULL,
				.offset = spm_properties.start_addr + addr + old_offset,
			},
			.variant = X86_ADDR_JUST_IMM,
		};
	}
}

static void spm_adjust_mem_accesses_for_alloc(node_perm_info *nodes, alloc_result *alloc_res)
{
	//TODO: walk over spm alloc list
	//if el is in nodes array, change node and free node_perm_info (set NULL in array)
	size_t nodes_cnt = ARR_LEN(nodes);
	unsigned int addr_changed_cnt = 0;
	list_for_each_entry(spm_content, var, &alloc_res->spm_content_head, list) {
		for (size_t i = 0; i < nodes_cnt; i++) {
			if (var->content == nodes[i].var_info) {
				ia32_set_spm_addr(nodes[i].irn, var->addr);
				addr_changed_cnt++;
			}
		}
		if (addr_changed_cnt == nodes_cnt)
			break;
	}
}


static void spm_adjust_mem_accesses(ir_node *blk, block_data *blk_data)
{
	int callee_cnt = 0;
	node_perm_info *vars_in_spm = NEW_ARR_F(node_perm_info, 0); //TODO: dont use pointer array
	alloc_result *alloc_res;

	sched_foreach(blk, irn) {
		node_data *n_data = retrieve_spm_node_data(irn);

		assert(callee_cnt <= blk_data->callee_cnt);
		alloc_res = blk_data->allocation_results[callee_cnt];

		if (!n_data)
			continue;
		if (n_data->data_type == CALLEE) {
			if (ARR_LEN(vars_in_spm) > 0) {
				spm_adjust_mem_accesses_for_alloc(vars_in_spm, alloc_res);
				DEL_ARR_F(vars_in_spm);
				vars_in_spm = NEW_ARR_F(node_perm_info, 0);
			}
			callee_cnt++;
		}

		ir_entity *entity = (ir_entity *) n_data->identifier;
		spm_var_info *var_info = pmap_get(spm_var_info, spm_var_infos, entity);
		if (pset_new_contains(alloc_res->spm_set, var_info)) {
			node_perm_info perm_info;
			perm_info.irn = irn;
			perm_info.var_info = var_info;
			ARR_APP1(node_perm_info, vars_in_spm, perm_info);
		}
	}
	spm_adjust_mem_accesses_for_alloc(vars_in_spm, alloc_res);
	DEL_ARR_F(vars_in_spm);
}

static void spm_adjust_to_allocations(pmap *block_data_map, pmap *loop_info)
{
	foreach_pmap(block_data_map, cur_entry) {
		ir_node *blk = (ir_node *) cur_entry->key;
		printf("Insert copy instr for %s\n", gdb_node_helper(blk));
		block_data *blk_data = cur_entry->value;
		if (blk_data->allocation_results[0] == NULL)
			continue;
		//mem access adjustment has to happen before copy instr insert
		//as otherwise those would be falsely manipulated as well
		spm_adjust_mem_accesses(blk, blk_data);
		spm_insert_block_copy_instrs(blk, blk_data, block_data_map);
	}

	foreach_pmap(loop_info, cur_entry) {
		loop_data *l_data = cur_entry->value;
		if (ARR_LEN(l_data->transfers) == 0)
			continue;
		ir_printf("Insert copy instr for loop: %+F\n", (ir_loop *) cur_entry->key);
		//find predblock of loop header
		ir_node *header = l_data->loop_header;
		ir_node *pre_header_block = NULL;
		for (int i = 0; i < get_Block_n_cfgpreds(header); i++) {
			if (!is_backedge(header, i))
				pre_header_block = get_Block_cfgpred_block(header, i);
		}
		assert(pre_header_block);
		/* liveness analysis as we need one free register and ensure SSA afterwards */
		ir_graph *irg = get_irn_irg(pre_header_block);
		be_assure_live_sets(irg);
		be_lv_t *lv = be_get_irg_liveness(irg);
		/* liveness analysis as we need one free register and ensure SSA afterwards */
		register_values = XMALLOCN(ir_node *, ir_target.isa->n_registers);
		memset(register_values, 0, sizeof(ir_node *) * ir_target.isa->n_registers);
		be_lv_foreach(lv, pre_header_block, be_lv_state_end, node) {
			set_reg_value(node);
		}
		spm_insert_copy_instrs_before_irn(sched_last(pre_header_block), l_data->transfers);
		free(register_values);
	}
}

static void debug_print_block_data(pmap *block_data_map, bool call_list_only)
{
	printf("--- BLOCK DATA ---\n");
	foreach_pmap(block_data_map, cur_entry) {
		const ir_node *blk = cur_entry->key;
		block_data *blk_data = cur_entry->value;
		printf("%s\n", gdb_node_helper(blk));
		printf("\tCallee count: %d\n", blk_data->callee_cnt);
		printf("\tMax exec freq: %f\n", blk_data->max_exec_freq);
		for (int i = 0; i < blk_data->callee_cnt + 2; i++) {
			if (i == 0) {
				printf("\tCallee List:\n");
				list_for_each_entry(node_data, n_data, &blk_data->node_lists[i], list) {
					printf("\t\t%s\n", get_entity_name((ir_entity *) n_data->identifier));
				}
				if (call_list_only)
					break;
			}
			else {
				printf("\tNode List %d:\n", i - 1);
				list_for_each_entry(node_data, n_data, &blk_data->node_lists[i], list) {
					printf("\t\tEntity:%s, ", get_entity_name((ir_entity *) n_data->identifier));
					printf("Size: %d, Acc_cnt: %d", n_data->size, n_data->access_cnt);
					if (n_data->modified)
						printf(", Modified");
					if (n_data->write_first)
						printf(", WriteFirst");
					printf("\n");
				}
			}
		}
		if (blk_data->allocation_results == NULL)
			continue;
		if (blk_data->allocation_results[0] == NULL)
			continue;
		for (int i = 0; i < blk_data->callee_cnt + 1; i++) {
			printf("Allocation Nr. %d:\n", i);
			alloc_result *alloc = blk_data->allocation_results[i];
			printf("Copy in: ");
			foreach_pmap(alloc->copy_in, copy_entry) {
				spm_transfer *transfer = copy_entry->value;
				if (transfer)
					printf("%s (%d -> %d); ", get_entity_name((ir_entity *) transfer->identifier), transfer->spm_addr_from, transfer->spm_addr_to);
			}
			printf("\nSwapout: ");
			foreach_pmap(alloc->swapout_set, swap_entry) {
				spm_transfer *transfer = swap_entry->value;
				if (transfer)
					printf("%s (%d -> %d); ", get_entity_name((ir_entity *) transfer->identifier), transfer->spm_addr_from, transfer->spm_addr_to);
			}
			printf("\n");
		}
	}
}

static void debug_print_loop_data(pmap *loop_info)
{
	printf("--- LOOP DATA ---\n");
	foreach_pmap(loop_info, cur_entry) {
		printf("%s:", gdb_node_helper((ir_loop *) cur_entry->key));
		loop_data *l_data = cur_entry->value;
		for (size_t i = 0; i < ARR_LEN(l_data->transfers); i++) {
			spm_transfer *transfer = l_data->transfers[i];
			printf("%s (%d -> %d); ", get_entity_name((ir_entity *) transfer->identifier), transfer->spm_addr_from, transfer->spm_addr_to);
		}
		printf("\n");
	}
}

static void free_alloc_result(alloc_result *alloc_res)
{
	pset_new_destroy(alloc_res->spm_set);
	free(alloc_res->spm_set);
	pset_new_destroy(alloc_res->modified_set);
	free(alloc_res->modified_set);
	pset_new_destroy(alloc_res->write_first_set);
	free(alloc_res->write_first_set);
	pmap_destroy(alloc_res->copy_in);
	pmap_destroy(alloc_res->swapout_set);
	if (alloc_res->compensation_transfers) {
		DEL_ARR_F(alloc_res->compensation_transfers);
	}


	list_for_each_entry_safe(spm_content, spm_var, tmp, &alloc_res->spm_content_head, list) {
		free(spm_var);
	}
}

static void free_block_data(block_data *b_data)
{
	for (int i = 0; i < b_data->callee_cnt + 2; i++) {
		list_for_each_entry_safe(node_data, n_data, tmp, &b_data->node_lists[i], list) {
			free(n_data);
		}
	}
	DEL_ARR_F(b_data->node_lists);

	//Check necessary as end block don't have allocation
	if (b_data->allocation_results[0]) {
		for (int i = 0; i < b_data->callee_cnt + 1; i++) {
			free_alloc_result(b_data->allocation_results[i]);
		}
	}
	DEL_ARR_F(b_data->allocation_results);

	if (b_data->compensation_callees) {
		pset_new_destroy(b_data->compensation_callees);
		free(b_data->compensation_callees);
	}
	if (b_data->dead_set) {
		pset_new_destroy(b_data->dead_set);
		free(b_data->dead_set);
	}
	free(b_data);
}

static void free_block_data_map(pmap *block_data_map)
{
	foreach_pmap(block_data_map, entry) {
		free_block_data(entry->value);
	}
	pmap_destroy(block_data_map);
}

static void free_loop_info(pmap *loop_info)
{
	foreach_pmap(loop_info, entry) {
		loop_data *l_info = entry->value;
		DEL_ARR_F(l_info->transfers);
		free(l_info);
	}
	pmap_destroy(loop_info);
}

static void free_spm_var_infos(void)
{
	foreach_pmap(spm_var_infos, entry) {
		free(entry->value);
	}
	pmap_destroy(spm_var_infos);
}

void spm_find_memory_allocation(node_data * (*func)(ir_node *))
{
	retrieve_spm_node_data = func;
	pmap *block_data_map  = pmap_create();
	spm_var_infos = pmap_create();

	foreach_irp_irg(i, irg) {
		ir_estimate_execfreq(irg);
		assure_irg_outs(irg);
		assure_loopinfo(irg);
		spm_collect_irg_data(irg, block_data_map);
	}
	printf("AFTER COLLECTING IRG DATA:\n");
	debug_print_block_data(block_data_map, true);
	spm_calc_blocks_access_freq(block_data_map);
	printf("AFTER CALC BLOCK ACC FREQ:\n");
	debug_print_block_data(block_data_map, false);
	spm_glb_exec_freq_walk(block_data_map);
	printf("AFTER GLB EXEC FREQ CALC:\n");
	debug_print_block_data(block_data_map, false);

	//find main method (only one entry point possible?)
	ir_graph *main_irg = get_irp_main_irg();
	//init metadata list here (actual allocation probably as a map)
	timestamp main_info = {
		.last_block = NULL,
		.last_alloc = NULL,
		.caller_timestamp = NULL,
		.block = get_irg_start_block(main_irg),
		.finished_callees = 0,
		.finished_preds = PREDS_DONE,
		.irg_exec_freq = 1.0f,
		.cur_loops = NEW_ARR_F(ir_loop *, 0),
	};

	dprg_walk_env walk_env;
	deq_init(&walk_env.workqueue);
	walk_env.cur_branch = &main_info;
	walk_env.block_data_map = block_data_map;
	walk_env.loop_info = pmap_create();
	spm_mem_alloc_block(&walk_env);

	timestamp *cur_branch;
	while (!deq_empty(&walk_env.workqueue)) {
		cur_branch = deq_pop_pointer_left(timestamp, &walk_env.workqueue);
		walk_env.cur_branch = cur_branch;
		spm_mem_alloc_block(&walk_env);
		DEL_ARR_F(cur_branch->cur_loops);
		free(cur_branch);
	}
	printf("FINAL DATA:\n");
	debug_print_block_data(block_data_map, false);
	debug_print_loop_data(walk_env.loop_info);
	spm_adjust_to_allocations(block_data_map, walk_env.loop_info);
	/*Have to adjust stack as we created push/pop cascades */
	foreach_irp_irg(i, irg) {
		be_fix_stack_nodes(irg, &ia32_registers[REG_ESP]);
		be_dump(DUMP_BE, irg, "spm");
	}

	deq_free(&walk_env.workqueue);
	free_block_data_map(block_data_map);
	free_spm_var_infos();
	free_loop_info(walk_env.loop_info);
}

static const lc_opt_table_entry_t options[] = {
	LC_OPT_ENT_INT("start_addr", "start address of SPM",  &spm_properties.start_addr),
	LC_OPT_ENT_INT("size", "SPM size",  &spm_properties.size),
	LC_OPT_ENT_INT("latency_diff", "SPM latency difference to RAM",  &spm_properties.latency_diff),
	LC_OPT_ENT_DBL("tp_ram",    "throughput of RAM", &spm_properties.throughput_ram),
	LC_OPT_ENT_DBL("tp_spm",    "throughput of SPM", &spm_properties.throughput_spm),
	LC_OPT_LAST
};

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_spm_alloc)
void be_init_spm_alloc(void)
{
	lc_opt_entry_t *be_grp = lc_opt_get_grp(firm_opt_get_root(), "be");
	lc_opt_entry_t *spm_grp      = lc_opt_get_grp(be_grp, "spm");
	lc_opt_add_table(spm_grp, options);
}
