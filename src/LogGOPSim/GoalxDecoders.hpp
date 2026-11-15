#ifndef GOALX_DECODERS_HPP
#define GOALX_DECODERS_HPP

/*
 * Built-in GOALX record decoders, all behind the C table of goalx_decoder.h:
 *
 *   interp          the optimized interpreter: one precomputed accessor
 *                   (byte offset, shift, mask) per field, no loop, no switch
 *   interp-generic  the naive interpreter: loops over all fields and switches
 *                   on the field kind for every call (reference point)
 *   fixed           the original hand-written decoder, constant offsets into
 *                   the 39-byte legacy record (legacy traces only)
 *   free            decodes every record once at load into flat arrays and
 *                   serves all calls from them: an upper bound on what any
 *                   decoder could gain (O(nodes) memory, for measurements)
 *
 * plus a registry for decoders compiled into the simulator and a loader for
 * plugin shared objects.
 */

#include <string>
#include <vector>
#include <utility>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "TraceSchema.hpp"
#include "goalx_decoder.h"

/* a Schema as a goalx_schema_t; the Schema must outlive the holder */
struct GoalxSchemaHolder {
	goalx_schema_t schema;
	std::vector<goalx_field_t> fields;
	std::vector<uint16_t> custom_ix;

	GoalxSchemaHolder() { memset(&schema, 0, sizeof(schema)); }

	/* custom_names: the custom fields the simulator wants, slot = position; all must exist */
	bool set(const Schema& s, const std::vector<std::string>& custom_names, std::string* err = NULL) {
		fields.clear();
		custom_ix.clear();
		for (size_t i = 0; i < s.fields.size(); i++) {
			const FieldDesc& f = s.fields[i];
			goalx_field_t g;
			g.kind = f.kind;
			g.bit_offset = f.bit_offset;
			g.width = f.width;
			g.flags = f.flags;
			g.op_mask = f.op_mask;
			g.dflt = f.dflt;
			g.max_value = f.max_value;
			g.name = f.name.c_str();
			fields.push_back(g);
		}
		for (size_t c = 0; c < custom_names.size(); c++) {
			int ix = -1;
			for (size_t i = 0; i < s.fields.size(); i++) {
				if (s.fields[i].kind == FK_CUSTOM && s.fields[i].name == custom_names[c]) ix = (int) i;
			}
			if (ix < 0) {
				if (err) *err = "trace has no custom field '" + custom_names[c] + "'";
				return false;
			}
			custom_ix.push_back((uint16_t) ix);
		}
		schema.schema_hash = s.legacy ? 0 : s.hash();
		schema.record_bytes = s.record_bytes;
		schema.num_fields = (uint16_t) fields.size();
		schema.edge_bytes = s.edge_bytes;
		schema.legacy = s.legacy ? 1 : 0;
		schema.fields = fields.empty() ? NULL : &fields[0];
		schema.num_custom = (uint16_t) custom_ix.size();
		schema.custom_field_ix = custom_ix.empty() ? NULL : &custom_ix[0];
		return true;
	}
};

static inline const goalx_field_t* goalx_find_kind(const goalx_schema_t* s, uint16_t kind) {
	for (uint16_t i = 0; i < s->num_fields; i++) if (s->fields[i].kind == kind) return &s->fields[i];
	return NULL;
}

static inline Accessor goalx_accessor(const goalx_field_t* g, uint32_t record_bytes) {
	if (g == NULL) return Accessor();
	FieldDesc f;
	f.bit_offset = g->bit_offset;
	f.width = g->width;
	f.flags = g->flags;
	f.dflt = g->dflt;
	return Accessor(&f, record_bytes);
}

static inline goalx_decoder_t* goalx_new_table(const char* name) {
	goalx_decoder_t* d = new goalx_decoder_t;
	memset(d, 0, sizeof(*d));
	d->abi = GOALX_DECODER_ABI;
	d->name = name;
	return d;
}

static inline const char* goalx_rec(const uint8_t* rec) { return (const char*) rec; }

/* ---------------------------------------------------------------- interp */

struct GoalxInterpState {
	Accessor type, peer, size, tag, cpu, nic, ndep, depix, nsdep, sdepix, depcnt;
	std::vector<Accessor> custom;
};

#define GOALX_IS(d) ((const GoalxInterpState*) (d)->state)

static void* goalx_interp_bind(goalx_decoder_t*, const goalx_rank_view_t*) { return NULL; }

static void goalx_interp_decode_op(const goalx_decoder_t* d, void*, const uint8_t* rec, uint32_t, goalx_op_t* o) {
	const GoalxInterpState* S = GOALX_IS(d);
	const char* r = goalx_rec(rec);
	o->type = (uint8_t) S->type.get_simple<false>(r);
	o->peer = (uint32_t) S->peer.get_simple<true>(r);
	o->size = S->size.get_simple<false>(r);
	o->tag  = (uint32_t) S->tag.get_simple<true>(r);
	o->cpu  = (uint8_t) S->cpu.get(r);
	o->nic  = (uint8_t) S->nic.get(r);
}

static uint32_t goalx_interp_depcnt(const goalx_decoder_t* d, void*, const uint8_t* rec, uint32_t) {
	return (uint32_t) GOALX_IS(d)->depcnt.get_simple<false>(goalx_rec(rec));
}

static void goalx_interp_done(const goalx_decoder_t* d, void*, const uint8_t* rec, uint32_t, uint32_t* n, uint32_t* ix) {
	const GoalxInterpState* S = GOALX_IS(d);
	*n  = (uint32_t) S->ndep.get_simple<false>(goalx_rec(rec));
	*ix = (uint32_t) S->depix.get_simple<false>(goalx_rec(rec));
}

static void goalx_interp_start(const goalx_decoder_t* d, void*, const uint8_t* rec, uint32_t, uint32_t* n, uint32_t* ix) {
	const GoalxInterpState* S = GOALX_IS(d);
	const char* r = goalx_rec(rec);
	*n  = (uint32_t) S->nsdep.get_simple<false>(r);
	*ix = S->sdepix.present ? (uint32_t) S->sdepix.get(r)
	                        : (uint32_t) (S->depix.get_simple<false>(r) + S->ndep.get_simple<false>(r));
}

static uint64_t goalx_interp_custom(const goalx_decoder_t* d, void*, const uint8_t* rec, uint32_t, uint16_t slot) {
	return GOALX_IS(d)->custom[slot].get(goalx_rec(rec));
}

static void goalx_interp_destroy(goalx_decoder_t* d) {
	delete (GoalxInterpState*) d->state;
	delete d;
}

static goalx_decoder_t* goalx_make_interp(const goalx_schema_t* s, const goalx_trace_view_t*) {
	GoalxInterpState* S = new GoalxInterpState;
	S->type   = goalx_accessor(goalx_find_kind(s, GOALX_FK_TYPE), s->record_bytes);
	S->peer   = goalx_accessor(goalx_find_kind(s, GOALX_FK_PEER), s->record_bytes);
	S->size   = goalx_accessor(goalx_find_kind(s, GOALX_FK_SIZE), s->record_bytes);
	S->tag    = goalx_accessor(goalx_find_kind(s, GOALX_FK_TAG), s->record_bytes);
	S->cpu    = goalx_accessor(goalx_find_kind(s, GOALX_FK_CPU), s->record_bytes);
	S->nic    = goalx_accessor(goalx_find_kind(s, GOALX_FK_NIC), s->record_bytes);
	S->ndep   = goalx_accessor(goalx_find_kind(s, GOALX_FK_NDEP), s->record_bytes);
	S->depix  = goalx_accessor(goalx_find_kind(s, GOALX_FK_DEPIX), s->record_bytes);
	S->nsdep  = goalx_accessor(goalx_find_kind(s, GOALX_FK_NSDEP), s->record_bytes);
	S->sdepix = goalx_accessor(goalx_find_kind(s, GOALX_FK_SDEPIX), s->record_bytes);
	S->depcnt = goalx_accessor(goalx_find_kind(s, GOALX_FK_DEPCNT), s->record_bytes);
	for (uint16_t c = 0; c < s->num_custom; c++) S->custom.push_back(goalx_accessor(&s->fields[s->custom_field_ix[c]], s->record_bytes));
	goalx_decoder_t* d = goalx_new_table("interp");
	d->state = S;
	d->bind_rank = goalx_interp_bind;
	d->decode_op = goalx_interp_decode_op;
	d->depcnt = goalx_interp_depcnt;
	d->done_edges = goalx_interp_done;
	d->start_edges = goalx_interp_start;
	d->get_custom = goalx_interp_custom;
	d->destroy = goalx_interp_destroy;
	return d;
}

/* -------------------------------------------------------- interp-generic */

struct GoalxGenericState {
	std::vector<goalx_field_t> fields;   // bit_offset rewritten relative to load_byte
	std::vector<uint32_t> load_byte;     // where the 64-bit load for each field starts
	std::vector<uint16_t> custom_ix;
	uint64_t dflt[FK_NUM_BUILTIN];
	bool has_sdepix;
};

/* the textbook interpreter: every call walks all fields and switches on the kind */
static void goalx_generic_all(const GoalxGenericState* g, const uint8_t* rec, uint64_t v[FK_NUM_BUILTIN]) {
	memcpy(v, g->dflt, sizeof(g->dflt));
	for (size_t i = 0; i < g->fields.size(); i++) {
		const goalx_field_t& f = g->fields[i];
		uint64_t w;
		memcpy(&w, rec + g->load_byte[i], sizeof(w));
		uint64_t mask = width_mask(f.width);
		uint64_t x = (w >> f.bit_offset) & mask;
		if ((f.flags & GOALX_FF_ANY_SENTINEL) && x == mask) x = 0xFFFFFFFFULL;
		switch (f.kind) {
			case GOALX_FK_TYPE:   v[FK_TYPE] = x; break;
			case GOALX_FK_PEER:   v[FK_PEER] = x; break;
			case GOALX_FK_SIZE:   v[FK_SIZE] = x; break;
			case GOALX_FK_TAG:    v[FK_TAG] = x; break;
			case GOALX_FK_CPU:    v[FK_CPU] = x; break;
			case GOALX_FK_NIC:    v[FK_NIC] = x; break;
			case GOALX_FK_NDEP:   v[FK_NDEP] = x; break;
			case GOALX_FK_DEPIX:  v[FK_DEPIX] = x; break;
			case GOALX_FK_NSDEP:  v[FK_NSDEP] = x; break;
			case GOALX_FK_SDEPIX: v[FK_SDEPIX] = x; break;
			case GOALX_FK_DEPCNT: v[FK_DEPCNT] = x; break;
			default: break; // custom fields: see goalx_generic_custom
		}
	}
	if (!g->has_sdepix) v[FK_SDEPIX] = v[FK_DEPIX] + v[FK_NDEP];
}

#define GOALX_GS(d) ((const GoalxGenericState*) (d)->state)

static void goalx_generic_decode_op(const goalx_decoder_t* d, void*, const uint8_t* rec, uint32_t, goalx_op_t* o) {
	uint64_t v[FK_NUM_BUILTIN];
	goalx_generic_all(GOALX_GS(d), rec, v);
	o->type = (uint8_t) v[FK_TYPE];
	o->peer = (uint32_t) v[FK_PEER];
	o->size = v[FK_SIZE];
	o->tag  = (uint32_t) v[FK_TAG];
	o->cpu  = (uint8_t) v[FK_CPU];
	o->nic  = (uint8_t) v[FK_NIC];
}

static uint32_t goalx_generic_depcnt(const goalx_decoder_t* d, void*, const uint8_t* rec, uint32_t) {
	uint64_t v[FK_NUM_BUILTIN];
	goalx_generic_all(GOALX_GS(d), rec, v);
	return (uint32_t) v[FK_DEPCNT];
}

static void goalx_generic_done(const goalx_decoder_t* d, void*, const uint8_t* rec, uint32_t, uint32_t* n, uint32_t* ix) {
	uint64_t v[FK_NUM_BUILTIN];
	goalx_generic_all(GOALX_GS(d), rec, v);
	*n = (uint32_t) v[FK_NDEP];
	*ix = (uint32_t) v[FK_DEPIX];
}

static void goalx_generic_start(const goalx_decoder_t* d, void*, const uint8_t* rec, uint32_t, uint32_t* n, uint32_t* ix) {
	uint64_t v[FK_NUM_BUILTIN];
	goalx_generic_all(GOALX_GS(d), rec, v);
	*n = (uint32_t) v[FK_NSDEP];
	*ix = (uint32_t) v[FK_SDEPIX];
}

static uint64_t goalx_generic_custom(const goalx_decoder_t* d, void*, const uint8_t* rec, uint32_t, uint16_t slot) {
	const GoalxGenericState* g = GOALX_GS(d);
	uint64_t result = 0;
	for (size_t i = 0; i < g->fields.size(); i++) {
		if (i != g->custom_ix[slot]) continue;
		const goalx_field_t& f = g->fields[i];
		uint64_t w;
		memcpy(&w, rec + g->load_byte[i], sizeof(w));
		result = (w >> f.bit_offset) & width_mask(f.width);
	}
	return result;
}

static void goalx_generic_destroy(goalx_decoder_t* d) {
	delete (GoalxGenericState*) d->state;
	delete d;
}

static goalx_decoder_t* goalx_make_generic(const goalx_schema_t* s, const goalx_trace_view_t*) {
	GoalxGenericState* g = new GoalxGenericState;
	g->fields.assign(s->fields, s->fields + s->num_fields);
	for (size_t i = 0; i < g->fields.size(); i++) {
		Accessor a = goalx_accessor(&g->fields[i], s->record_bytes);
		g->load_byte.push_back(a.byte_off);
		g->fields[i].bit_offset = a.shift; // now the shift within the loaded word
	}
	g->custom_ix.assign(s->custom_field_ix, s->custom_field_ix + s->num_custom);
	memset(g->dflt, 0, sizeof(g->dflt));
	for (uint16_t i = 0; i < s->num_fields; i++) {
		if (s->fields[i].kind < FK_NUM_BUILTIN) g->dflt[s->fields[i].kind] = s->fields[i].dflt;
	}
	g->has_sdepix = goalx_find_kind(s, GOALX_FK_SDEPIX) != NULL;
	goalx_decoder_t* d = goalx_new_table("interp-generic");
	d->state = g;
	d->bind_rank = goalx_interp_bind;
	d->decode_op = goalx_generic_decode_op;
	d->depcnt = goalx_generic_depcnt;
	d->done_edges = goalx_generic_done;
	d->start_edges = goalx_generic_start;
	d->get_custom = goalx_generic_custom;
	d->destroy = goalx_generic_destroy;
	return d;
}

/* ----------------------------------------------------------------- fixed */

template <typename T> static inline T goalx_at(const uint8_t* rec, size_t off) {
	T v;
	memcpy(&v, rec + off, sizeof(T));
	return v;
}

static void goalx_fixed_decode_op(const goalx_decoder_t*, void*, const uint8_t* rec, uint32_t, goalx_op_t* o) {
	o->type = goalx_at<uint8_t>(rec, 4);
	o->peer = goalx_at<uint32_t>(rec, 5);
	o->size = goalx_at<uint64_t>(rec, 9);
	o->tag  = goalx_at<uint32_t>(rec, 17);
	o->cpu  = goalx_at<uint8_t>(rec, 21);
	o->nic  = goalx_at<uint8_t>(rec, 22);
}

static uint32_t goalx_fixed_depcnt(const goalx_decoder_t*, void*, const uint8_t* rec, uint32_t) {
	return goalx_at<uint32_t>(rec, 0);
}

static void goalx_fixed_done(const goalx_decoder_t*, void*, const uint8_t* rec, uint32_t, uint32_t* n, uint32_t* ix) {
	*n  = goalx_at<uint32_t>(rec, 23);
	*ix = goalx_at<uint32_t>(rec, 27);
}

static void goalx_fixed_start(const goalx_decoder_t*, void*, const uint8_t* rec, uint32_t, uint32_t* n, uint32_t* ix) {
	*n  = goalx_at<uint32_t>(rec, 31);
	*ix = goalx_at<uint32_t>(rec, 35);
}

static uint64_t goalx_fixed_custom(const goalx_decoder_t*, void*, const uint8_t*, uint32_t, uint16_t) { return 0; }

static void goalx_fixed_destroy(goalx_decoder_t* d) { delete d; }

static goalx_decoder_t* goalx_make_fixed(const goalx_schema_t* s, const goalx_trace_view_t*) {
	if (!s->legacy) return NULL;
	goalx_decoder_t* d = goalx_new_table("fixed");
	d->bind_rank = goalx_interp_bind;
	d->decode_op = goalx_fixed_decode_op;
	d->depcnt = goalx_fixed_depcnt;
	d->done_edges = goalx_fixed_done;
	d->start_edges = goalx_fixed_start;
	d->get_custom = goalx_fixed_custom;
	d->destroy = goalx_fixed_destroy;
	return d;
}

/* ------------------------------------------------------------------ free */

struct GoalxFreeRank {
	std::vector<goalx_op_t> ops;
	std::vector<uint32_t> depcnt, ndep, depix, nsdep, sdepix;
	std::vector<uint64_t> custom; // num_nodes * num_custom
};

struct GoalxFreeState {
	goalx_decoder_t* interp;
	uint16_t record_bytes;
	uint16_t num_custom;
	std::vector<GoalxFreeRank*> ranks;
};

#define GOALX_FR(rs) ((const GoalxFreeRank*) (rs))

/* decodes the whole rank once with interp; every later call is an array lookup */
static void* goalx_free_bind(goalx_decoder_t* d, const goalx_rank_view_t* v) {
	GoalxFreeState* S = (GoalxFreeState*) d->state;
	const goalx_decoder_t* in = S->interp;
	GoalxFreeRank* R = new GoalxFreeRank;
	uint32_t n = v->num_nodes;
	R->ops.resize(n);
	R->depcnt.resize(n); R->ndep.resize(n); R->depix.resize(n); R->nsdep.resize(n); R->sdepix.resize(n);
	R->custom.resize((size_t) n * S->num_custom);
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t* rec = v->records + (size_t) i * S->record_bytes;
		in->decode_op(in, NULL, rec, i, &R->ops[i]);
		R->depcnt[i] = in->depcnt(in, NULL, rec, i);
		in->done_edges(in, NULL, rec, i, &R->ndep[i], &R->depix[i]);
		uint32_t ns, sx;
		in->start_edges(in, NULL, rec, i, &ns, &sx);
		R->nsdep[i] = ns;
		R->sdepix[i] = sx;
		for (uint16_t c = 0; c < S->num_custom; c++) R->custom[(size_t) i * S->num_custom + c] = in->get_custom(in, NULL, rec, i, c);
	}
	S->ranks.push_back(R);
	return R;
}

static void goalx_free_decode_op(const goalx_decoder_t*, void* rs, const uint8_t*, uint32_t idx, goalx_op_t* o) {
	*o = GOALX_FR(rs)->ops[idx];
}

static uint32_t goalx_free_depcnt(const goalx_decoder_t*, void* rs, const uint8_t*, uint32_t idx) {
	return GOALX_FR(rs)->depcnt[idx];
}

static void goalx_free_done(const goalx_decoder_t*, void* rs, const uint8_t*, uint32_t idx, uint32_t* n, uint32_t* ix) {
	*n = GOALX_FR(rs)->ndep[idx];
	*ix = GOALX_FR(rs)->depix[idx];
}

static void goalx_free_start(const goalx_decoder_t*, void* rs, const uint8_t*, uint32_t idx, uint32_t* n, uint32_t* ix) {
	*n = GOALX_FR(rs)->nsdep[idx];
	*ix = GOALX_FR(rs)->sdepix[idx];
}

static uint64_t goalx_free_custom(const goalx_decoder_t* d, void* rs, const uint8_t*, uint32_t idx, uint16_t slot) {
	const GoalxFreeState* S = (const GoalxFreeState*) d->state;
	return GOALX_FR(rs)->custom[(size_t) idx * S->num_custom + slot];
}

static void goalx_free_destroy(goalx_decoder_t* d) {
	GoalxFreeState* S = (GoalxFreeState*) d->state;
	for (size_t i = 0; i < S->ranks.size(); i++) delete S->ranks[i];
	S->interp->destroy(S->interp);
	delete S;
	delete d;
}

static goalx_decoder_t* goalx_make_free(const goalx_schema_t* s, const goalx_trace_view_t* t) {
	GoalxFreeState* S = new GoalxFreeState;
	S->interp = goalx_make_interp(s, t);
	S->record_bytes = s->record_bytes;
	S->num_custom = s->num_custom;
	goalx_decoder_t* d = goalx_new_table("free");
	d->state = S;
	d->bind_rank = goalx_free_bind;
	d->decode_op = goalx_free_decode_op;
	d->depcnt = goalx_free_depcnt;
	d->done_edges = goalx_free_done;
	d->start_edges = goalx_free_start;
	d->get_custom = goalx_free_custom;
	d->destroy = goalx_free_destroy;
	return d;
}

/* -------------------------------------------------- registry and loading */

typedef std::vector<std::pair<std::string, goalx_decoder_factory_t> > GoalxRegistry;

static inline GoalxRegistry& goalx_registry() {
	static GoalxRegistry r;
	return r;
}

/* makes a decoder compiled into the simulator selectable as --decoder <name> */
static inline void goalx_register_decoder(const char* name, goalx_decoder_factory_t f) {
	goalx_registry().push_back(std::make_pair(std::string(name), f));
}

/*
 * Creates the decoder called name for a trace: a registered one, a built-in
 * (interp, interp-generic, fixed, free) or plugin:<path.so>. Returns NULL and
 * sets err if that is impossible. A plugin that declines the schema is
 * replaced by interp, with a notice on stderr.
 */
static inline goalx_decoder_t* goalx_create_decoder(const char* name, const goalx_schema_t* s,
                                                    const goalx_trace_view_t* t, std::string* err) {
	std::string n = name ? name : "";
	GoalxRegistry& reg = goalx_registry();
	for (size_t i = 0; i < reg.size(); i++) {
		if (reg[i].first != n) continue;
		goalx_decoder_t* d = reg[i].second(s, t);
		if (d == NULL) {
			fprintf(stderr, "decoder '%s' declined this schema, using interp\n", name);
			return goalx_make_interp(s, t);
		}
		return d;
	}
	if (n == "interp") return goalx_make_interp(s, t);
	if (n == "interp-generic") return goalx_make_generic(s, t);
	if (n == "free") return goalx_make_free(s, t);
	if (n == "fixed") {
		if (!s->legacy) {
			*err = "the fixed decoder only reads legacy (4223) traces";
			return NULL;
		}
		return goalx_make_fixed(s, t);
	}
	if (n.compare(0, 7, "plugin:") == 0) {
		std::string path = n.substr(7);
		void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (h == NULL) {
			const char* e = dlerror();
			*err = "cannot load decoder plugin '" + path + "': " + (e ? e : "unknown error");
			return NULL;
		}
		goalx_decoder_factory_t f = (goalx_decoder_factory_t) dlsym(h, "goalx_make_decoder");
		if (f == NULL) {
			*err = "decoder plugin '" + path + "' does not export goalx_make_decoder";
			return NULL;
		}
		goalx_decoder_t* d = f(s, t);
		if (d == NULL) {
			fprintf(stderr, "decoder '%s' declined this schema, using interp\n", path.c_str());
			return goalx_make_interp(s, t);
		}
		if (d->abi != GOALX_DECODER_ABI) {
			*err = "decoder plugin '" + path + "' implements ABI " + std::to_string(d->abi) + ", this simulator needs " +
			       std::to_string(GOALX_DECODER_ABI);
			return NULL;
		}
		return d;
	}
	*err = "unknown decoder '" + n + "' (known: auto, interp, interp-generic, fixed, free, plugin:<path>";
	for (size_t i = 0; i < reg.size(); i++) *err += ", " + reg[i].first;
	*err += ")";
	return NULL;
}

/* --------------------------------------------- recording (benchmarks) */

/* one recorded decoder call: kind in the top 2 bits of idx_and_kind */
struct GoalxCall {
	uint32_t rank;
	uint32_t idx_and_kind;
};

enum { GOALX_CALL_DECODE_OP = 0, GOALX_CALL_DEPCNT = 1, GOALX_CALL_DONE_EDGES = 2, GOALX_CALL_START_EDGES = 3 };

static inline GoalxCall goalx_call(uint32_t rank, uint32_t idx, uint32_t kind) {
	GoalxCall c;
	c.rank = rank;
	c.idx_and_kind = idx | (kind << 30);
	return c;
}

struct GoalxRecordRank {
	uint32_t rank;
	void* inner_rs;
};

struct GoalxRecordState {
	goalx_decoder_t* inner;
	std::string path;
	std::string name;
	std::vector<GoalxCall> calls;
	std::vector<GoalxRecordRank*> ranks;
};

#define GOALX_RS(d) ((GoalxRecordState*) (d)->state)
#define GOALX_RR(rs) ((const GoalxRecordRank*) (rs))

static void* goalx_record_bind(goalx_decoder_t* d, const goalx_rank_view_t* v) {
	GoalxRecordState* S = GOALX_RS(d);
	GoalxRecordRank* R = new GoalxRecordRank;
	R->rank = v->rank;
	R->inner_rs = S->inner->bind_rank ? S->inner->bind_rank(S->inner, v) : NULL;
	S->ranks.push_back(R);
	return R;
}

static void goalx_record_decode_op(const goalx_decoder_t* d, void* rs, const uint8_t* rec, uint32_t idx, goalx_op_t* o) {
	GoalxRecordState* S = GOALX_RS(d);
	S->calls.push_back(goalx_call(GOALX_RR(rs)->rank, idx, GOALX_CALL_DECODE_OP));
	S->inner->decode_op(S->inner, GOALX_RR(rs)->inner_rs, rec, idx, o);
}

static uint32_t goalx_record_depcnt(const goalx_decoder_t* d, void* rs, const uint8_t* rec, uint32_t idx) {
	GoalxRecordState* S = GOALX_RS(d);
	S->calls.push_back(goalx_call(GOALX_RR(rs)->rank, idx, GOALX_CALL_DEPCNT));
	return S->inner->depcnt(S->inner, GOALX_RR(rs)->inner_rs, rec, idx);
}

static void goalx_record_done(const goalx_decoder_t* d, void* rs, const uint8_t* rec, uint32_t idx, uint32_t* n, uint32_t* ix) {
	GoalxRecordState* S = GOALX_RS(d);
	S->calls.push_back(goalx_call(GOALX_RR(rs)->rank, idx, GOALX_CALL_DONE_EDGES));
	S->inner->done_edges(S->inner, GOALX_RR(rs)->inner_rs, rec, idx, n, ix);
}

static void goalx_record_start(const goalx_decoder_t* d, void* rs, const uint8_t* rec, uint32_t idx, uint32_t* n, uint32_t* ix) {
	GoalxRecordState* S = GOALX_RS(d);
	S->calls.push_back(goalx_call(GOALX_RR(rs)->rank, idx, GOALX_CALL_START_EDGES));
	S->inner->start_edges(S->inner, GOALX_RR(rs)->inner_rs, rec, idx, n, ix);
}

static uint64_t goalx_record_custom(const goalx_decoder_t* d, void* rs, const uint8_t* rec, uint32_t idx, uint16_t slot) {
	GoalxRecordState* S = GOALX_RS(d);
	return S->inner->get_custom(S->inner, GOALX_RR(rs)->inner_rs, rec, idx, slot);
}

/* writes the recorded calls, then frees everything */
static void goalx_record_destroy(goalx_decoder_t* d) {
	GoalxRecordState* S = GOALX_RS(d);
	FILE* f = fopen(S->path.c_str(), "wb");
	if (f == NULL || (!S->calls.empty() && fwrite(&S->calls[0], sizeof(GoalxCall), S->calls.size(), f) != S->calls.size())) {
		perror(S->path.c_str());
	}
	if (f) fclose(f);
	for (size_t i = 0; i < S->ranks.size(); i++) delete S->ranks[i];
	S->inner->destroy(S->inner);
	delete S;
	delete d;
}

/* wraps a decoder so that every hot call is recorded; the log is written to path on destroy */
static inline goalx_decoder_t* goalx_wrap_recording(goalx_decoder_t* inner, const char* path) {
	GoalxRecordState* S = new GoalxRecordState;
	S->inner = inner;
	S->path = path;
	S->name = std::string("record:") + inner->name;
	S->calls.reserve(1 << 20);
	goalx_decoder_t* d = goalx_new_table("record");
	d->name = S->name.c_str();
	d->state = S;
	d->bind_rank = goalx_record_bind;
	d->decode_op = goalx_record_decode_op;
	d->depcnt = goalx_record_depcnt;
	d->done_edges = goalx_record_done;
	d->start_edges = goalx_record_start;
	d->get_custom = goalx_record_custom;
	d->destroy = goalx_record_destroy;
	return d;
}

#endif
