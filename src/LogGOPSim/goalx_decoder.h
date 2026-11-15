/*
 * GOALX decoder contract (C99 and C++11).
 *
 * Every record decoder LogGOPSim uses implements this table: the built-in
 * interpreters, the legacy fixed-offset decoder, decoders compiled into the
 * simulator (e.g. template-instantiated ones, registered with
 * goalx_register_decoder) and decoders loaded at run time from a shared object
 * (--decoder plugin:<path.so>, e.g. JIT-generated C compiled with cc -shared).
 * The simulator calls every decoder through these function pointers, so all
 * of them are measured with the same dispatch cost.
 *
 * Lifetimes: the factory is called once per trace; bind_rank once per rank at
 * load. The trace mapping and the goalx_schema_t outlive the decoder. destroy
 * frees state and every rank_state the decoder returned.
 */
#ifndef GOALX_DECODER_H
#define GOALX_DECODER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ABI 1, frozen 2026-11-14 after measuring the function-table dispatch end to
 * end: on legacy files the reader with every decoder behind this table is as
 * fast as or faster than inlined decoders (data in the benchmarks repository).
 * Changing any struct or signature below needs GOALX_DECODER_ABI 2.
 */
#define GOALX_DECODER_ABI 1

/* field kinds (same numbers as FieldKind in TraceSchema.hpp) */
#define GOALX_FK_TYPE    0
#define GOALX_FK_PEER    1
#define GOALX_FK_SIZE    2
#define GOALX_FK_TAG     3
#define GOALX_FK_CPU     4
#define GOALX_FK_NIC     5
#define GOALX_FK_NDEP    6
#define GOALX_FK_DEPIX   7
#define GOALX_FK_NSDEP   8
#define GOALX_FK_SDEPIX  9
#define GOALX_FK_DEPCNT  10
#define GOALX_FK_CUSTOM  0xFFFF

/* goalx_field_t.flags: all ones of the field's width decodes as ANY (0xFFFFFFFF) */
#define GOALX_FF_ANY_SENTINEL 0x1

typedef struct {                 /* the schema, as plain data, for decoder generators */
    uint16_t kind;               /* GOALX_FK_* */
    uint16_t bit_offset;         /* (bit_offset % 8) + width <= 64 always holds */
    uint8_t  width, flags, op_mask;
    uint64_t dflt;               /* value of an absent field */
    uint64_t max_value;          /* largest value in the trace (data, not part of the hash) */
    const char* name;
} goalx_field_t;

typedef struct {
    uint64_t schema_hash;        /* layout hash: a cache key for generated decoders */
    uint16_t record_bytes, num_fields;
    uint8_t  edge_bytes, legacy; /* legacy: the 39-byte 4223 layout */
    const goalx_field_t* fields;
    uint16_t num_custom;         /* custom fields the simulator requested ... */
    const uint16_t* custom_field_ix; /* ... slot s -> index into fields[] */
} goalx_schema_t;

typedef struct {                 /* what the simulator needs to issue an op */
    uint64_t size;
    uint32_t peer, tag;          /* ANY already mapped to 0xFFFFFFFF */
    uint8_t  type, cpu, nic;     /* type: 1 send, 2 recv, 3 calc */
} goalx_op_t;

typedef struct {                 /* one rank's block inside the read-only mapping */
    uint32_t rank, num_nodes;
    const uint8_t* records;      /* record i at records + i * record_bytes */
    const uint8_t* appendix;     /* entries of edge_bytes each */
} goalx_rank_view_t;

typedef struct {                 /* the whole mapped trace, for factories that need it */
    uint32_t num_ranks;
    const goalx_rank_view_t* ranks;  /* num_ranks entries, valid for the decoder's lifetime */
} goalx_trace_view_t;

typedef struct goalx_decoder goalx_decoder_t;
struct goalx_decoder {
    uint32_t abi;                /* GOALX_DECODER_ABI */
    const char* name;            /* "interp", "fixed", "jit:<hash>", ... */
    void* state;                 /* per trace, owned by the decoder */
    /* once per rank at load; the result is passed to every hot call for that rank (may be NULL) */
    void*    (*bind_rank)  (goalx_decoder_t*, const goalx_rank_view_t*);
    void     (*decode_op)  (const goalx_decoder_t*, void* rank_state, const uint8_t* rec, uint32_t idx, goalx_op_t* out);
    uint32_t (*depcnt)     (const goalx_decoder_t*, void* rank_state, const uint8_t* rec, uint32_t idx);
    void     (*done_edges) (const goalx_decoder_t*, void* rank_state, const uint8_t* rec, uint32_t idx, uint32_t* n, uint32_t* ix);
    void     (*start_edges)(const goalx_decoder_t*, void* rank_state, const uint8_t* rec, uint32_t idx, uint32_t* n, uint32_t* ix);
    uint64_t (*get_custom) (const goalx_decoder_t*, void* rank_state, const uint8_t* rec, uint32_t idx, uint16_t slot);
    void     (*destroy)    (goalx_decoder_t*);
};

/* called once per trace; returns NULL if it cannot handle this schema (the simulator then uses interp) */
typedef goalx_decoder_t* (*goalx_decoder_factory_t)(const goalx_schema_t*, const goalx_trace_view_t*);

/* the symbol a plugin shared object exports */
goalx_decoder_t* goalx_make_decoder(const goalx_schema_t*, const goalx_trace_view_t*);

/*
 * Optional, recommended: a plugin that also exports
 *     const uint32_t goalx_decoder_abi = GOALX_DECODER_ABI;
 * is refused before its factory runs if the ABIs differ. Without it the
 * simulator can only check goalx_decoder_t.abi after the factory returned.
 */
extern const uint32_t goalx_decoder_abi;

#ifdef __cplusplus
}
#endif

#endif
