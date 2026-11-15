/*
 * Example GOALX decoder plugin: decodes the 39-byte legacy (4223) record with
 * constant offsets, exactly like the built-in "fixed" decoder, and declines
 * every other schema. It shows what a JIT- or template-generated decoder has
 * to provide. Build: cc -std=c99 -O3 -shared -fPIC -I<LogGOPSim src> legacy_plugin.c -o liblegacy_plugin.so
 * Use:   LogGOPSim -f trace.bin --decoder plugin:./liblegacy_plugin.so
 */
#include <stdlib.h>
#include <string.h>
#include "goalx_decoder.h"

/* lets the simulator refuse this plugin before calling the factory if the ABIs differ */
const uint32_t goalx_decoder_abi = GOALX_DECODER_ABI;

static uint32_t u32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void* bind_rank(goalx_decoder_t* d, const goalx_rank_view_t* v) { (void) d; (void) v; return NULL; }

static void decode_op(const goalx_decoder_t* d, void* rs, const uint8_t* rec, uint32_t idx, goalx_op_t* o) {
	(void) d; (void) rs; (void) idx;
	o->type = rec[4];
	o->peer = u32(rec + 5);
	memcpy(&o->size, rec + 9, 8);
	o->tag = u32(rec + 17);
	o->cpu = rec[21];
	o->nic = rec[22];
}

static uint32_t depcnt(const goalx_decoder_t* d, void* rs, const uint8_t* rec, uint32_t idx) {
	(void) d; (void) rs; (void) idx;
	return u32(rec);
}

static void done_edges(const goalx_decoder_t* d, void* rs, const uint8_t* rec, uint32_t idx, uint32_t* n, uint32_t* ix) {
	(void) d; (void) rs; (void) idx;
	*n = u32(rec + 23);
	*ix = u32(rec + 27);
}

static void start_edges(const goalx_decoder_t* d, void* rs, const uint8_t* rec, uint32_t idx, uint32_t* n, uint32_t* ix) {
	(void) d; (void) rs; (void) idx;
	*n = u32(rec + 31);
	*ix = u32(rec + 35);
}

static uint64_t get_custom(const goalx_decoder_t* d, void* rs, const uint8_t* rec, uint32_t idx, uint16_t slot) {
	(void) d; (void) rs; (void) rec; (void) idx; (void) slot;
	return 0;
}

static void destroy(goalx_decoder_t* d) { free(d); }

goalx_decoder_t* goalx_make_decoder(const goalx_schema_t* s, const goalx_trace_view_t* t) {
	goalx_decoder_t* d;
	(void) t;
	if (!s->legacy) return NULL;
	d = (goalx_decoder_t*) calloc(1, sizeof(*d));
	d->abi = GOALX_DECODER_ABI;
	d->name = "plugin:legacy";
	d->bind_rank = bind_rank;
	d->decode_op = decode_op;
	d->depcnt = depcnt;
	d->done_edges = done_edges;
	d->start_edges = start_edges;
	d->get_custom = get_custom;
	d->destroy = destroy;
	return d;
}
