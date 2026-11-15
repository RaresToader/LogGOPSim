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
#include <chrono>

#include "LogGOPSim.hpp"
#include "TraceSchema.hpp"
#include "GoalxDecoders.hpp"

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

/*
 * One rank of a mapped trace. The trace is read-only: dependency counters live
 * in a lazily zeroed side array, and every record access goes through the
 * decoder's function table (goalx_decoder.h).
 */
class SerializedGraph {

	private:

	const goalx_decoder_t* dec;
	void* rank_state;
	const uint8_t* roots;
	const uint8_t* records;
	const uint8_t* appendix;
	uint64_t num_entries;     // appendix entries inside the rank block
	uint32_t record_bytes;
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

	inline uint32_t entry(const uint8_t* base, uint64_t i) const {
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

	inline const uint8_t* record(uint32_t offset) const {
		return records + (size_t) record_bytes * offset;
	}

	void add_root_nodes() {
		for (uint32_t cnt=0; cnt<num_root_nodes; cnt++) {
			uint32_t offset = entry(roots, cnt);
			check_offset(offset);
			executableNodes.push_back(offset);
		}
	}

	/* walks n dependents starting at appendix index ix; nodes whose last dependency this was become executable */
	inline void release(uint32_t n, uint32_t ix) {
		if (n == 0) return; // old legacy files hold sentinels in the index of empty lists
		if ((uint64_t) ix + n > num_entries) {
			fprintf(stderr, "[rank %u] dependents list %u+%u runs past the appendix (%llu entries)\n",
			        my_rank, ix, n, (unsigned long long) num_entries);
			exit(EXIT_FAILURE);
		}
		for (uint32_t cnt=0; cnt<n; cnt++) {
			uint32_t offset = entry(appendix, (uint64_t) ix + cnt);
			check_offset(offset);
			uint32_t r = remaining[offset];
			if (r == 0) r = dec->depcnt(dec, rank_state, record(offset), offset) + 1;
			remaining[offset] = --r;
			if (r == 1) executableNodes.push_back(offset);
		}
	}

	DeserializedNode get_node_by_offset(uint32_t offset) {
		check_offset(offset);
		const uint8_t* rec = record(offset);
		DeserializedNode N;
		goalx_op_t op;
		uint32_t n, ix;
		dec->decode_op(dec, rank_state, rec, offset, &op);
		N.DependenciesCnt = dec->depcnt(dec, rank_state, rec, offset);
		N.Type = (char) op.type;
		N.Peer = op.peer;
		N.Size = op.size;
		N.Tag = op.tag;
		N.Proc = op.cpu;
		N.Nic = op.nic;
		N.offset = offset;
		dec->done_edges(dec, rank_state, rec, offset, &n, &ix);
		for (uint32_t c = 0; c < n; c++) N.DependOnMe.push_back(entry(appendix, (uint64_t) ix + c));
		dec->start_edges(dec, rank_state, rec, offset, &n, &ix);
		for (uint32_t c = 0; c < n; c++) N.StartDependOnMe.push_back(entry(appendix, (uint64_t) ix + c));
		return N;
	}

	SerializedGraph(const SerializedGraph&);
	SerializedGraph& operator=(const SerializedGraph&);

	public:

	/*
	 * block points at this rank's block (num_nodes); block_end is the end of
	 * the rank's data (before any padding); view is filled in for the decoder.
	 */
	SerializedGraph(const uint8_t* block, const uint8_t* block_end, const Schema& schema, uint32_t rank,
	                goalx_rank_view_t* view, const char* filename) {

		my_rank = rank;
		dec = NULL;
		rank_state = NULL;
		cpu_slot = -1;
		nic_slot = -1;
		record_bytes = schema.record_bytes;
		edge_bytes = schema.edge_bytes;
		entry_mask = edge_bytes >= 4 ? 0xFFFFFFFFu : ((1u << (8 * edge_bytes)) - 1);

		if (block_end < block + 2 * sizeof(uint32_t)) {
			fprintf(stderr, "Error: trace '%s': rank %u block is too short\n", filename, rank);
			exit(EXIT_FAILURE);
		}
		memcpy(&num_nodes, block, sizeof(uint32_t));
		memcpy(&num_root_nodes, block + sizeof(uint32_t), sizeof(uint32_t));
		roots = block + sizeof(uint32_t)*2;
		records = roots + (size_t) edge_bytes*num_root_nodes;
		appendix = records + (size_t) record_bytes*num_nodes;
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

		view->rank = rank;
		view->num_nodes = num_nodes;
		view->records = records;
		view->appendix = appendix;
	}

	SerializedGraph(SerializedGraph&& o) :
		dec(o.dec), rank_state(o.rank_state), roots(o.roots), records(o.records), appendix(o.appendix),
		num_entries(o.num_entries), record_bytes(o.record_bytes), entry_mask(o.entry_mask), edge_bytes(o.edge_bytes),
		num_root_nodes(o.num_root_nodes), num_nodes(o.num_nodes), my_rank(o.my_rank),
		cpu_slot(o.cpu_slot), nic_slot(o.nic_slot), remaining(o.remaining) {
		executableNodes.swap(o.executableNodes);
		o.remaining = NULL;
	}

	~SerializedGraph() {
		free(remaining);
	}

	/* called once the decoder exists; reads the roots */
	void Bind(const goalx_decoder_t* d, void* rs) {
		dec = d;
		rank_state = rs;
		executableNodes.clear();
		add_root_nodes();
	}

	uint32_t GetNumNodes() {
		return this->num_nodes;
	}

	/* random access for tools (goalx-convert): one node's payload, initial dependency count and dependents */
	void DecodeOp(uint32_t offset, goalx_op_t* op) {
		check_offset(offset);
		dec->decode_op(dec, rank_state, record(offset), offset, op);
	}

	uint32_t DepCnt(uint32_t offset) {
		check_offset(offset);
		return dec->depcnt(dec, rank_state, record(offset), offset);
	}

	void Dependents(uint32_t offset, bool start, std::vector<uint32_t>& out) {
		uint32_t n, ix;
		check_offset(offset);
		if (start) dec->start_edges(dec, rank_state, record(offset), offset, &n, &ix);
		else dec->done_edges(dec, rank_state, record(offset), offset, &n, &ix);
		out.clear();
		if (n == 0) return;
		if ((uint64_t) ix + n > num_entries) {
			fprintf(stderr, "[rank %u] dependents list %u+%u runs past the appendix\n", my_rank, ix, n);
			exit(EXIT_FAILURE);
		}
		for (uint32_t c = 0; c < n; c++) {
			uint32_t j = entry(appendix, (uint64_t) ix + c);
			check_offset(j);
			out.push_back(j);
		}
	}

	/* take CPU (nic == false) or NIC indices from a custom slot */
	void SetResourceSlot(bool nic, int slot) {
		if (nic) nic_slot = slot;
		else cpu_slot = slot;
	}

	/* the value of the requested custom field `slot` of a node */
	uint64_t GetCustom(uint32_t offset, uint16_t slot) {
		return dec->get_custom(dec, rank_state, record(offset), offset, slot);
	}

	/* decodes every record of this rank through the decoder table, for measuring decoder throughput */
	uint64_t DecodeAll() const {
		uint64_t sum = 0;
		goalx_op_t op;
		uint32_t n, ix;
		for (uint32_t i = 0; i < num_nodes; i++) {
			const uint8_t* rec = record(i);
			dec->decode_op(dec, rank_state, rec, i, &op);
			sum += op.type + op.peer + op.size + op.tag + op.cpu + op.nic;
			sum += dec->depcnt(dec, rank_state, rec, i);
			dec->done_edges(dec, rank_state, rec, i, &n, &ix);
			sum += n + ix;
			dec->start_edges(dec, rank_state, rec, i, &n, &ix);
			sum += n + ix;
		}
		return sum;
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

	/* the _DSN variants (used by write_as_dot) build full DeserializedNodes with their lists */
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
		goalx_op_t op;

		for (uint32_t cnt=0; cnt<executableNodes.size(); cnt++) {
			uint32_t offset = executableNodes[cnt];
			dec->decode_op(dec, rank_state, record(offset), offset, &op);
			graph_node_properties gp;
			gp.target = op.peer;
			gp.size = op.size;
			gp.tag = op.tag;
			gp.proc = cpu_slot < 0 ? op.cpu : (uint8_t) dec->get_custom(dec, rank_state, record(offset), offset, (uint16_t) cpu_slot);
			gp.nic = nic_slot < 0 ? op.nic : (uint8_t) dec->get_custom(dec, rank_state, record(offset), offset, (uint16_t) nic_slot);
			if (op.type == OPTYPE_SEND) gp.type = OP_SEND;
			else if (op.type == OPTYPE_RECV) gp.type = OP_RECV;
			else if (op.type == OPTYPE_CALC) gp.type = OP_LOCOP;
			gp.offset = offset;
			ret.push_back(gp);
		}
		executableNodes.clear();
	}

	/* only the record's "wait for my start" list is read, walked in place in the appendix */
	void MarkNodeAsStarted(uint32_t offset) {
		uint32_t n, ix;
		check_offset(offset);
		dec->start_edges(dec, rank_state, record(offset), offset, &n, &ix);
		release(n, ix);
	}

	/* only the record's "wait for my finish" list is read */
	void MarkNodeAsDone(uint32_t offset) {
		uint32_t n, ix;
		check_offset(offset);
		dec->done_edges(dec, rank_state, record(offset), offset, &n, &ix);
		release(n, ix);
	}

};

class Parser {

	private:

	char* mapping_start;
	size_t mapping_length;
	uint32_t num_ranks_in_schedule;
	uint32_t max_cpu;
	uint32_t max_nic;
	FILE *schedules_fd;
	Schema schema;
	GoalxSchemaHolder holder;
	std::vector<goalx_rank_view_t> views;
	goalx_trace_view_t trace_view;
	goalx_decoder_t* decoder;
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
	void legacy_block(const char* filename, uint32_t rank, const uint8_t** start, const uint8_t** end) {
		const uint8_t* base = (const uint8_t*) mapping_start + sizeof(uint64_t);
		uint64_t jump[2];
		memcpy(jump, base + sizeof(uint32_t) + sizeof(uint8_t)*2 + sizeof(uint64_t)*2*rank, sizeof(jump));
		uint64_t avail = mapping_length - sizeof(uint64_t);
		if (jump[0] > jump[1] || jump[1] > avail) {
			fail(filename, "jump table entry of rank " + std::to_string(rank) + " is out of range");
		}
		*start = base + jump[0];
		*end = base + jump[1];
	}

	void goalx_block(const char* filename, uint32_t rank, uint32_t header_len, const uint8_t** start, const uint8_t** end) {
		uint64_t jump[2];
		memcpy(jump, mapping_start + header_len + sizeof(uint64_t)*2*rank, sizeof(jump));
		if (jump[0] > jump[1] || jump[1] > mapping_length || jump[1] - jump[0] < sizeof(uint32_t)*2 + GOALX_PADDING) {
			fail(filename, "jump table entry of rank " + std::to_string(rank) + " is out of range");
		}
		*start = (const uint8_t*) mapping_start + jump[0];
		*end = (const uint8_t*) mapping_start + jump[1] - GOALX_PADDING; // the padding holds no entries
	}

	public:
	
	typedef std::vector<SerializedGraph> schedules_t;
	schedules_t schedules;

	/*
	 * reqs: extra fields the simulation needs (custom ones become slots for
	 * GetCustom, in this order); decoder_name: auto (fixed for legacy traces,
	 * interp for GOALX), a built-in decoder or plugin:<path>.
	 */
	/* seconds spent in each load phase (TIMING line, --emit-results) */
	struct Timing {
		double header_s, validate_s, decoder_init_s, counters_s;
	} timing;

	/*
	 * all_custom: every custom field of the trace gets a slot (tools that copy a trace);
	 * record_calls: wrap the decoder and write every hot call to this file (benchmarks).
	 */
	Parser(char* filename, bool save_mem, const std::vector<Requirement>& reqs, const char* decoder_name = "auto",
	       bool all_custom = false, const char* record_calls = NULL) {

		typedef std::chrono::steady_clock clk;
		clk::time_point t0 = clk::now();
		
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
		clk::time_point t1 = clk::now();

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
		if (!holder.set(schema, custom_slots, &err)) fail(filename, err);
		clk::time_point t2 = clk::now();

		views.resize(num_ranks_in_schedule);
		schedules.reserve(num_ranks_in_schedule);
		for (uint32_t cnt=0; cnt<num_ranks_in_schedule; cnt++) {
			const uint8_t *start, *end;
			if (schema.legacy) legacy_block(filename, cnt, &start, &end);
			else goalx_block(filename, cnt, header_len, &start, &end);
			schedules.emplace_back(start, end, schema, cnt, &views[cnt], filename);
		}
		trace_view.num_ranks = num_ranks_in_schedule;
		trace_view.ranks = views.empty() ? NULL : &views[0];
		clk::time_point t3 = clk::now();

		std::string dname = decoder_name ? decoder_name : "auto";
		if (dname == "auto") dname = schema.legacy ? "fixed" : "interp";
		decoder = goalx_create_decoder(dname.c_str(), &holder.schema, &trace_view, &err);
		if (decoder == NULL) fail(filename, err);
		if (record_calls != NULL) {
			for (uint32_t cnt=0; cnt<num_ranks_in_schedule; cnt++) {
				if (views[cnt].num_nodes >= (1u << 30)) fail(filename, "--record-calls needs fewer than 2^30 nodes per rank");
			}
			decoder = goalx_wrap_recording(decoder, record_calls);
		}
		rank_states.resize(num_ranks_in_schedule);
		for (uint32_t cnt=0; cnt<num_ranks_in_schedule; cnt++) {
			rank_states[cnt] = decoder->bind_rank ? decoder->bind_rank(decoder, &views[cnt]) : NULL;
			schedules[cnt].Bind(decoder, rank_states[cnt]);
		}
		clk::time_point t4 = clk::now();
		timing.header_s = std::chrono::duration<double>(t1 - t0).count();
		timing.validate_s = std::chrono::duration<double>(t2 - t1).count();
		timing.counters_s = std::chrono::duration<double>(t3 - t2).count();
		timing.decoder_init_s = std::chrono::duration<double>(t4 - t3).count();
	}

	/* ---- decode-only benchmarks: replay decoder calls without simulating ---- */

	/* every node of every rank in file order, four calls each */
	std::vector<GoalxCall> SequentialCalls() {
		std::vector<GoalxCall> calls;
		for (uint32_t r = 0; r < num_ranks_in_schedule; r++) {
			for (uint32_t i = 0; i < views[r].num_nodes; i++) {
				for (uint32_t k = 0; k < 4; k++) calls.push_back(goalx_call(r, i, k));
			}
		}
		return calls;
	}

	/* false (with a message) if a recorded call does not fit this trace */
	bool CheckCalls(const std::vector<GoalxCall>& calls, std::string* err) {
		for (size_t i = 0; i < calls.size(); i++) {
			uint32_t r = calls[i].rank, idx = calls[i].idx_and_kind & 0x3FFFFFFFu;
			if (r >= num_ranks_in_schedule || idx >= views[r].num_nodes) {
				*err = "recorded call " + std::to_string(i) + " (rank " + std::to_string(r) + ", node " + std::to_string(idx) +
				       ") does not fit this trace";
				return false;
			}
		}
		return true;
	}

	/* replays calls through the decoder's table (as the simulator calls it); returns a checksum */
	uint64_t Replay(const std::vector<GoalxCall>& calls) {
		struct Table {
			const goalx_decoder_t* d;
			void op(void* rs, const uint8_t* rec, uint32_t i, goalx_op_t* o) const { d->decode_op(d, rs, rec, i, o); }
			uint32_t cnt(void* rs, const uint8_t* rec, uint32_t i) const { return d->depcnt(d, rs, rec, i); }
			void done(void* rs, const uint8_t* rec, uint32_t i, uint32_t* n, uint32_t* x) const { d->done_edges(d, rs, rec, i, n, x); }
			void start(void* rs, const uint8_t* rec, uint32_t i, uint32_t* n, uint32_t* x) const { d->start_edges(d, rs, rec, i, n, x); }
		} t = { decoder };
		return replay(calls, t);
	}

	/* the same with the built-in interp or fixed functions called directly, so the compiler can inline them */
	bool CanReplayInline() {
		return strcmp(decoder->name, "interp") == 0 || strcmp(decoder->name, "fixed") == 0;
	}

	uint64_t ReplayInline(const std::vector<GoalxCall>& calls) {
		struct Interp {
			const goalx_decoder_t* d;
			void op(void* rs, const uint8_t* rec, uint32_t i, goalx_op_t* o) const { goalx_interp_decode_op(d, rs, rec, i, o); }
			uint32_t cnt(void* rs, const uint8_t* rec, uint32_t i) const { return goalx_interp_depcnt(d, rs, rec, i); }
			void done(void* rs, const uint8_t* rec, uint32_t i, uint32_t* n, uint32_t* x) const { goalx_interp_done(d, rs, rec, i, n, x); }
			void start(void* rs, const uint8_t* rec, uint32_t i, uint32_t* n, uint32_t* x) const { goalx_interp_start(d, rs, rec, i, n, x); }
		};
		struct Fixed {
			const goalx_decoder_t* d;
			void op(void* rs, const uint8_t* rec, uint32_t i, goalx_op_t* o) const { goalx_fixed_decode_op(d, rs, rec, i, o); }
			uint32_t cnt(void* rs, const uint8_t* rec, uint32_t i) const { return goalx_fixed_depcnt(d, rs, rec, i); }
			void done(void* rs, const uint8_t* rec, uint32_t i, uint32_t* n, uint32_t* x) const { goalx_fixed_done(d, rs, rec, i, n, x); }
			void start(void* rs, const uint8_t* rec, uint32_t i, uint32_t* n, uint32_t* x) const { goalx_fixed_start(d, rs, rec, i, n, x); }
		};
		if (strcmp(decoder->name, "fixed") == 0) {
			Fixed f = { decoder };
			return replay(calls, f);
		}
		Interp in = { decoder };
		return replay(calls, in);
	}

	private:

	std::vector<void*> rank_states;

	template <typename D>
	uint64_t replay(const std::vector<GoalxCall>& calls, const D& dec) {
		uint64_t sum = 0;
		const size_t rb = schema.record_bytes;
		goalx_op_t op;
		uint32_t n, ix;
		for (size_t c = 0; c < calls.size(); c++) {
			uint32_t r = calls[c].rank;
			uint32_t idx = calls[c].idx_and_kind & 0x3FFFFFFFu;
			const uint8_t* rec = views[r].records + rb * idx;
			void* rs = rank_states[r];
			switch (calls[c].idx_and_kind >> 30) {
				case GOALX_CALL_DECODE_OP:
					dec.op(rs, rec, idx, &op);
					sum += op.type + op.peer + op.size + op.tag + op.cpu + op.nic;
					break;
				case GOALX_CALL_DEPCNT:
					sum += dec.cnt(rs, rec, idx);
					break;
				case GOALX_CALL_DONE_EDGES:
					dec.done(rs, rec, idx, &n, &ix);
					sum += n + ix;
					break;
				default:
					dec.start(rs, rec, idx, &n, &ix);
					sum += n + ix;
					break;
			}
		}
		return sum;
	}

	public:

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
		return decoder->name;
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
		schedules.clear(); // the ranks refer to the decoder and the mapping
		decoder->destroy(decoder);
		int r = munmap(mapping_start, mapping_length);
		assert(r == 0);
		fclose(schedules_fd);
	}
	
};
