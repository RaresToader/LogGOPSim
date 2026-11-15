#ifndef TRACE_SCHEMA_HPP
#define TRACE_SCHEMA_HPP

#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/*
 * Self-describing GOAL binary format, version 1.1 (GOALX 1.1).
 *
 * The header embeds a schema: a list of fields with a kind, a bit width and a
 * bit offset inside a fixed-size node record. Writers (txt2bin, goalx-convert)
 * and the reader (LogGOPSim) both go through this file, so the layout is
 * defined once.
 *
 * File layout (little-endian, offsets absolute from the start of the file):
 *
 *   0  u64 magic (GOALX_MAGIC)
 *   8  u16 version · u8 endian · u8 flags (bit 0: aligned layout) · u32 header_len
 *  16  u64 schema_hash
 *  24  u32 num_ranks · u32 max_cpu · u32 max_nic
 *  36  u16 num_fields · u16 record_bits · u16 record_bytes · u16 num_meta
 *  44  u8 edge_bytes · u8 reserved[7]
 *  52  field[i]: u16 kind · u16 bit_offset · u8 width · u8 flags · u8 op_mask · u8 reserved
 *                · u64 default · u64 max_value · u8 name_len · name
 *      meta[i]:  u16 klen · key · u16 vlen · value
 *  header_len: jump table, num_ranks x { u64 start, u64 end }
 *
 *  rank block: u32 num_nodes · u32 num_roots · roots[num_roots] (edge_bytes each)
 *              · records[num_nodes * record_bytes] · appendix[num_edges] (edge_bytes each)
 *              · GOALX_PADDING zero bytes
 *
 * Every field satisfies (bit_offset % 8) + width <= 64, so it is read with one
 * unaligned 64-bit load, one shift and one mask; the padding keeps that load
 * (and a 32-bit load of an appendix entry) inside the file.
 */

#define GOALX_MAGIC          0x0031584C414F4723ULL  /* "#GOALX1\0" */
/* version field: (major << 8) | minor; the first GOALX version wrote 1 (= 1.0) */
#define GOALX_VERSION_MAJOR  1
#define GOALX_VERSION_MINOR  1
#define GOALX_VERSION        ((GOALX_VERSION_MAJOR << 8) | GOALX_VERSION_MINOR)
#define GOALX_ENDIAN_LITTLE  1
#define GOALX_PADDING        8

#define GOALX_LAYOUT_ALIGNED 0x1   /* header flags */

#define GOALX_OFF_VERSION    8
#define GOALX_OFF_HEADER_LEN 12
#define GOALX_OFF_MAX_CPU    28
#define GOALX_OFF_MAX_NIC    32
#define GOALX_OFF_FIELDS     52

/* op_mask bits: the op types a field is meaningful for */
#define GOALX_OP_SEND 0x1
#define GOALX_OP_RECV 0x2
#define GOALX_OP_CALC 0x4
#define GOALX_OP_ALL  0x7

/* field kinds the simulator knows about; everything else is FK_CUSTOM */
enum FieldKind {
	FK_TYPE = 0,
	FK_PEER,
	FK_SIZE,
	FK_TAG,
	FK_CPU,
	FK_NIC,
	FK_NDEP,    // number of nodes depending on this node's completion
	FK_DEPIX,   // start of that list in the appendix
	FK_NSDEP,   // number of nodes depending on this node's start
	FK_SDEPIX,  // start of that list (optional, defaults to depix + ndep)
	FK_DEPCNT,  // initial number of dependencies of this node (read-only)
	FK_NUM_BUILTIN,
	FK_CUSTOM = 0xFFFF
};

/* decode all-ones-of-width as ANY (~0u), used for peer and tag */
#define FF_ANY_SENTINEL 0x1

static const char* const builtin_field_names[FK_NUM_BUILTIN] = {
	"type", "peer", "size", "tag", "cpu", "nic", "ndep", "depix", "nsdep", "sdepix", "depcnt"
};

/* widths used for builtin fields that an explicit (non-auto) spec does not list */
static const uint8_t builtin_field_default_widths[FK_NUM_BUILTIN] = {
	2, 32, 64, 32, 8, 8, 32, 32, 32, 0, 32
};

/* the op types each builtin field is meaningful for */
static const uint8_t builtin_field_op_masks[FK_NUM_BUILTIN] = {
	GOALX_OP_ALL, GOALX_OP_SEND | GOALX_OP_RECV, GOALX_OP_ALL, GOALX_OP_SEND | GOALX_OP_RECV, GOALX_OP_ALL,
	GOALX_OP_SEND | GOALX_OP_RECV, GOALX_OP_ALL, GOALX_OP_ALL, GOALX_OP_ALL, GOALX_OP_ALL, GOALX_OP_ALL
};

/* builtin fields that may be left out of a schema (they then decode as their default) */
static inline bool builtin_is_optional(int kind) {
	return kind == FK_CPU || kind == FK_NIC || kind == FK_SDEPIX;
}

static inline bool host_is_little_endian() {
	uint16_t x = 1;
	return *((uint8_t*) &x) == 1;
}

static inline uint64_t width_mask(uint8_t width) {
	return width >= 64 ? ~0ULL : ((1ULL << width) - 1);
}

static inline bool check_fits(uint64_t value, uint8_t width) {
	return value <= width_mask(width);
}

/* number of bits needed to represent v (0 for v == 0) */
static inline uint8_t bits_for(uint64_t v) {
	uint8_t b = 0;
	while (v) { b++; v >>= 1; }
	return b;
}

struct FieldDesc {
	uint16_t    kind;
	uint16_t    bit_offset;
	uint8_t     width;
	uint8_t     flags;
	uint8_t     op_mask;
	uint64_t    dflt;
	uint64_t    max_value; // largest value stored (excluding ANY); data, not layout
	std::string name;

	FieldDesc() : kind(FK_CUSTOM), bit_offset(0), width(0), flags(0), op_mask(GOALX_OP_ALL), dflt(0), max_value(0) {}
};

#if defined(__GNUC__)
#define GOALX_INLINE inline __attribute__((always_inline))
#else
#define GOALX_INLINE inline
#endif

/*
 * Precomputed decoding information for one field. get() handles absent fields
 * (default) and the ANY sentinel; get_simple() is for fields known to be
 * present. The no-straddle rule guarantees one 64-bit load is always enough.
 */
struct Accessor {
	uint32_t byte_off;
	uint8_t  shift;
	bool     present;
	bool     any_sentinel;
	uint64_t mask;
	uint64_t dflt;

	Accessor() : byte_off(0), shift(0), present(false), any_sentinel(false), mask(0), dflt(0) {}

	/*
	 * record_bytes: a field in the last 8 bytes of a record is loaded from
	 * record_bytes - 8 with a larger shift, so the load never leaves the
	 * record (legacy files have no padding after the last record).
	 */
	explicit Accessor(const FieldDesc* f, uint32_t record_bytes = 0) {
		if (f == NULL) {
			*this = Accessor();
			return;
		}
		byte_off     = f->bit_offset / 8;
		shift        = f->bit_offset % 8;
		if (record_bytes >= 8 && byte_off + 8 > record_bytes) {
			shift   += (byte_off - (record_bytes - 8)) * 8;
			byte_off = record_bytes - 8;
		}
		present      = true;
		any_sentinel = (f->flags & FF_ANY_SENTINEL) != 0;
		mask         = width_mask(f->width);
		dflt         = f->dflt;
	}

	bool simple() const { return present; }

	GOALX_INLINE uint64_t get(const char* rec) const {
		if (!present) return dflt;
		uint64_t w;
		memcpy(&w, rec + byte_off, sizeof(w));
		uint64_t v = (w >> shift) & mask;
		if (any_sentinel && v == mask) return 0xFFFFFFFFULL;
		return v;
	}

	/* ANY: the field may hold the all-ones ANY sentinel (peer, tag) */
	template <bool ANY>
	GOALX_INLINE uint64_t get_simple(const char* rec) const {
		uint64_t w;
		memcpy(&w, rec + byte_off, sizeof(w));
		uint64_t v = (w >> shift) & mask;
		if (ANY && any_sentinel && v == mask) return 0xFFFFFFFFULL;
		return v;
	}
};

/* writes value into the record at bit_offset; the field must not straddle a 64-bit load */
static inline void put_bits(char* rec, uint16_t bit_offset, uint8_t width, uint64_t value) {
	uint32_t byte_off = bit_offset / 8;
	uint8_t shift = bit_offset % 8;
	uint64_t mask = width_mask(width);
	uint64_t w;

	value &= mask;
	memcpy(&w, rec + byte_off, sizeof(w));
	w = (w & ~(mask << shift)) | (value << shift);
	memcpy(rec + byte_off, &w, sizeof(w));
}

static inline uint64_t fnv1a(const char* data, size_t len, uint64_t h = 0xcbf29ce484222325ULL) {
	for (size_t i = 0; i < len; i++) {
		h ^= (uint8_t) data[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

static inline bool valid_field_name(const std::string& n) {
	if (n.empty() || n.size() > 255) return false;
	for (size_t i = 0; i < n.size(); i++) {
		char c = n[i];
		bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
		bool digit = c >= '0' && c <= '9';
		if (!(alpha || (digit && i > 0))) return false;
	}
	return true;
}

static inline bool parse_uint(const std::string& s, uint64_t* out) {
	if (s.empty() || s.size() > 20) return false;
	uint64_t v = 0;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] < '0' || s[i] > '9') return false;
		uint64_t d = (uint64_t) (s[i] - '0');
		if (v > (~0ULL - d) / 10) return false;
		v = v * 10 + d;
	}
	*out = v;
	return true;
}

/* per-field maxima collected by a writer's first pass over the trace */
struct FieldStats {
	uint64_t max[FK_NUM_BUILTIN];      // excluding ANY values (0xFFFFFFFF) for peer/tag
	bool     any_used[FK_NUM_BUILTIN]; // peer/tag: the ANY value occurs
	std::vector<std::pair<std::string, uint64_t> > custom_max;
	std::vector<uint64_t> custom_count;      // nodes that carry each custom attribute
	uint64_t total_nodes;                    // nodes of the whole trace
	uint64_t max_nodes_per_rank;
	uint32_t max_cpu, max_nic;

	FieldStats() : total_nodes(0), max_nodes_per_rank(0), max_cpu(0), max_nic(0) {
		memset(max, 0, sizeof(max));
		memset(any_used, 0, sizeof(any_used));
	}

	void note(int kind, uint64_t v) {
		if ((kind == FK_PEER || kind == FK_TAG) && v == 0xFFFFFFFFULL) {
			any_used[kind] = true;
			return;
		}
		if (v > max[kind]) max[kind] = v;
	}

	void note_custom(const std::string& name, uint64_t v) {
		for (size_t i = 0; i < custom_max.size(); i++) {
			if (custom_max[i].first == name) {
				if (v > custom_max[i].second) custom_max[i].second = v;
				custom_count[i]++;
				return;
			}
		}
		custom_max.push_back(std::make_pair(name, v));
		custom_count.push_back(1);
	}

	/* true if some node of the trace does not carry the attribute (it then stores the default) */
	bool custom_absent_somewhere(const std::string& name) const {
		for (size_t i = 0; i < custom_max.size(); i++) {
			if (custom_max[i].first == name) return custom_count[i] < total_nodes;
		}
		return total_nodes > 0;
	}

	const uint64_t* custom(const std::string& name) const {
		for (size_t i = 0; i < custom_max.size(); i++) {
			if (custom_max[i].first == name) return &custom_max[i].second;
		}
		return NULL;
	}
};

/* a parsed --schema argument */
struct SchemaSpec {
	struct Item {
		std::string name;
		uint8_t     width;
		bool        has_default;
		uint64_t    dflt;
	};
	bool auto_rest;          // fields not listed get automatic widths
	std::vector<Item> items;

	SchemaSpec() : auto_rest(true) {}

	const Item* find(const std::string& name) const {
		for (size_t i = 0; i < items.size(); i++) {
			if (items[i].name == name) return &items[i];
		}
		return NULL;
	}
};

enum Layout { LAYOUT_PACKED, LAYOUT_ALIGNED };

class Schema {

	public:

	std::vector<FieldDesc> fields;
	std::vector<std::pair<std::string, std::string> > meta;
	uint16_t record_bits;
	uint16_t record_bytes;
	uint8_t  edge_bytes;
	uint8_t  flags;
	bool     legacy;

	Schema() : record_bits(0), record_bytes(0), edge_bytes(4), flags(0), legacy(false) {}

	const FieldDesc* find(const std::string& name) const {
		for (size_t i = 0; i < fields.size(); i++) {
			if (fields[i].name == name) return &fields[i];
		}
		return NULL;
	}

	const FieldDesc* builtin(FieldKind kind) const {
		for (size_t i = 0; i < fields.size(); i++) {
			if (fields[i].kind == kind) return &fields[i];
		}
		return NULL;
	}

	static int builtin_kind(const std::string& name) {
		for (int k = 0; k < FK_NUM_BUILTIN; k++) {
			if (name == builtin_field_names[k]) return k;
		}
		return FK_CUSTOM;
	}

	/*
	 * The fixed 39-byte record of MAGIC_COOKIE (4223) files, depcnt at byte 0.
	 * sDepIx (byte 35) is kept: files written by the old serialize() hold a
	 * sentinel in depIx when nDep is 0, so depIx + nDep is not always sDepIx.
	 */
	static Schema legacy_layout() {
		static const uint8_t kinds[]  = { FK_DEPCNT, FK_TYPE, FK_PEER, FK_SIZE, FK_TAG, FK_CPU, FK_NIC, FK_NDEP, FK_DEPIX, FK_NSDEP, FK_SDEPIX };
		static const uint16_t bytes[] = { 0, 4, 5, 9, 17, 21, 22, 23, 27, 31, 35 };
		static const uint8_t widths[] = { 32, 8, 32, 64, 32, 8, 8, 32, 32, 32, 32 };
		Schema s;
		for (size_t i = 0; i < sizeof(kinds); i++) {
			FieldDesc f;
			f.kind = kinds[i];
			f.bit_offset = bytes[i] * 8;
			f.width = widths[i];
			f.flags = (f.kind == FK_PEER || f.kind == FK_TAG) ? FF_ANY_SENTINEL : 0;
			f.op_mask = builtin_field_op_masks[f.kind];
			f.max_value = width_mask(f.width); // unknown
			f.name = builtin_field_names[f.kind];
			s.fields.push_back(f);
		}
		s.record_bytes = 39;
		s.record_bits = 39 * 8;
		s.edge_bytes = 4;
		s.legacy = true;
		return s;
	}

	/*
	 * Parses a spec like "auto,peer:17,tag:24,gpu_id:6=1". "auto" (also the
	 * meaning of an empty spec) gives every unlisted field an automatic width;
	 * without it, unlisted builtin fields get their legacy-equivalent default
	 * width and custom fields must all be listed.
	 */
	static bool parse_spec(const char* spec, SchemaSpec* out, std::string* err) {
		SchemaSpec r;
		std::string s = spec ? spec : "";
		r.auto_rest = s.empty();
		size_t pos = 0;
		while (pos <= s.size() && !s.empty()) {
			size_t end = s.find(',', pos);
			if (end == std::string::npos) end = s.size();
			std::string item = s.substr(pos, end - pos);
			pos = end + 1;
			if (item == "auto") {
				r.auto_rest = true;
			} else {
				size_t colon = item.find(':');
				if (colon == std::string::npos) {
					*err = "schema entry '" + item + "' must look like name:width[=default] or auto";
					return false;
				}
				SchemaSpec::Item it;
				it.name = item.substr(0, colon);
				std::string rest = item.substr(colon + 1);
				std::string dflt;
				size_t eq = rest.find('=');
				it.has_default = eq != std::string::npos;
				if (it.has_default) {
					dflt = rest.substr(eq + 1);
					rest = rest.substr(0, eq);
				}
				uint64_t width = 0;
				it.dflt = 0;
				if (!valid_field_name(it.name)) {
					*err = "schema entry '" + item + "' has an invalid field name";
					return false;
				}
				if (!parse_uint(rest, &width) || width > 64) {
					*err = "field '" + it.name + "' has width '" + rest + "', must be a number between 0 and 64";
					return false;
				}
				if (it.has_default && !parse_uint(dflt, &it.dflt)) {
					*err = "field '" + it.name + "' has default '" + dflt + "', must be a non-negative number";
					return false;
				}
				it.width = (uint8_t) width;
				if (builtin_kind(it.name) == FK_CUSTOM && it.width == 0) {
					*err = "custom field '" + it.name + "' needs a width of at least 1";
					return false;
				}
				if (r.find(it.name) != NULL) {
					*err = "field '" + it.name + "' is listed twice";
					return false;
				}
				r.items.push_back(it);
			}
			if (end == s.size()) break;
		}
		*out = r;
		return true;
	}

	/*
	 * Chooses widths and the layout for a trace with the given statistics.
	 * Fails if an explicit width cannot hold the trace's values.
	 */
	static bool build(const SchemaSpec& spec, const FieldStats& st, Layout layout, Schema* out, std::string* err) {
		std::vector<FieldDesc> fs;

		for (int k = 0; k < FK_NUM_BUILTIN; k++) {
			const SchemaSpec::Item* it = spec.find(builtin_field_names[k]);
			bool any = (k == FK_PEER || k == FK_TAG) && st.any_used[k];
			uint64_t mx = st.max[k];
			if ((k == FK_PEER || k == FK_TAG) && mx >= 0xFFFFFFFFULL) {
				*err = std::string("field '") + builtin_field_names[k] + "' does not fit in 32 bits: the trace uses values up to " +
				       std::to_string(mx) + " (4294967295 is reserved for ANY)";
				return false;
			}
			uint64_t dflt = it && it->has_default ? it->dflt : 0;
			// the all-ones value is reserved when ANY is used
			uint8_t need = bits_for(std::max(mx, dflt) + (any ? 1 : 0));
			uint8_t width;
			if (it) width = it->width;
			else if (spec.auto_rest) {
				width = need;
				if (k == FK_TYPE && width < 2) width = 2;
				if (k == FK_SDEPIX) width = 0;
				if (!builtin_is_optional(k) && width < 1) width = 1;
			}
			else width = builtin_field_default_widths[k];

			if (width == 0) {
				if (!builtin_is_optional(k)) {
					*err = std::string("field '") + builtin_field_names[k] + "' is required and cannot have width 0";
					return false;
				}
				if (mx != 0 || dflt != 0 || any) {
					*err = std::string("field '") + builtin_field_names[k] + "' is left out, but the trace uses values up to " + std::to_string(mx);
					return false;
				}
				continue;
			}
			if (width < need) {
				*err = std::string("field '") + builtin_field_names[k] + "' does not fit in " + std::to_string(width) +
				       " bits: the trace uses values up to " + std::to_string(mx) + (any ? " and ANY" : "");
				return false;
			}
			FieldDesc f;
			f.kind = (uint16_t) k;
			f.width = width;
			f.flags = any ? FF_ANY_SENTINEL : 0;
			f.op_mask = builtin_field_op_masks[k];
			f.dflt = dflt;
			f.max_value = mx;
			f.name = builtin_field_names[k];
			fs.push_back(f);
		}

		// custom fields: listed in the spec, or seen in the trace
		std::vector<std::string> names;
		for (size_t i = 0; i < spec.items.size(); i++) {
			if (builtin_kind(spec.items[i].name) == FK_CUSTOM) names.push_back(spec.items[i].name);
		}
		for (size_t i = 0; i < st.custom_max.size(); i++) {
			const std::string& n = st.custom_max[i].first;
			if (spec.find(n) != NULL) continue;
			if (!spec.auto_rest) {
				*err = "custom field '" + n + "' is used by the trace but not declared in the schema";
				return false;
			}
			names.push_back(n);
		}
		for (size_t i = 0; i < names.size(); i++) {
			const SchemaSpec::Item* it = spec.find(names[i]);
			const uint64_t* m = st.custom(names[i]);
			uint64_t mx = m ? *m : 0;
			uint64_t dflt = it && it->has_default ? it->dflt : 0;
			// nodes without the attribute store the default: it is a stored value too
			if (st.custom_absent_somewhere(names[i]) && dflt > mx) mx = dflt;
			uint8_t need = bits_for(std::max(mx, dflt));
			uint8_t width = it ? it->width : std::max<uint8_t>(need, 1);
			if (width < need) {
				*err = "field '" + names[i] + "' does not fit in " + std::to_string(width) +
				       " bits: the trace uses values up to " + std::to_string(std::max(mx, dflt));
				return false;
			}
			FieldDesc f;
			f.kind = FK_CUSTOM;
			f.width = width;
			f.op_mask = GOALX_OP_ALL;
			f.dflt = dflt;
			f.max_value = mx;
			f.name = names[i];
			fs.push_back(f);
		}

		Schema r;
		r.flags = layout == LAYOUT_ALIGNED ? GOALX_LAYOUT_ALIGNED : 0;
		if (!r.place(fs, layout, err)) return false;
		uint8_t eb = (uint8_t) ((bits_for(st.max_nodes_per_rank > 0 ? st.max_nodes_per_rank - 1 : 0) + 7) / 8);
		r.edge_bytes = eb < 1 ? 1 : eb;
		if (r.edge_bytes > 4) {
			*err = "a rank has more than 2^32 nodes";
			return false;
		}
		*out = r;
		return true;
	}

	/* the bytes that define the record layout; their hash identifies the schema */
	void serialize_fields(std::vector<char>& buf, bool with_max) const {
		for (size_t i = 0; i < fields.size(); i++) {
			const FieldDesc& f = fields[i];
			put(buf, f.kind);
			put(buf, f.bit_offset);
			put(buf, f.width);
			put(buf, f.flags);
			put(buf, f.op_mask);
			put(buf, (uint8_t) 0);
			put(buf, f.dflt);
			if (with_max) put(buf, f.max_value);
			put(buf, (uint8_t) f.name.size());
			buf.insert(buf.end(), f.name.begin(), f.name.end());
		}
	}

	/* covers the layout only: max_value and metadata are data, not layout */
	uint64_t hash() const {
		std::vector<char> buf;
		serialize_fields(buf, false);
		put(buf, record_bits);
		put(buf, record_bytes);
		put(buf, edge_bytes);
		put(buf, flags);
		return fnv1a(buf.data(), buf.size());
	}

	/* everything before the jump table */
	void serialize_header(std::vector<char>& buf, uint32_t num_ranks, uint32_t max_cpu, uint32_t max_nic) const {
		buf.clear();
		put(buf, (uint64_t) GOALX_MAGIC);
		put(buf, (uint16_t) GOALX_VERSION);
		put(buf, (uint8_t) GOALX_ENDIAN_LITTLE);
		put(buf, flags);
		put(buf, (uint32_t) 0); // header_len, patched below
		put(buf, hash());
		put(buf, num_ranks);
		put(buf, max_cpu);
		put(buf, max_nic);
		put(buf, (uint16_t) fields.size());
		put(buf, record_bits);
		put(buf, record_bytes);
		put(buf, (uint16_t) meta.size());
		put(buf, edge_bytes);
		for (int i = 0; i < 7; i++) put(buf, (uint8_t) 0);
		serialize_fields(buf, true);
		for (size_t i = 0; i < meta.size(); i++) {
			put(buf, (uint16_t) meta[i].first.size());
			buf.insert(buf.end(), meta[i].first.begin(), meta[i].first.end());
			put(buf, (uint16_t) meta[i].second.size());
			buf.insert(buf.end(), meta[i].second.begin(), meta[i].second.end());
		}
		uint32_t header_len = buf.size();
		memcpy(&buf[GOALX_OFF_HEADER_LEN], &header_len, sizeof(header_len));
	}

	/* parses everything before the jump table and checks it for consistency */
	static bool parse_header(const char* hdr, size_t len, Schema* out, uint32_t* num_ranks,
	                         uint32_t* max_cpu, uint32_t* max_nic, uint32_t* header_len, std::string* err) {
		size_t pos = 0;
		uint64_t magic, hash;
		uint16_t version, num_fields, num_meta;
		uint8_t endian;
		Schema s;

		if (len < 16) {
			*err = "file is too short for a GOALX header";
			return false;
		}
		get(hdr, pos, magic);
		get(hdr, pos, version);
		if (magic != GOALX_MAGIC) {
			*err = "not a GOALX file";
			return false;
		}
		if (version == 1) {
			*err = "this is a GOALX 1.0 trace; this build reads GOALX 1.1 only, regenerate it with txt2bin or goalx-convert";
			return false;
		}
		if (version != GOALX_VERSION) {
			*err = "unsupported GOALX version field " + std::to_string(version) + " (this build reads GOALX 1.1)";
			return false;
		}
		if (len < GOALX_OFF_FIELDS) {
			*err = "file is too short for a GOALX header";
			return false;
		}
		get(hdr, pos, endian);
		get(hdr, pos, s.flags);
		get(hdr, pos, *header_len);
		get(hdr, pos, hash);
		get(hdr, pos, *num_ranks);
		get(hdr, pos, *max_cpu);
		get(hdr, pos, *max_nic);
		get(hdr, pos, num_fields);
		get(hdr, pos, s.record_bits);
		get(hdr, pos, s.record_bytes);
		get(hdr, pos, num_meta);
		get(hdr, pos, s.edge_bytes);
		pos += 7;

		if (endian != GOALX_ENDIAN_LITTLE || !host_is_little_endian()) {
			*err = "byte order of the trace does not match this host";
			return false;
		}
		if (*header_len > len || *header_len < GOALX_OFF_FIELDS) {
			*err = "header length " + std::to_string(*header_len) + " is out of range";
			return false;
		}

		len = *header_len;
		for (uint16_t i = 0; i < num_fields; i++) {
			FieldDesc f;
			uint8_t name_len, reserved;
			if (pos + 25 > len) { *err = "truncated field descriptor"; return false; }
			get(hdr, pos, f.kind);
			get(hdr, pos, f.bit_offset);
			get(hdr, pos, f.width);
			get(hdr, pos, f.flags);
			get(hdr, pos, f.op_mask);
			get(hdr, pos, reserved);
			get(hdr, pos, f.dflt);
			get(hdr, pos, f.max_value);
			get(hdr, pos, name_len);
			if (pos + name_len > len) { *err = "truncated field name"; return false; }
			f.name.assign(hdr + pos, name_len);
			pos += name_len;
			if (f.kind != FK_CUSTOM && (f.kind >= FK_NUM_BUILTIN || s.builtin((FieldKind) f.kind) != NULL)) {
				*err = "field '" + f.name + "' has an invalid or duplicate kind";
				return false;
			}
			if (s.find(f.name) != NULL) {
				*err = "field '" + f.name + "' is declared twice";
				return false;
			}
			s.fields.push_back(f);
		}
		for (uint16_t i = 0; i < num_meta; i++) {
			uint16_t klen, vlen;
			if (pos + 2 > len) { *err = "truncated metadata"; return false; }
			get(hdr, pos, klen);
			if (pos + klen + 2 > len) { *err = "truncated metadata"; return false; }
			std::string key(hdr + pos, klen);
			pos += klen;
			get(hdr, pos, vlen);
			if (pos + vlen > len) { *err = "truncated metadata"; return false; }
			s.meta.push_back(std::make_pair(key, std::string(hdr + pos, vlen)));
			pos += vlen;
		}
		if (s.record_bytes != (s.record_bits + 7) / 8) {
			*err = "record size does not match record bits";
			return false;
		}
		if (s.hash() != hash) {
			*err = "schema hash mismatch, the header is corrupt";
			return false;
		}
		*out = s;
		return true;
	}

	void print(FILE* fd, uint32_t num_ranks, uint32_t max_cpu, uint32_t max_nic) const {
		fprintf(fd, "format: %s\n", legacy ? "legacy (magic 4223, fixed 39-byte records)" : "GOALX 1.1");
		if (!legacy) {
			fprintf(fd, "schema hash: %016" PRIx64 "\n", hash());
			fprintf(fd, "layout: %s\n", (flags & GOALX_LAYOUT_ALIGNED) ? "aligned" : "packed");
		}
		fprintf(fd, "ranks: %u  max cpu: %u  max nic: %u\n", num_ranks, max_cpu, max_nic);
		fprintf(fd, "record: %u bits, %u bytes  edge entries: %u bytes\n", record_bits, record_bytes, edge_bytes);
		fprintf(fd, "fields:\n");
		for (size_t i = 0; i < fields.size(); i++) {
			const FieldDesc& f = fields[i];
			fprintf(fd, "  %-10s bits %3u..%3u  width %2u  ops %s%s%s", f.name.c_str(), f.bit_offset,
			        f.bit_offset + f.width - 1, f.width,
			        f.op_mask == GOALX_OP_ALL ? "all" : ((f.op_mask & GOALX_OP_CALC) ? "calc" : "send,recv"),
			        f.kind == FK_CUSTOM ? "  custom" : "",
			        (f.flags & FF_ANY_SENTINEL) ? "  (all ones = ANY)" : "");
			if (!legacy) fprintf(fd, "  max %" PRIu64, f.max_value);
			if (f.dflt != 0) fprintf(fd, "  default %" PRIu64, f.dflt);
			fprintf(fd, "\n");
		}
		for (size_t i = 0; i < meta.size(); i++) {
			fprintf(fd, "meta: %s=%s\n", meta[i].first.c_str(), meta[i].second.c_str());
		}
	}

	private:

	static uint8_t aligned_width(uint8_t w) {
		if (w <= 8) return 8;
		if (w <= 16) return 16;
		if (w <= 32) return 32;
		return 64;
	}

	static bool place_before(const FieldDesc& a, const FieldDesc& b) {
		if (a.width != b.width) return a.width > b.width;
		if (a.kind != b.kind) return a.kind < b.kind; // builtins (small kinds) before custom
		return a.name < b.name;
	}

	/* assigns bit offsets: widest first, never straddling a 64-bit load */
	bool place(std::vector<FieldDesc> fs, Layout layout, std::string* err) {
		if (layout == LAYOUT_ALIGNED) {
			for (size_t i = 0; i < fs.size(); i++) fs[i].width = aligned_width(fs[i].width);
		}
		std::stable_sort(fs.begin(), fs.end(), place_before);
		uint32_t bit = 0;
		for (size_t i = 0; i < fs.size(); i++) {
			uint32_t w = fs[i].width;
			if (layout == LAYOUT_ALIGNED) bit = (bit + w - 1) / w * w;
			else if ((bit % 8) + w > 64) bit = (bit + 7) / 8 * 8;
			fs[i].bit_offset = (uint16_t) bit;
			bit += w;
			if (bit > 0xFFFF) {
				*err = "record is too large";
				return false;
			}
		}
		fields = fs;
		record_bits = (uint16_t) bit;
		record_bytes = (uint16_t) ((bit + 7) / 8);
		return true;
	}

	template <typename T>
	static void put(std::vector<char>& buf, T v) {
		const char* p = (const char*) &v;
		buf.insert(buf.end(), p, p + sizeof(T));
	}

	template <typename T>
	static void get(const char* buf, size_t& pos, T& v) {
		memcpy(&v, buf + pos, sizeof(T));
		pos += sizeof(T);
	}
};

/* what a consumer of the trace needs: a field name, a minimum width and who needs it */
struct Requirement {
	std::string name;
	uint8_t     min_width;
	std::string needed_by;
};

static inline bool parse_requirements(const char* spec, std::vector<Requirement>* out, std::string* err) {
	std::string s = spec ? spec : "";
	size_t pos = 0;
	while (pos < s.size()) {
		size_t end = s.find(',', pos);
		if (end == std::string::npos) end = s.size();
		std::string item = s.substr(pos, end - pos);
		pos = end + 1;
		if (item.empty()) continue;
		Requirement r;
		size_t colon = item.find(':');
		r.name = item.substr(0, colon);
		r.needed_by = "--require";
		uint64_t w = 1;
		if (colon != std::string::npos && (!parse_uint(item.substr(colon + 1), &w) || w < 1 || w > 64)) {
			*err = "requirement '" + item + "' needs a width between 1 and 64";
			return false;
		}
		r.min_width = (uint8_t) w;
		if (!valid_field_name(r.name)) {
			*err = "requirement '" + item + "' has no valid field name";
			return false;
		}
		out->push_back(r);
	}
	return true;
}

/*
 * Checks that the trace provides what the simulator needs. The simulator
 * itself keeps peer/tag in 32 bits and cpu/nic in 8 bits, so wider values
 * are refused here instead of being truncated later.
 */
static inline bool validate_schema(const Schema& s, const std::vector<Requirement>& extra,
                                   uint32_t num_ranks, uint32_t max_cpu, uint32_t max_nic, std::string* err) {
	for (size_t i = 0; i < s.fields.size(); i++) {
		const FieldDesc& f = s.fields[i];
		if (f.width < 1 || f.width > 64 || (uint32_t) f.bit_offset + f.width > s.record_bits) {
			*err = "field '" + f.name + "' does not fit into the record";
			return false;
		}
		if ((f.bit_offset % 8) + f.width > 64) {
			*err = "field '" + f.name + "' crosses a 64-bit load boundary (bit offset " + std::to_string(f.bit_offset) +
			       ", width " + std::to_string(f.width) + ")";
			return false;
		}
	}
	if (s.edge_bytes < 1 || s.edge_bytes > 4) {
		*err = "edge entries are " + std::to_string(s.edge_bytes) + " bytes wide, must be 1 to 4";
		return false;
	}
	static const FieldKind required[] = { FK_TYPE, FK_PEER, FK_SIZE, FK_TAG, FK_NDEP, FK_DEPIX, FK_NSDEP, FK_DEPCNT };
	for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
		if (s.builtin(required[i]) == NULL) {
			*err = std::string("does not provide required field '") + builtin_field_names[required[i]] + "'";
			return false;
		}
	}
	for (size_t i = 0; i < extra.size(); i++) {
		const FieldDesc* f = s.find(extra[i].name);
		if (f == NULL) {
			*err = "does not provide required field '" + extra[i].name + "' (needed by " + extra[i].needed_by + ")";
			return false;
		}
		if (f->width < extra[i].min_width) {
			*err = "field '" + extra[i].name + "' is " + std::to_string(f->width) + " bits wide, at least " +
			       std::to_string(extra[i].min_width) + " are required (needed by " + extra[i].needed_by + ")";
			return false;
		}
	}

	if (s.builtin(FK_TYPE)->width < 2) {
		*err = "field 'type' needs at least 2 bits";
		return false;
	}
	const FieldKind max32[] = { FK_PEER, FK_TAG, FK_NDEP, FK_DEPIX, FK_NSDEP, FK_SDEPIX, FK_DEPCNT };
	for (size_t i = 0; i < sizeof(max32) / sizeof(max32[0]); i++) {
		const FieldDesc* f = s.builtin(max32[i]);
		if (f != NULL && f->width > 32) {
			*err = "field '" + f->name + "' is " + std::to_string(f->width) + " bits wide, this simulator supports at most 32";
			return false;
		}
	}
	const FieldDesc* peer = s.builtin(FK_PEER);
	uint64_t peer_limit = width_mask(peer->width);
	if (!(peer->flags & FF_ANY_SENTINEL)) peer_limit += 1; // all ones is a real rank unless ANY is used
	if (num_ranks > 0 && (uint64_t) (num_ranks - 1) >= peer_limit) {
		*err = "field 'peer' (" + std::to_string(peer->width) + " bits) cannot address " + std::to_string(num_ranks) + " ranks";
		return false;
	}
	if (max_cpu > 255 || max_nic > 255) {
		*err = "trace uses cpu " + std::to_string(max_cpu) + " / nic " + std::to_string(max_nic) + ", this simulator supports at most 255";
		return false;
	}
	return true;
}

#endif
