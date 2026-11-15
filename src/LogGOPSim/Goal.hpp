#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <algorithm>

#include "Parser.hpp"

typedef Node* goalop_t;

enum GoalMode { GOAL_WRITE_LEGACY, GOAL_WRITE_GOALX, GOAL_COLLECT_STATS };

/* GOAL keywords and builtin field names cannot name a custom attribute */
static inline bool reserved_attribute_name(const std::string& n) {
	static const char* const kw[] = { "send", "recv", "calc", "requires", "irequires", "rank", "num_ranks",
	                                   "to", "from", "tag", "cpu", "nic", "b" };
	for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) if (n == kw[i]) return true;
	return Schema::builtin_kind(n) != FK_CUSTOM;
}

class Goal {

	private:
		Graph graph;
		uint32_t rank;
		uint32_t num_ranks;
		GoalMode mode;
		const Schema* schema;      // GOAL_WRITE_GOALX: the layout to write
		FieldStats* stats;         // filled in GOAL_COLLECT_STATS, read in GOAL_WRITE_GOALX
		CustomNames* names;        // custom attribute names, shared by all ranks

		uint32_t MaxCPU(uint32_t cpu = 0) {
			static uint32_t max_cpu = 0;
			if (cpu > max_cpu) max_cpu = cpu;
			return max_cpu;
		}
	
		uint32_t MaxNIC(uint32_t nic = 0) {
			static uint32_t max_nic = 0;
			if (nic > max_nic) max_nic = nic;
			return max_nic;
		}
			
	public:

		Goal() : rank(0), num_ranks(0), mode(GOAL_WRITE_LEGACY), schema(NULL), stats(NULL), names(NULL) {}

		/* adds a custom attribute to op; exits with an error naming the line if it is not allowed */
		void Custom(goalop_t op, const std::string& name, uint64_t value, uint32_t line) {
			if (reserved_attribute_name(name)) {
				fprintf(stderr, "Error in line %u: '%s' is reserved and cannot be a custom attribute\n", line, name.c_str());
				exit(EXIT_FAILURE);
			}
			if (mode == GOAL_WRITE_LEGACY || names == NULL) {
				fprintf(stderr, "Error in line %u: custom attribute '%s' requires --format goalx\n", line, name.c_str());
				exit(EXIT_FAILURE);
			}
			uint16_t slot = names->get(name);
			for (size_t i = 0; i < op->Custom.size(); i++) {
				if (op->Custom[i].first == slot) {
					fprintf(stderr, "Error in line %u: attribute '%s' is given twice\n", line, name.c_str());
					exit(EXIT_FAILURE);
				}
			}
			op->Custom.push_back(std::make_pair(slot, value));
		}
	
		goalop_t Send(uint32_t src, uint64_t dest, uint64_t size, uint64_t tag, uint32_t cpu, uint32_t nic) {
			
			Node* n = graph.addNode();
			
			n->Type = OPTYPE_SEND; 
			n->Peer = dest;
			n->Tag = tag;
			n->Proc = cpu;
			n->Nic = nic;
			n->Size = size;

			MaxCPU(cpu);
			MaxNIC(nic);
			
			return n;
		}

		goalop_t Recv(uint64_t src, uint32_t dest, uint64_t size, uint64_t tag, uint32_t cpu, uint32_t nic) {
		
			Node* n = graph.addNode();
			
			n->Type = OPTYPE_RECV; 
			n->Peer = src;
			n->Tag = tag;
			n->Proc = cpu;
			n->Nic = nic;
			n->Size = size;
		
			MaxCPU(cpu);
			MaxNIC(nic);	
			
			return n;
		}

		goalop_t Calc(uint32_t src, uint64_t size, uint32_t cpu, uint32_t nic) {
		
			Node* n = graph.addNode();
			
			n->Type = OPTYPE_CALC; 
			n->Peer = 0; // this optype has not real peer, i just set it so it is clearly defined
			n->Tag = 0; // this optype has not real tag, i just set it so it is clearly defined
			n->Proc = cpu;
			n->Nic = nic;
			n->Size = size;
			
			MaxCPU(cpu);
			MaxNIC(nic);

			return n;
		}

		void StartDependency(goalop_t src, goalop_t dest) {
			// a can not be executed before b is started	
			graph.addStartDependency(src, dest);
		}

		void Dependency(goalop_t src, goalop_t dest) {
			//a can not be executed before b is finished
			graph.addDependency(src, dest);
		}

		void SerializeSchedule(char* filename) {
			
			static int fd;

			if (mode == GOAL_COLLECT_STATS) {
				graph.collect_stats(*stats, *names, num_ranks);
				return;
			}
			
			// create/open binary schedule if it is the first rank (rank 0)
			if (rank==0) {
				fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, S_IWUSR | S_IRUSR);
				if (fd == -1) {
					fprintf(stderr, "Couldn't open %s for schedule serialization!\n", filename);		
					perror("system error message:");
					exit(EXIT_FAILURE);
				}
			}

			if (mode == GOAL_WRITE_GOALX) {
				graph.serialize_goalx(fd, rank, num_ranks, stats->max_cpu, stats->max_nic, *schema, *names);
			} else {
				// values above 255 are rejected per node by serialize_mmap
				graph.serialize_mmap(fd, rank, num_ranks, (uint8_t) std::min(MaxCPU(), 255u), (uint8_t) std::min(MaxNIC(), 255u));
			}
			
			// close the binary schedule if it is the last rank
			if (rank == num_ranks-1) {
				close(fd);
				sync();
			}
			
		}

		void SetRank(uint32_t r) {
			rank=r;
		}

		void SetNumRanks(uint32_t nr) {
			num_ranks = nr;
		}

		void SetMode(GoalMode m, const Schema* s, FieldStats* st, CustomNames* n) {
			mode = m;
			schema = s;
			stats = st;
			names = n;
		}

};
