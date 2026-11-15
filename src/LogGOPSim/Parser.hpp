#include <map>
#include <vector>
#include <string>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <stddef.h>
#include <algorithm>

#include "LogGOPSim.hpp"
#include "TraceSchema.hpp"

#define MAGIC_COOKIE 4223
#define MAGIC_COOKIE_INVALID 2342


#define OPTYPE_SEND 1
#define OPTYPE_RECV 2
#define OPTYPE_CALC 3

typedef uint64_t base_t;

// TODO Experiment with packing of the node structure,
//      that could save us up to 30% in space, but might
//      harm time.

struct Node {
	
	uint64_t           Size;
	std::vector<Node*> DependOnMe;
	std::vector<Node*> StartDependOnMe;
	uint32_t           DependenciesCnt;
	uint32_t           offset;
	// wider than in the binary, so that the writer can range-check instead of truncating
	uint64_t           Peer;
	uint64_t           Tag;
	uint32_t           Proc;
	uint32_t           Nic;
	char               Type;
	// custom attributes: (slot in the writer's CustomNames, value)
	std::vector<std::pair<uint16_t, uint64_t> > Custom;
	
};

/* the writer's table of custom attribute names, slot = index */
struct CustomNames {
	std::vector<std::string> names;
	int find(const std::string& name) const {
		for (size_t i = 0; i < names.size(); i++) if (names[i] == name) return (int) i;
		return -1;
	}
	uint16_t get(const std::string& name) {
		int i = find(name);
		if (i >= 0) return (uint16_t) i;
		names.push_back(name);
		return (uint16_t) (names.size() - 1);
	}
};

struct DeserializedNode {

	uint32_t           DependenciesCnt;
	char               Type;
	uint32_t           Peer;
	uint64_t           Size;
	uint32_t           Tag;
	uint8_t            Proc;
	uint8_t            Nic;
	uint32_t           offset;
	std::vector<uint32_t> DependOnMe;
	std::vector<uint32_t> StartDependOnMe;
};

class Graph {
	
	private:
	
	std::vector<Node*> RootNodes;
	std::vector<Node*> allNodes;

	//uint32_t num_nodes;
	uint32_t num_edges;
	uint32_t offset_cntr;

	char* mapping_start;

/*
	uint64_t get_file_size(int fd) {
		
		struct stat f_info;
		int r = fstat(fd, &f_info);
		assert(r == 0);
		return f_info.st_size;
	}
*/

	void find_root_nodes() {

		RootNodes.clear();

		for (std::vector<Node*>::iterator it =  allNodes.begin(); it != allNodes.end(); it++) {
			if ((**it).DependenciesCnt == 0) RootNodes.push_back(*it);
		}

	}

	public:

	~Graph() {
		for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
			delete *it;
		}
	}

	Graph() {
	//	num_nodes = 0;
		num_edges = 0;
		offset_cntr = 0;
	}
	
	inline Node* addNode() {
		
		/**
			add a new node to the graph
		*/
		
		Node* N = new Node;
		N->DependenciesCnt = 0;
		N->offset = offset_cntr;
		offset_cntr++;
		allNodes.push_back(N);
		
		return N;
	}

	inline void addDependency(Node* a, Node* b) {
		
		/** 
		   addDependency(a,b) means that a can not be
		   executed before b is finished 
		*/

		b->DependOnMe.push_back(a);
		a->DependenciesCnt++;
		num_edges++;
	}

	inline void addStartDependency(Node* a, Node *b) {
		
		/** 
		   addStartDependency(a,b) means that a can not be
		   executed before b is started
		*/

		b->StartDependOnMe.push_back(a);
		a->DependenciesCnt++;
		num_edges++;
	}

	void write_as_dot() {
		/** 
			Produces a dot representation of the graph. This is usefull for debugging purposes.
		*/
		
		FILE* fd = fopen("graph.dot", "w");
		assert(fd != NULL);
		fprintf(fd, "digraph mygraph {\n");
		fprintf(fd, "graph [rankdir=LR];\n");
		fprintf(fd, "node [shape=record];\n");
					
			for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
				char typestr[5];
				if ((**it).Type == OPTYPE_SEND) strcpy(typestr, "Send");
				else if ((**it).Type == OPTYPE_RECV) strcpy(typestr, "Recv");
				else if ((**it).Type == OPTYPE_CALC) strcpy(typestr, "Calc");
				else strcpy(typestr, "Unkn");
				fprintf(fd, "%i [label=\"<f0> Type: %s | <f1> Peer: %llu | <f2> Size: %lu | <f3> Tag: %llu | <f4> Proc: %u | <f5> Nic: %u \"]\n", 
							 (**it).offset,    typestr,        (unsigned long long) (**it).Peer, (unsigned long int) (**it).Size,  (unsigned long long) (**it).Tag  , (**it).Proc,     (**it).Nic);
			}

			for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
				for (std::vector<Node*>::iterator dit = (**it).DependOnMe.begin(); dit != (**it).DependOnMe.end(); dit++) {
					fprintf(fd, "%i:f0 -> %i:f0\n", (**it).offset, (**dit).offset );
				}
				for (std::vector<Node*>::iterator dit = (**it).StartDependOnMe.begin(); dit != (**it).StartDependOnMe.end(); dit++) {
					fprintf(fd, "%i:f0 -> %i:f0 [arrowhead=diamond]\n", (**it).offset, (**dit).offset );
				}
			}

		fprintf(fd, "} \n");

	}

/*
	void serialize(FILE* fd, uint32_t rank, uint32_t num_ranks) {

		uint32_t buf32;

		rewind(fd);

		if (rank == 0) {
			// make room for jumptable
			fseek(fd, sizeof(uint64_t)*2*num_ranks + sizeof(uint32_t), SEEK_SET);
		}
		else {
			// jump to the end of the file
			fseek(fd, sizeof(uint32_t) + (2*(rank-1)+1)*sizeof(uint64_t), SEEK_SET);
			uint64_t eos;
			fread(&eos, sizeof(uint64_t), 1, fd);
			fseek(fd, eos, SEEK_SET);
		}
		
		find_root_nodes();
	
		long start = ftell(fd);
		// appendix starts right after all nodes
		long pos_in_appendix = start + 
		                       sizeof(uint32_t)*2 + // for Nodecount and Independent Actions count
							   sizeof(uint32_t)*RootNodes.size() + // for indp. actions offsets
							   (sizeof(char) + sizeof(uint8_t)*2 + sizeof(uint32_t)*7 + sizeof(uint64_t)) * allNodes.size(); // for actual nodeinfo
		// number of elements in the appendix so far
		uint32_t num_in_appendix = 0;

		// number of nodes in the schedule
		buf32 = (uint32_t) allNodes.size();
		fwrite( &buf32, sizeof(uint32_t), 1, fd );


		// write independant actions offsets
		buf32 = RootNodes.size(); 
		fwrite(&buf32, sizeof(uint32_t), 1, fd); // number of independent actions
		for (std::vector<Node*>::iterator it = RootNodes.begin(); it != RootNodes.end(); it++) {
			buf32 = (**it).offset;
			fwrite(&buf32, sizeof(uint32_t), 1, fd); // offset of independent action
		}
		

		Node n;
		int cnt = 0;
		for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
			
			cnt++;

			// write the actual operation info - this is a bit complicated thanks to padding that the compiler might do...
			fwrite( &((**it).DependenciesCnt), sizeof(uint32_t), 1, fd );
			fwrite( &((**it).Type), sizeof(char), 1, fd );
			fwrite( &((**it).Peer), sizeof(uint32_t), 1, fd );
			fwrite( &((**it).Size), sizeof(uint64_t), 1, fd );
			fwrite( &((**it).Tag), sizeof(uint32_t), 1, fd );
			fwrite( &((**it).Proc), sizeof(uint8_t), 1, fd );
			fwrite( &((**it).Nic), sizeof(uint8_t), 1, fd );
			
			// handling dependencies
			buf32 = (**it).DependOnMe.size();
			fwrite( &buf32, sizeof(uint32_t), 1, fd ); // number of ops depending on this node
			
			if ((**it).DependOnMe.size() > 0) {
				fwrite( &num_in_appendix, sizeof(uint32_t), 1, fd ); // start of dependant offsets in appendix
			} 
			else {
				uint32_t b = 999;
				fwrite( &b, sizeof(uint32_t), 1, fd ); // undefined
			}
			
			long pos = ftell(fd);
			// write offsets of depending ops into appendix
			fseek(fd, pos_in_appendix, SEEK_SET);
			for (std::vector<Node*>::iterator dit = (**it).DependOnMe.begin(); dit != (**it).DependOnMe.end(); dit++) {
				fwrite( &((**dit).offset), sizeof(uint32_t), 1, fd );
			}
			// adjust number of elements and position in appendix
			num_in_appendix += (**it).DependOnMe.size();
			pos_in_appendix = ftell(fd);
			// jump back to the node info (we're in appendix right now)
			fseek(fd, pos, SEEK_SET);
			
			// handling start-dependencies
			buf32 = (**it).StartDependOnMe.size();
			fwrite( &buf32, sizeof(uint32_t), 1, fd ); // number of ops start-depending on this node
			if ((**it).StartDependOnMe.size() > 0) { 
				fwrite( &num_in_appendix, sizeof(uint32_t), 1, fd ); // start of start-dependant offsets in appendix
			}
			else {
				uint32_t b = -1;
				fwrite( &b, sizeof(uint32_t), 1, fd ); // undefinedx
			}
			pos =  ftell(fd);
			// write offsets of start-depending ops into appendix
			fseek(fd, pos_in_appendix, SEEK_SET);
			for (std::vector<Node*>::iterator dit = (**it).StartDependOnMe.begin(); dit != (**it).StartDependOnMe.end(); dit++) {
				fwrite( &((**dit).offset), sizeof(uint32_t), 1, fd );
			}
			// adjust number of elements and position in appendix
			num_in_appendix += (**it).StartDependOnMe.size();
			pos_in_appendix = ftell(fd);
			// jump back to the node info (we're in appendix right now)
			fseek(fd, pos, SEEK_SET);
		
		}
		
		// write jumptable info: the schedule started at start and ended at pos_in_appendix
		fseek(fd, 0, SEEK_SET);
		
		// the number of ranks in this file
		buf32 = num_ranks;
		fwrite(&buf32, sizeof(uint32_t), 1, fd);
		fseek(fd, sizeof(uint64_t)*2*rank + sizeof(uint32_t), SEEK_SET);
		uint64_t buf64 = start;
		fwrite(&buf64, sizeof(uint64_t), 1, fd);
		buf64 = pos_in_appendix;
		fwrite(&buf64, sizeof(uint64_t), 1, fd);
	}

*/

	void serialize_mmap(int fd, uint32_t rank, uint32_t num_ranks, uint8_t max_cpu, uint8_t max_nic) {
		
		char *start_rankdata;
		uint64_t end_of_lastrank;
		static uint64_t filesize;
		char *pos;

		find_root_nodes();

		if (rank == 0) {
			// calculate the size of the file
			filesize = 0;
			filesize += sizeof(uint64_t); // magic cookie
			filesize += sizeof(uint32_t); // num ranks
			filesize += sizeof(uint8_t); // max_cpu
			filesize += sizeof(uint8_t); // max_nic
			filesize += sizeof(uint64_t)*2*num_ranks; // jumptable
			filesize += sizeof(uint32_t); // num nodes
			filesize += sizeof(uint32_t); // num indp actions
			filesize += (sizeof(uint32_t)*RootNodes.size()); // rootnodes offsets
			filesize += (sizeof(char)+sizeof(uint8_t)*2+sizeof(uint32_t)*7+sizeof(uint64_t))*allNodes.size(); // nodeinfo
			filesize += (sizeof(uint32_t)*num_edges); //appendix

			// enlarge the file
			lseek(fd, filesize-1, SEEK_SET);
			int r = write(fd, "", 1);
			assert(r == 1);
			
			// mmap the file
			mapping_start = (char*) mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (mapping_start == MAP_FAILED) {
				perror("couldn't mmap the output file");
				exit(EXIT_FAILURE);
			}
			*( (uint64_t*) mapping_start ) = (uint64_t) MAGIC_COOKIE;
			mapping_start += sizeof(uint64_t); // jump over magic cookie
			end_of_lastrank = sizeof(uint32_t) + sizeof(uint8_t)*2 + sizeof(uint64_t)*2*num_ranks;
			start_rankdata = mapping_start + sizeof(uint32_t) + sizeof(uint8_t)*2 + sizeof(uint64_t)*2*num_ranks;  // our rankdata starts right after the jumptable
			
		}
		else {
			filesize += sizeof(uint32_t); // num nodes
			filesize += sizeof(uint32_t); // num indp actions
			filesize += sizeof(uint32_t)*RootNodes.size(); // rootnodes offsets
			filesize += (sizeof(char)+sizeof(uint8_t)*2+sizeof(uint32_t)*7+sizeof(uint64_t))*allNodes.size(); // nodeinfo
			filesize += sizeof(uint32_t)*num_edges; //appendix


			// enlarge the file
			lseek(fd, filesize-1, SEEK_SET);
			int r = write(fd, "", 1);
			assert(r == 1);

			// mmap the file
			mapping_start = (char*) mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (mapping_start == MAP_FAILED) {
				perror("mmap failed");
				exit(EXIT_FAILURE);
			}

			mapping_start += sizeof(uint64_t); // jump over magic cookie

			end_of_lastrank = *((uint64_t*) (mapping_start + sizeof(uint32_t) + sizeof(uint8_t)*2 + sizeof(uint64_t)*(2*(rank-1)+1))); 
			start_rankdata = mapping_start + end_of_lastrank;
		}

		pos = start_rankdata;
		uint32_t num_in_appendix = 0;

		*((uint32_t*) pos) = (uint32_t) allNodes.size();	 	pos += sizeof(uint32_t);	// number of nodes in the schedule
		*((uint32_t*) pos) = (uint32_t) RootNodes.size();		pos += sizeof(uint32_t);	// number of independent actions
		
		// independent action offsets
		for (std::vector<Node*>::iterator it = RootNodes.begin(); it != RootNodes.end(); it++) {
			*((uint32_t*) pos) = (**it).offset; pos += sizeof(uint32_t);
		}

		// node data 

		for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {

			// the legacy record has fixed widths, refuse values that would be truncated
			check_value("peer", (**it).Peer, 32, rank, (**it).offset);
			check_value("tag",  (**it).Tag,  32, rank, (**it).offset);
			check_value("cpu",  (**it).Proc, 8,  rank, (**it).offset);
			check_value("nic",  (**it).Nic,  8,  rank, (**it).offset);

			*((uint32_t*) pos) = (**it).DependenciesCnt; 		pos += sizeof(uint32_t);	// number of actions this action depends on
			*((char*) pos)     = (**it).Type;            		pos += sizeof(char);		// type of this action
			*((uint32_t*) pos) = (**it).Peer;            		pos += sizeof(uint32_t);	// peer of this action
			*((uint64_t*) pos) = (**it).Size;            		pos += sizeof(uint64_t);	// size of the data transfer / length of the local calculation
			*((uint32_t*) pos) = (**it).Tag;             		pos += sizeof(uint32_t);	// tag of the send/recv operation
			*((uint8_t*) pos)  = (**it).Proc;            		pos += sizeof(uint8_t);		// processor used for this action
			*((uint8_t*) pos)  = (**it).Nic;             		pos += sizeof(uint8_t);		// network interface used for this action
			*((uint32_t*) pos) = (**it).DependOnMe.size();		pos += sizeof(uint32_t);	// number of actions that depend on this actions termination
			*((uint32_t*) pos) = num_in_appendix;				pos += sizeof(uint32_t);	// start index of dependent actions (in appendix)
			num_in_appendix += (**it).DependOnMe.size();
			*((uint32_t*) pos) = (**it).StartDependOnMe.size();	pos += sizeof(uint32_t);	// number of actions that depend on this actions start
			*((uint32_t*) pos) = num_in_appendix;				pos += sizeof(uint32_t);	// start index of start-dependent actions (in appendix)
			num_in_appendix += (**it).StartDependOnMe.size();
		}
		
		// appendix data
	
		for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
			for (std::vector<Node*>::iterator dit = (**it).DependOnMe.begin(); dit != (**it).DependOnMe.end(); dit++) {
				*((uint32_t*) pos) = (**dit).offset;       		pos += sizeof(uint32_t);	// offset of dependent action
			}
			for (std::vector<Node*>::iterator dit = (**it).StartDependOnMe.begin(); dit != (**it).StartDependOnMe.end(); dit++) {
				*((uint32_t*) pos) = (**dit).offset;       		pos += sizeof(uint32_t);	// offset of start-dependent action
			}
		}

		// jumptable info
		*((uint32_t*) mapping_start) = num_ranks;														// number of ranks in this schedule-file

		*((uint8_t*) (mapping_start + sizeof(uint32_t) )) = max_cpu;	// minimal number of cpu required to simulate this schedule
		*((uint8_t*) (mapping_start + sizeof(uint32_t) + sizeof(uint8_t) )) = max_nic;	// minimal number of nics required to simulate this schedule
		
		*((uint64_t*) (mapping_start + sizeof(uint32_t) + sizeof(uint8_t)*2 + sizeof(uint64_t)*2*rank)) = end_of_lastrank;	// start of this ranks info
		*((uint64_t*) (mapping_start + sizeof(uint32_t) + sizeof(uint8_t)*2 + sizeof(uint64_t)*(2*rank+1))) = pos - mapping_start;			// end of this ranks info
	
		//printf("s: %llu e: %llu\n", (long long unsigned int) end_of_lastrank, (long long unsigned int) (pos - mapping_start));

		// munmap the files so that the contents get written
		int r = munmap(mapping_start-sizeof(uint64_t), (size_t) (pos - mapping_start));
		assert(r == 0);
	}

	static void check_value(const char* field, uint64_t value, uint8_t width, uint32_t rank, uint32_t node) {
		if (!check_fits(value, width)) {
			fprintf(stderr, "Error: field '%s' value %llu does not fit in %u bits (rank %u, node %u)\n",
			        field, (unsigned long long) value, width, rank, node);
			exit(EXIT_FAILURE);
		}
	}

	/* encodes one value into a GOALX record, refusing anything the declared width or op mask cannot hold */
	static void encode_field(char* rec, const FieldDesc* f, const char* name, uint64_t value, char type, uint32_t rank, uint32_t node) {
		if (f == NULL) {
			// an omitted field decodes as 0, so only 0 may be dropped
			if (value != 0) {
				fprintf(stderr, "Error: field '%s' is not in the schema, but rank %u node %u uses value %llu\n",
				        name, rank, node, (unsigned long long) value);
				exit(EXIT_FAILURE);
			}
			return;
		}
		uint8_t opbit = type == OPTYPE_SEND ? GOALX_OP_SEND : (type == OPTYPE_RECV ? GOALX_OP_RECV : GOALX_OP_CALC);
		if (!(f->op_mask & opbit) && value != f->dflt) {
			fprintf(stderr, "Error: field '%s' does not apply to this op type, but rank %u node %u sets it to %llu\n",
			        name, rank, node, (unsigned long long) value);
			exit(EXIT_FAILURE);
		}
		uint64_t mask = width_mask(f->width);
		if ((f->flags & FF_ANY_SENTINEL) && value == 0xFFFFFFFFULL) {
			value = mask;
		} else if (value > mask || ((f->flags & FF_ANY_SENTINEL) && value == mask)) {
			fprintf(stderr, "Error: field '%s' value %llu does not fit in %u bits%s (rank %u, node %u)\n",
			        name, (unsigned long long) value, f->width,
			        value == mask ? ", all ones is reserved for ANY" : "", rank, node);
			exit(EXIT_FAILURE);
		}
		put_bits(rec, f->bit_offset, f->width, value);
	}

	/* the value of custom attribute slot s on node n, or dflt */
	static uint64_t custom_value(const Node& n, int slot, uint64_t dflt) {
		for (size_t i = 0; i < n.Custom.size(); i++) {
			if ((int) n.Custom[i].first == slot) return n.Custom[i].second;
		}
		return dflt;
	}

	/* first writer pass: what a schema for this rank must be able to hold */
	void collect_stats(FieldStats& st, const CustomNames& names, uint32_t num_ranks) {
		uint64_t num_in_appendix = 0;
		// peer must address every rank (validation requires it), even ranks nobody names explicitly
		if (num_ranks > 0) st.note(FK_PEER, num_ranks - 1);
		st.total_nodes += allNodes.size();
		if (allNodes.size() > st.max_nodes_per_rank) st.max_nodes_per_rank = allNodes.size();
		for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
			Node& n = **it;
			st.note(FK_TYPE, (uint8_t) n.Type);
			st.note(FK_PEER, n.Peer);
			st.note(FK_SIZE, n.Size);
			st.note(FK_TAG, n.Tag);
			st.note(FK_CPU, n.Proc);
			st.note(FK_NIC, n.Nic);
			st.note(FK_NDEP, n.DependOnMe.size());
			st.note(FK_DEPIX, num_in_appendix);
			st.note(FK_NSDEP, n.StartDependOnMe.size());
			st.note(FK_DEPCNT, n.DependenciesCnt);
			num_in_appendix += n.DependOnMe.size() + n.StartDependOnMe.size();
			if (n.Proc > st.max_cpu) st.max_cpu = n.Proc;
			if (n.Nic > st.max_nic) st.max_nic = n.Nic;
			for (size_t i = 0; i < n.Custom.size(); i++) st.note_custom(names.names[n.Custom[i].first], n.Custom[i].second);
		}
		if (num_in_appendix > 0xFFFFFFFFULL) {
			fprintf(stderr, "Error: a rank has more than 2^32 dependencies\n");
			exit(EXIT_FAILURE);
		}
	}

	static void put_entry(char*& pos, uint32_t v, uint8_t eb) {
		memcpy(pos, &v, eb); // little-endian: the low eb bytes
		pos += eb;
	}

	/* second writer pass: one GOALX 1.1 rank block (see TraceSchema.hpp) */
	void serialize_goalx(int fd, uint32_t rank, uint32_t num_ranks, uint32_t max_cpu, uint32_t max_nic,
	                     const Schema& schema, const CustomNames& names) {

		static uint64_t filesize;
		static uint32_t header_len;
		std::vector<char> header;
		uint64_t start;

		find_root_nodes();

		if (rank == 0) {
			schema.serialize_header(header, num_ranks, max_cpu, max_nic);
			header_len = header.size();
			start = header_len + sizeof(uint64_t)*2*num_ranks; // rank data starts right after the jumptable
		} else {
			start = filesize;
		}

		const uint64_t rb = schema.record_bytes;
		const uint8_t eb = schema.edge_bytes;
		filesize = start;
		filesize += sizeof(uint32_t)*2; // num nodes, num roots
		filesize += (uint64_t) eb*RootNodes.size(); // root node offsets
		filesize += rb*allNodes.size(); // records
		filesize += (uint64_t) eb*num_edges; // appendix
		filesize += GOALX_PADDING;

		if (ftruncate(fd, filesize) != 0) {
			perror("couldn't enlarge the output file");
			exit(EXIT_FAILURE);
		}
		char* map = (char*) mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (map == MAP_FAILED) {
			perror("couldn't mmap the output file");
			exit(EXIT_FAILURE);
		}
		if (rank == 0) memcpy(map, header.data(), header.size());

		char* pos = map + start;
		uint32_t buf32;

		buf32 = allNodes.size();	memcpy(pos, &buf32, sizeof(uint32_t));	pos += sizeof(uint32_t);
		buf32 = RootNodes.size();	memcpy(pos, &buf32, sizeof(uint32_t));	pos += sizeof(uint32_t);
		for (std::vector<Node*>::iterator it = RootNodes.begin(); it != RootNodes.end(); it++) put_entry(pos, (**it).offset, eb);

		const FieldDesc* f[FK_NUM_BUILTIN];
		for (int k = 0; k < FK_NUM_BUILTIN; k++) f[k] = schema.builtin((FieldKind) k);
		std::vector<const FieldDesc*> customs;
		std::vector<int> slots;
		for (size_t i = 0; i < schema.fields.size(); i++) {
			if (schema.fields[i].kind != FK_CUSTOM) continue;
			customs.push_back(&schema.fields[i]);
			slots.push_back(names.find(schema.fields[i].name));
		}

		std::vector<char> rec(rb + GOALX_PADDING);
		uint32_t num_in_appendix = 0;
		for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
			Node& n = **it;
			uint32_t ndep = n.DependOnMe.size();
			uint32_t nsdep = n.StartDependOnMe.size();
			char t = n.Type;

			std::fill(rec.begin(), rec.end(), 0);
			encode_field(&rec[0], f[FK_TYPE],   "type",   (uint8_t) t,       t, rank, n.offset);
			encode_field(&rec[0], f[FK_PEER],   "peer",   n.Peer,            t, rank, n.offset);
			encode_field(&rec[0], f[FK_SIZE],   "size",   n.Size,            t, rank, n.offset);
			encode_field(&rec[0], f[FK_TAG],    "tag",    n.Tag,             t, rank, n.offset);
			encode_field(&rec[0], f[FK_CPU],    "cpu",    n.Proc,            t, rank, n.offset);
			encode_field(&rec[0], f[FK_NIC],    "nic",    n.Nic,             t, rank, n.offset);
			encode_field(&rec[0], f[FK_NDEP],   "ndep",   ndep,              t, rank, n.offset);
			encode_field(&rec[0], f[FK_DEPIX],  "depix",  num_in_appendix,   t, rank, n.offset);
			encode_field(&rec[0], f[FK_NSDEP],  "nsdep",  nsdep,             t, rank, n.offset);
			encode_field(&rec[0], f[FK_DEPCNT], "depcnt", n.DependenciesCnt, t, rank, n.offset);
			if (f[FK_SDEPIX] != NULL) encode_field(&rec[0], f[FK_SDEPIX], "sdepix", num_in_appendix + ndep, t, rank, n.offset);
			for (size_t i = 0; i < customs.size(); i++) {
				encode_field(&rec[0], customs[i], customs[i]->name.c_str(), custom_value(n, slots[i], customs[i]->dflt), t, rank, n.offset);
			}
			memcpy(pos, &rec[0], rb);	pos += rb;
			num_in_appendix += ndep + nsdep;
		}

		for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
			for (std::vector<Node*>::iterator dit = (**it).DependOnMe.begin(); dit != (**it).DependOnMe.end(); dit++) put_entry(pos, (**dit).offset, eb);
			for (std::vector<Node*>::iterator dit = (**it).StartDependOnMe.begin(); dit != (**it).StartDependOnMe.end(); dit++) put_entry(pos, (**dit).offset, eb);
		}
		memset(pos, 0, GOALX_PADDING);	pos += GOALX_PADDING;
		assert((uint64_t) (pos - map) == filesize);

		uint64_t jump[2] = { start, filesize };
		memcpy(map + header_len + sizeof(uint64_t)*2*rank, jump, sizeof(jump));

		int r = munmap(map, filesize);
		assert(r == 0);
	}

};

/* decoding information shared by all ranks of one trace, computed once from the schema */
enum DecoderKind {
	DECODER_INTERP,         // "interp": the interpreter with get_simple() for the fields that qualify, see NodeLayout::simple_ok
	DECODER_FIXED,          // "fixed": the original hand-written decoder, constant offsets, legacy files only
	DECODER_INTERP_GENERIC  // "interp-generic": schema-agnostic, get() for every field, any layout
};

struct NodeLayout {

	Accessor type, peer, size, tag, cpu, nic, ndep, depix, nsdep, sdepix, depcnt;
	std::vector<Accessor> custom; // the custom fields the simulator requested, slot = index
	uint32_t record_bytes;
	uint8_t  edge_bytes;  // width of a root or appendix entry
	bool     legacy;
	DecoderKind decoder;

	NodeLayout() : record_bytes(0), edge_bytes(4), legacy(true), decoder(DECODER_INTERP_GENERIC) {}

	NodeLayout(const Schema& s, DecoderKind d, const std::vector<std::string>& custom_slots) :
		type(s.builtin(FK_TYPE), s.record_bytes), peer(s.builtin(FK_PEER), s.record_bytes),
		size(s.builtin(FK_SIZE), s.record_bytes), tag(s.builtin(FK_TAG), s.record_bytes),
		cpu(s.builtin(FK_CPU), s.record_bytes), nic(s.builtin(FK_NIC), s.record_bytes),
		ndep(s.builtin(FK_NDEP), s.record_bytes), depix(s.builtin(FK_DEPIX), s.record_bytes),
		nsdep(s.builtin(FK_NSDEP), s.record_bytes), sdepix(s.builtin(FK_SDEPIX), s.record_bytes),
		depcnt(s.builtin(FK_DEPCNT), s.record_bytes), record_bytes(s.record_bytes), edge_bytes(s.edge_bytes),
		legacy(s.legacy), decoder(d) {
		for (size_t i = 0; i < custom_slots.size(); i++) custom.push_back(Accessor(s.find(custom_slots[i]), s.record_bytes));
	}

	/*
	 * Whether DECODER_INTERP may be used: the fields it reads with
	 * get_simple() must be present. Validation guarantees this for every
	 * GOALX 1.1 schema; the check stays so that a bad file falls back to the
	 * generic interpreter instead of misreading.
	 */
	bool simple_ok() const {
		return type.simple() && peer.simple() && tag.simple() && ndep.simple() && depix.simple() && nsdep.simple() && depcnt.simple();
	}
};

/*
 * One decoder per DecoderKind, all with the same signature: the payload fields
 * go into N, the edge-list fields into the four counters. Kept as separate
 * blocks so each can be selected (and benchmarked) on its own.
 */
template <DecoderKind K> struct Decoder;

/* the original hand-written decoder: constant offsets into the 39-byte legacy record */
template <> struct Decoder<DECODER_FIXED> {
	template <typename T> static GOALX_INLINE T at(const char* rec, size_t off) {
		T v;
		memcpy(&v, rec + off, sizeof(T));
		return v;
	}
	static GOALX_INLINE void decode(const NodeLayout&, const char* rec, DeserializedNode& N, uint32_t& num_deps,
	                                uint32_t& deps_ix, uint32_t& num_startdeps, uint32_t& startdeps_ix) {
		N.Type = (char) at<uint8_t>(rec, 4); // after depcnt
		N.Peer = at<uint32_t>(rec, 5);
		N.Size = at<uint64_t>(rec, 9);
		N.Tag  = at<uint32_t>(rec, 17);
		N.Proc = at<uint8_t>(rec, 21);
		N.Nic  = at<uint8_t>(rec, 22);
		num_deps      = at<uint32_t>(rec, 23);
		deps_ix       = at<uint32_t>(rec, 27);
		num_startdeps = at<uint32_t>(rec, 31);
		startdeps_ix  = at<uint32_t>(rec, 35);
	}
	/* the initial number of dependencies */
	static GOALX_INLINE uint32_t depcnt(const NodeLayout&, const char* rec) {
		return at<uint32_t>(rec, 0);
	}
	/* only the "wait for my finish" list: count and start index in the appendix */
	static GOALX_INLINE void done_edges(const NodeLayout&, const char* rec, uint32_t& n, uint32_t& ix) {
		n  = at<uint32_t>(rec, 23);
		ix = at<uint32_t>(rec, 27);
	}
	/* only the "wait for my start" list */
	static GOALX_INLINE void start_edges(const NodeLayout&, const char* rec, uint32_t& n, uint32_t& ix) {
		n  = at<uint32_t>(rec, 31);
		ix = at<uint32_t>(rec, 35);
	}
};

/* the schema-agnostic interpreter: one precomputed accessor per field the simulator uses */
template <> struct Decoder<DECODER_INTERP_GENERIC> {
	static GOALX_INLINE void decode(const NodeLayout& L, const char* rec, DeserializedNode& N, uint32_t& num_deps,
	                                uint32_t& deps_ix, uint32_t& num_startdeps, uint32_t& startdeps_ix) {
		N.Type = (char) L.type.get(rec);
		N.Peer = (uint32_t) L.peer.get(rec);
		N.Size = L.size.get(rec);
		N.Tag  = (uint32_t) L.tag.get(rec);
		N.Proc = (uint8_t) L.cpu.get(rec);
		N.Nic  = (uint8_t) L.nic.get(rec);
		num_deps      = (uint32_t) L.ndep.get(rec);
		deps_ix       = (uint32_t) L.depix.get(rec);
		num_startdeps = (uint32_t) L.nsdep.get(rec);
		startdeps_ix  = L.sdepix.present ? (uint32_t) L.sdepix.get(rec) : deps_ix + num_deps;
	}
	static GOALX_INLINE uint32_t depcnt(const NodeLayout& L, const char* rec) {
		return (uint32_t) L.depcnt.get(rec);
	}
	static GOALX_INLINE void done_edges(const NodeLayout& L, const char* rec, uint32_t& n, uint32_t& ix) {
		n  = (uint32_t) L.ndep.get(rec);
		ix = (uint32_t) L.depix.get(rec);
	}
	static GOALX_INLINE void start_edges(const NodeLayout& L, const char* rec, uint32_t& n, uint32_t& ix) {
		n  = (uint32_t) L.nsdep.get(rec);
		ix = L.sdepix.present ? (uint32_t) L.sdepix.get(rec) : (uint32_t) (L.depix.get(rec) + L.ndep.get(rec));
	}
};

/*
 * The same interpreter, using what validation guarantees: type, peer, tag,
 * depcnt and the edge fields are present, and only peer and tag can hold
 * ANY. Size, cpu, nic (optional) and sdepix (optional) keep the generic path.
 */
template <> struct Decoder<DECODER_INTERP> {
	static GOALX_INLINE void decode(const NodeLayout& L, const char* rec, DeserializedNode& N, uint32_t& num_deps,
	                                uint32_t& deps_ix, uint32_t& num_startdeps, uint32_t& startdeps_ix) {
		N.Type = (char) L.type.get_simple<false>(rec);
		N.Peer = (uint32_t) L.peer.get_simple<true>(rec);
		N.Size = L.size.get(rec);
		N.Tag  = (uint32_t) L.tag.get_simple<true>(rec);
		N.Proc = (uint8_t) L.cpu.get(rec);
		N.Nic  = (uint8_t) L.nic.get(rec);
		num_deps      = (uint32_t) L.ndep.get_simple<false>(rec);
		deps_ix       = (uint32_t) L.depix.get_simple<false>(rec);
		num_startdeps = (uint32_t) L.nsdep.get_simple<false>(rec);
		startdeps_ix  = L.sdepix.present ? (uint32_t) L.sdepix.get(rec) : deps_ix + num_deps;
	}
	static GOALX_INLINE uint32_t depcnt(const NodeLayout& L, const char* rec) {
		return (uint32_t) L.depcnt.get_simple<false>(rec);
	}
	static GOALX_INLINE void done_edges(const NodeLayout& L, const char* rec, uint32_t& n, uint32_t& ix) {
		n  = (uint32_t) L.ndep.get_simple<false>(rec);
		ix = (uint32_t) L.depix.get_simple<false>(rec);
	}
	static GOALX_INLINE void start_edges(const NodeLayout& L, const char* rec, uint32_t& n, uint32_t& ix) {
		n  = (uint32_t) L.nsdep.get_simple<false>(rec);
		ix = L.sdepix.present ? (uint32_t) L.sdepix.get(rec)
		                      : (uint32_t) (L.depix.get_simple<false>(rec) + L.ndep.get_simple<false>(rec));
	}
};

/*
 * One rank of a mapped trace. The trace is read-only: dependency counters
 * live in a lazily zeroed side array.
 */
class SerializedGraph {
	
	private:
	
	const NodeLayout* layout;
	const char* roots;
	const char* records;
	const char* appendix;
	uint64_t num_entries;     // appendix entries inside the rank block
	uint32_t entry_mask;      // edge_bytes < 4: entries are read with a 32-bit load and masked
	uint8_t  edge_bytes;
	uint32_t num_root_nodes;
	uint32_t num_nodes;
	uint32_t my_rank;
	int      cpu_slot, nic_slot; // >= 0: the op's CPU / NIC come from this custom slot

	/*
	 * remaining[i] == 0: node i was not released yet; otherwise it is the
	 * number of outstanding dependencies + 1. calloc'd, so untouched pages
	 * never become resident and there is no pass over the trace at load.
	 */
	uint32_t* remaining;

	// nodes whose dependencies are all satisfied, in the order they became ready; decoded only when handed out
	std::vector<uint32_t> executableNodes;

	inline uint32_t entry(const char* base, uint64_t i) const {
		uint32_t v;
		memcpy(&v, base + i * edge_bytes, sizeof(v));
		return v & entry_mask;
	}

	inline void check_offset(uint32_t offset) const {
		if (offset >= num_nodes) {
			fprintf(stderr, "[rank %i] got offset %i, have %i nodes\n", my_rank, offset, num_nodes);
			exit(EXIT_FAILURE);
		}
	}

	inline const char* record(uint32_t offset) const {
		return records + (size_t) layout->record_bytes * offset;
	}

	void add_root_nodes() {

		for (uint32_t cnt=0; cnt<num_root_nodes; cnt++) {
			uint32_t offset = entry(roots, cnt);
			check_offset(offset);
			executableNodes.push_back(offset);
		}

	}

	/* decodes the payload and edge-list fields of one record; the branch is constant for a run */
	inline void decode_record(const char* rec, DeserializedNode& N, uint32_t& num_deps, uint32_t& deps_ix,
	                          uint32_t& num_startdeps, uint32_t& startdeps_ix) const {

		switch (layout->decoder) {
			case DECODER_FIXED:  Decoder<DECODER_FIXED>::decode(*layout, rec, N, num_deps, deps_ix, num_startdeps, startdeps_ix); break;
			case DECODER_INTERP: Decoder<DECODER_INTERP>::decode(*layout, rec, N, num_deps, deps_ix, num_startdeps, startdeps_ix); break;
			default:             Decoder<DECODER_INTERP_GENERIC>::decode(*layout, rec, N, num_deps, deps_ix, num_startdeps, startdeps_ix); break;
		}
	}

	/* the initial number of dependencies of a record */
	inline uint32_t depcnt(const char* rec) const {
		switch (layout->decoder) {
			case DECODER_FIXED:  return Decoder<DECODER_FIXED>::depcnt(*layout, rec);
			case DECODER_INTERP: return Decoder<DECODER_INTERP>::depcnt(*layout, rec);
			default:             return Decoder<DECODER_INTERP_GENERIC>::depcnt(*layout, rec);
		}
	}

	/* the decode-only benchmark loop for one decoder, with the layout in a local copy */
	template <DecoderKind K>
	uint64_t decode_all() const {
		const NodeLayout L = *layout;
		const size_t rb = L.record_bytes;
		const char* rec = records;
		uint64_t sum = 0;
		DeserializedNode N;
		uint32_t ndep, depix, nsdep, sdepix;
		for (uint32_t i = 0; i < num_nodes; i++, rec += rb) {
			Decoder<K>::decode(L, rec, N, ndep, depix, nsdep, sdepix);
			sum += (uint64_t) (uint8_t) N.Type + N.Peer + N.Size + N.Tag + N.Proc + N.Nic + ndep + depix + nsdep + sdepix;
			sum += Decoder<K>::depcnt(L, rec);
		}
		return sum;
	}

	/* a dependents list must lie inside the appendix */
	inline void check_list(uint32_t n, uint32_t ix) const {
		if ((uint64_t) ix + n > num_entries) {
			fprintf(stderr, "[rank %u] dependents list %u+%u runs past the appendix (%llu entries)\n",
			        my_rank, ix, n, (unsigned long long) num_entries);
			exit(EXIT_FAILURE);
		}
	}

	/* appends the n dependents at appendix index ix to out */
	void read_list(uint32_t n, uint32_t ix, std::vector<uint32_t>& out) const {
		if (n == 0) return; // old legacy files hold sentinels in the index of empty lists
		check_list(n, ix);
		for (uint32_t c = 0; c < n; c++) {
			uint32_t j = entry(appendix, (uint64_t) ix + c);
			check_offset(j);
			out.push_back(j);
		}
	}

	DeserializedNode get_node_by_offset(uint32_t offset) {
		check_offset(offset);
		const char* rec = record(offset);
		DeserializedNode N;
		uint32_t num_deps, deps_ix, num_startdeps, startdeps_ix;
		decode_record(rec, N, num_deps, deps_ix, num_startdeps, startdeps_ix);
		N.DependenciesCnt = depcnt(rec);
		N.offset = offset;
		read_list(num_deps, deps_ix, N.DependOnMe);
		read_list(num_startdeps, startdeps_ix, N.StartDependOnMe);
		return N;
	}

	/*
	 * Walks n dependents in the appendix, starting at index ix, in place: each
	 * one's counter is decremented, and nodes that reach 0 become executable.
	 */
	inline void release(uint32_t n, uint32_t ix) {
		if (n == 0) return; // old legacy files hold sentinels in the index of empty lists
		check_list(n, ix);
		for (uint32_t cnt=0; cnt<n; cnt++) {
			uint32_t offset = entry(appendix, (uint64_t) ix + cnt);
			check_offset(offset);
			uint32_t r = remaining[offset];
			if (r == 0) r = depcnt(record(offset)) + 1;
			remaining[offset] = --r;
			if (r == 1) executableNodes.push_back(offset);
		}
	}

	/* reads only one dependents list of a record: the "wait for my start" one or the "wait for my finish" one */
	inline void edges(uint32_t offset, bool start, uint32_t& n, uint32_t& ix) const {
		check_offset(offset);
		const char* rec = record(offset);
		switch (layout->decoder) {
			case DECODER_FIXED:
				if (start) Decoder<DECODER_FIXED>::start_edges(*layout, rec, n, ix);
				else Decoder<DECODER_FIXED>::done_edges(*layout, rec, n, ix);
				break;
			case DECODER_INTERP:
				if (start) Decoder<DECODER_INTERP>::start_edges(*layout, rec, n, ix);
				else Decoder<DECODER_INTERP>::done_edges(*layout, rec, n, ix);
				break;
			default:
				if (start) Decoder<DECODER_INTERP_GENERIC>::start_edges(*layout, rec, n, ix);
				else Decoder<DECODER_INTERP_GENERIC>::done_edges(*layout, rec, n, ix);
				break;
		}
	}

	SerializedGraph(const SerializedGraph&);
	SerializedGraph& operator=(const SerializedGraph&);

	public:

	uint32_t GetNumNodes() {
		return this->num_nodes;
	}

	/* random access for tools (goalx-convert): one node with its initial dependency count and dependents */
	DeserializedNode GetNode(uint32_t offset) {
		return get_node_by_offset(offset);
	}

	/* take CPU (nic == false) or NIC indices from a custom slot */
	void SetResourceSlot(bool nic, int slot) {
		if (nic) nic_slot = slot;
		else cpu_slot = slot;
	}

	/* the value of the requested custom field `slot` of a node */
	uint64_t GetCustom(uint32_t offset, uint16_t slot) {
		check_offset(offset);
		return layout->custom[slot].get(record(offset));
	}

	/* decodes every record of this rank, for measuring decoder throughput; the checksum compares decoders */
	uint64_t DecodeAll() const {
		// the decoder is chosen once, outside the loop
		switch (layout->decoder) {
			case DECODER_FIXED:         return decode_all<DECODER_FIXED>();
			case DECODER_INTERP:        return decode_all<DECODER_INTERP>();
			default:                    return decode_all<DECODER_INTERP_GENERIC>();
		}
	}

	void write_as_dot(char *filename) {
		/** 
			Produces a dot representation of the graph. This is usefull for debugging purposes.
		*/
		
		std::vector<DeserializedNode> allNodes;
		std::vector<DeserializedNode> executableNodes;

		executableNodes = GetExecutableNodes_DSN();
		while (executableNodes.size() > 0) {
			for (uint32_t cnt=0; cnt < executableNodes.size(); cnt++) {
				allNodes.push_back(executableNodes[cnt]);
				MarkNodeAsStarted_DSN(executableNodes[cnt]);
				MarkNodeAsDone_DSN(executableNodes[cnt]);
			}
			executableNodes.clear();
			executableNodes = GetExecutableNodes_DSN();
		}

		FILE* fd = fopen(filename, "w");
		assert(fd != NULL);
		fprintf(fd, "digraph mygraph {\n");
		fprintf(fd, "graph [rankdir=LR];\n");
		fprintf(fd, "node [shape=record];\n");
					
			for (std::vector<DeserializedNode>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
				char typestr[5];
				if ((*it).Type == OPTYPE_SEND) strcpy(typestr, "Send");
				else if ((*it).Type == OPTYPE_RECV) strcpy(typestr, "Recv");
				else if ((*it).Type == OPTYPE_CALC) strcpy(typestr, "Calc");
				else strcpy(typestr, "Unkn");
				fprintf(fd, "%i [label=\"<f0> Type: %s | <f1> Peer: %i | <f2> Size: %llu | <f3> Tag: %i | <f4> Proc: %i | <f5> Nic: %i \"]\n", 
							 (*it).offset,    typestr,        (*it).Peer, (unsigned long long) (*it).Size,  (*it).Tag  , (*it).Proc,     (*it).Nic);
			}

			for (std::vector<DeserializedNode>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
				for (std::vector<uint32_t>::iterator dit = (*it).DependOnMe.begin(); dit != (*it).DependOnMe.end(); dit++) {
					fprintf(fd, "%i:f0 -> %i:f0\n", (*it).offset, (*dit));
				}
				for (std::vector<uint32_t>::iterator dit = (*it).StartDependOnMe.begin(); dit != (*it).StartDependOnMe.end(); dit++) {
					fprintf(fd, "%i:f0 -> %i:f0 [arrowhead=diamond]\n", (*it).offset, (*dit));
				}
			}

		fprintf(fd, "} \n");
		fclose(fd);
	}

	/* block points at this rank's block (num_nodes); block_end is the end of the rank's data (before any padding) */
	SerializedGraph(const char* block, const char* block_end, const NodeLayout* node_layout, uint32_t rank, const char* filename) {

		layout = node_layout;
		my_rank = rank;
		cpu_slot = -1;
		nic_slot = -1;
		edge_bytes = layout->edge_bytes;
		entry_mask = edge_bytes >= 4 ? 0xFFFFFFFFu : ((1u << (8 * edge_bytes)) - 1);

		if (block_end < block + 2 * sizeof(uint32_t)) {
			fprintf(stderr, "Error: trace '%s': rank %u block is too short\n", filename, rank);
			exit(EXIT_FAILURE);
		}
		memcpy(&num_nodes, block, sizeof(uint32_t));
		memcpy(&num_root_nodes, block + sizeof(uint32_t), sizeof(uint32_t));
		roots = block + sizeof(uint32_t)*2;
		records = roots + (size_t) edge_bytes*num_root_nodes;
		appendix = records + (size_t) layout->record_bytes*num_nodes;
		if (appendix > block_end) {
			fprintf(stderr, "Error: trace '%s': rank %u claims %u nodes, but the trace ends before them\n", filename, rank, num_nodes);
			exit(EXIT_FAILURE);
		}
		num_entries = (uint64_t) (block_end - appendix) / edge_bytes;

		remaining = num_nodes > 0 ? (uint32_t*) calloc(num_nodes, sizeof(uint32_t)) : NULL;
		if (num_nodes > 0 && remaining == NULL) {
			fprintf(stderr, "Error: out of memory for the dependency counters of rank %u\n", rank);
			exit(EXIT_FAILURE);
		}

		executableNodes.clear();
		add_root_nodes();
	}

	SerializedGraph(SerializedGraph&& o) :
		layout(o.layout), roots(o.roots), records(o.records), appendix(o.appendix), num_entries(o.num_entries),
		entry_mask(o.entry_mask), edge_bytes(o.edge_bytes), num_root_nodes(o.num_root_nodes), num_nodes(o.num_nodes),
		my_rank(o.my_rank), cpu_slot(o.cpu_slot), nic_slot(o.nic_slot), remaining(o.remaining) {
		executableNodes.swap(o.executableNodes);
		o.remaining = NULL;
	}

	~SerializedGraph() {
		free(remaining);
	}

	/* the _DSN variants (used by write_as_dot) still build full DeserializedNodes with their lists */
	void MarkNodeAsStarted_DSN(DeserializedNode node) {
		MarkNodeAsStarted(node.offset);
	}

	void MarkNodeAsDone_DSN(DeserializedNode node) {
		MarkNodeAsDone(node.offset);
	}

	std::vector<DeserializedNode> GetExecutableNodes_DSN() {

		std::vector<DeserializedNode> ret;

		for (uint32_t cnt=0; cnt<executableNodes.size(); cnt++) {
			ret.push_back(get_node_by_offset(executableNodes[cnt]));
		}
		executableNodes.clear();
		return ret;
	}

  typedef std::vector<graph_node_properties> nodelist_t;
	/* hands out the executable nodes; each is decoded here, once, straight into the simulator's struct */
	void GetExecutableNodes(nodelist_t *ret_ptr) {
		nodelist_t& ret = *ret_ptr;
		DeserializedNode N; // payload only: its lists stay empty
		uint32_t num_deps, deps_ix, num_startdeps, startdeps_ix;

		for (uint32_t cnt=0; cnt<executableNodes.size(); cnt++) {
			uint32_t offset = executableNodes[cnt];
			const char* rec = record(offset);
			decode_record(rec, N, num_deps, deps_ix, num_startdeps, startdeps_ix);
			graph_node_properties gp;
			gp.target = N.Peer;
			gp.size = N.Size;
			gp.tag = N.Tag;
			gp.proc = cpu_slot < 0 ? N.Proc : (uint8_t) layout->custom[cpu_slot].get(rec);
			gp.nic = nic_slot < 0 ? N.Nic : (uint8_t) layout->custom[nic_slot].get(rec);
			if (N.Type == OPTYPE_SEND) gp.type = OP_SEND;
			else if (N.Type == OPTYPE_RECV) gp.type = OP_RECV;
			else if (N.Type == OPTYPE_CALC) gp.type = OP_LOCOP;
			gp.offset = offset;
			ret.push_back(gp);
		}
		executableNodes.clear();
	}

	/* only the record's "wait for my start" list is read, walked in place in the appendix */
	void MarkNodeAsStarted(uint32_t offset) {
		uint32_t n, ix;
		edges(offset, true, n, ix);
		release(n, ix);
	}

	/* only the record's "wait for my finish" list is read */
	void MarkNodeAsDone(uint32_t offset) {
		uint32_t n, ix;
		edges(offset, false, n, ix);
		release(n, ix);
	}

};

class Parser {

	private:

	char* mapping_start; // argh. this should be void* but c++ is anal on void*-arithmetic
	size_t mapping_length;
	uint32_t num_ranks_in_schedule;
	uint32_t max_cpu;
	uint32_t max_nic;
	FILE *schedules_fd;
	Schema schema;
	NodeLayout layout;
	std::vector<std::string> custom_slots; // requested custom fields, slot = index
	std::string trace_name;

	uint64_t get_file_size(FILE* fd) {
		
		struct stat f_info;
		int r = fstat(fileno(fd), &f_info);
		assert(r == 0);
		return f_info.st_size;
	}

	static void fail(const char* filename, const std::string& msg) {
		fprintf(stderr, "Error: trace '%s': %s\n", filename, msg.c_str());
		exit(EXIT_FAILURE);
	}

	/* the legacy jump table: offsets relative to the byte after the magic cookie */
	void legacy_block(const char* filename, uint32_t rank, const char** start, const char** end) {
		const char* base = mapping_start + sizeof(uint64_t);
		uint64_t jump[2];
		memcpy(jump, base + sizeof(uint32_t) + sizeof(uint8_t)*2 + sizeof(uint64_t)*2*rank, sizeof(jump));
		uint64_t avail = mapping_length - sizeof(uint64_t);
		if (jump[0] > jump[1] || jump[1] > avail) {
			fail(filename, "jump table entry of rank " + std::to_string(rank) + " is out of range");
		}
		*start = base + jump[0];
		*end = base + jump[1];
	}

	void goalx_block(const char* filename, uint32_t rank, uint32_t header_len, const char** start, const char** end) {
		uint64_t jump[2];
		memcpy(jump, mapping_start + header_len + sizeof(uint64_t)*2*rank, sizeof(jump));
		if (jump[0] > jump[1] || jump[1] > mapping_length || jump[1] - jump[0] < sizeof(uint32_t)*2 + GOALX_PADDING) {
			fail(filename, "jump table entry of rank " + std::to_string(rank) + " is out of range");
		}
		*start = mapping_start + jump[0];
		*end = mapping_start + jump[1] - GOALX_PADDING; // the padding holds no entries
	}

	public:
	
  typedef std::vector<SerializedGraph> schedules_t ;
	schedules_t schedules;

	/*
	 * reqs: extra fields the simulation needs (custom ones become slots for
	 * GetCustom, in this order); decoder_name: auto (fixed for legacy traces,
	 * interp for GOALX), interp, interp-generic or fixed; all_custom:
	 * every custom field of the trace gets a slot (tools that copy a trace).
	 */
	Parser(char* filename, bool save_mem, const std::vector<Requirement>& reqs, const char* decoder_name = "auto",
	       bool all_custom = false) {
		
		trace_name = filename;
		schedules_fd = fopen(filename, "r");
		if (schedules_fd == NULL) {
			fprintf(stderr, "Couldn't open input file %s!\n", filename);
			exit(EXIT_FAILURE);
		}

		uint64_t magic_cookie = 0;
		mapping_length = get_file_size(schedules_fd);
		if (fread(&magic_cookie, sizeof(uint64_t), 1, schedules_fd) != 1) magic_cookie = 0;
	
		if (magic_cookie == MAGIC_COOKIE_INVALID) {
			fprintf(stderr, "This is serialized goal schedule was invalidated by a prior simulation run!\n");
			exit(EXIT_FAILURE);
		}
		if (magic_cookie != GOALX_MAGIC && magic_cookie != MAGIC_COOKIE) {
			fprintf(stderr, "This is not a serialized goal schedule - the magic cookie is missing\n");
			exit(EXIT_FAILURE);
		}
		if (save_mem) printf("--save-mem has no effect: traces are mapped read-only\n");

		// both formats are mapped read-only: the simulator never writes the trace
		mapping_start = (char*) mmap(NULL, mapping_length, PROT_READ, MAP_PRIVATE, fileno(schedules_fd), 0);
		if (mapping_start == MAP_FAILED) {
			perror("couldn't mmap the trace");
			exit(EXIT_FAILURE);
		}

		std::string err;
		uint32_t header_len = 0;
		if (magic_cookie == GOALX_MAGIC) {
			if (!Schema::parse_header(mapping_start, mapping_length, &schema, &num_ranks_in_schedule,
			                          &max_cpu, &max_nic, &header_len, &err)) {
				fail(filename, err);
			}
			if (header_len + sizeof(uint64_t)*2*(uint64_t) num_ranks_in_schedule > mapping_length) {
				fail(filename, "jump table extends past the end of the file");
			}
		} else {
			const size_t fixed = sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint8_t)*2;
			if (mapping_length < fixed) fail(filename, "file is too short for a legacy header");
			uint8_t c8, n8;
			memcpy(&num_ranks_in_schedule, mapping_start + sizeof(uint64_t), sizeof(uint32_t));
			memcpy(&c8, mapping_start + sizeof(uint64_t) + sizeof(uint32_t), 1);
			memcpy(&n8, mapping_start + sizeof(uint64_t) + sizeof(uint32_t) + 1, 1);
			max_cpu = c8;
			max_nic = n8;
			if (fixed + sizeof(uint64_t)*2*(uint64_t) num_ranks_in_schedule > mapping_length) {
				fail(filename, "jump table extends past the end of the file");
			}
			schema = Schema::legacy_layout();
		}

		if (!validate_schema(schema, reqs, num_ranks_in_schedule, max_cpu, max_nic, &err)) fail(filename, err);
		for (size_t i = 0; i < reqs.size(); i++) {
			const FieldDesc* f = schema.find(reqs[i].name);
			if (f != NULL && f->kind == FK_CUSTOM &&
			    std::find(custom_slots.begin(), custom_slots.end(), reqs[i].name) == custom_slots.end()) {
				custom_slots.push_back(reqs[i].name);
			}
		}
		if (all_custom) {
			for (size_t i = 0; i < schema.fields.size(); i++) {
				const FieldDesc& f = schema.fields[i];
				if (f.kind == FK_CUSTOM && std::find(custom_slots.begin(), custom_slots.end(), f.name) == custom_slots.end()) {
					custom_slots.push_back(f.name);
				}
			}
		}

		DecoderKind decoder = schema.legacy ? DECODER_FIXED : DECODER_INTERP;
		if (decoder_name != NULL && strcmp(decoder_name, "interp") == 0) decoder = DECODER_INTERP;
		if (decoder_name != NULL && strcmp(decoder_name, "interp-generic") == 0) decoder = DECODER_INTERP_GENERIC;
		if (decoder_name != NULL && strcmp(decoder_name, "fixed") == 0) {
			if (!schema.legacy) fail(filename, "the fixed decoder only reads legacy (4223) traces");
			decoder = DECODER_FIXED;
		}

		layout = NodeLayout(schema, decoder, custom_slots);
		// the interpreter's fast path needs simple fields; otherwise fall back to the generic one
		if (decoder == DECODER_INTERP && !layout.simple_ok()) layout.decoder = DECODER_INTERP_GENERIC;
		schedules.reserve(num_ranks_in_schedule);
		for (uint32_t cnt=0; cnt<num_ranks_in_schedule; cnt++) {
			const char *start, *end;
			if (schema.legacy) legacy_block(filename, cnt, &start, &end);
			else goalx_block(filename, cnt, header_len, &start, &end);
			schedules.emplace_back(start, end, &layout, cnt, filename);
		}
	}


	int GetNumCPU() {
		return max_cpu+1;
	}

	int GetNumNIC() {
		return max_nic+1;
	}

	/* slot of a requested custom field for SerializedGraph::GetCustom, -1 if it was not requested */
	int GetCustomSlot(const char* name) {
		for (size_t i = 0; i < custom_slots.size(); i++) if (custom_slots[i] == name) return (int) i;
		return -1;
	}

	/* the largest value of a field in the trace (from the header) */
	uint64_t GetFieldMax(const char* name) {
		const FieldDesc* f = schema.find(name);
		return f ? f->max_value : 0;
	}

	/*
	 * Takes every op's CPU (nic == false) or NIC index from a custom field.
	 * The field was required at load; its largest value sizes the simulator's
	 * per-CPU / per-NIC state and must stay within the 8-bit resource ids.
	 */
	void UseCustomAsResource(bool nic, const char* field, const char* needed_by) {
		const FieldDesc* f = schema.find(field);
		int slot = GetCustomSlot(field);
		if (f == NULL || f->kind != FK_CUSTOM || slot < 0) {
			fail(trace_name.c_str(), std::string("'") + field + "' is not a custom field of this trace (needed by " + needed_by + ")");
		}
		if (f->max_value > 255) {
			fail(trace_name.c_str(), "field '" + std::string(field) + "' has values up to " + std::to_string(f->max_value) +
			     ", this simulator supports at most 255 (needed by " + needed_by + ")");
		}
		if (nic) max_nic = (uint32_t) f->max_value;
		else max_cpu = (uint32_t) f->max_value;
		for (size_t r = 0; r < schedules.size(); r++) schedules[r].SetResourceSlot(nic, slot);
	}

	const char* GetDecoderName() {
		switch (layout.decoder) {
			case DECODER_FIXED:         return "fixed";
			case DECODER_INTERP:        return "interp";
			default:                    return "interp-generic";
		}
	}

	const Schema& GetSchema() { return schema; }
	uint32_t GetNumRanks() { return num_ranks_in_schedule; }
	uint32_t GetMaxCPU() { return max_cpu; }
	uint32_t GetMaxNIC() { return max_nic; }
	const std::vector<std::string>& GetCustomSlots() { return custom_slots; }

	void PrintSchema(FILE* fd) {
		schema.print(fd, num_ranks_in_schedule, max_cpu, max_nic);
	}

	~Parser() {
		schedules.clear(); // the ranks refer to the layout and the mapping
		int r = munmap(mapping_start, mapping_length);
		assert(r == 0);
		fclose(schedules_fd);
	}
	
};
