/*
 * goalx-convert: rewrites a legacy (4223) or GOALX 1.1 trace as GOALX 1.1.
 *
 * Every rank is decoded into the same in-memory graph txt2bin builds from GOAL
 * text (nodes in their original order, dependents in appendix order) and
 * written by the same serializer, after a first pass that collects the field
 * maxima. So the output equals what txt2bin writes from the equivalent text
 * with the same options, except for the metadata.
 */
#include <map>
#include <string>
#include <vector>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "Parser.hpp"
#include "goalx_convert_cmdline.h"

static const char* partial_output = NULL;

static void remove_partial_output() {
	if (partial_output != NULL) unlink(partial_output);
}

/* rebuilds rank r of the input as a writer-side graph */
static void load_rank(SerializedGraph& in, const std::vector<int>& slot_of_custom, CustomNames& names,
                      const std::vector<std::string>& custom, Graph& g) {
	uint32_t n = in.GetNumNodes();
	std::vector<Node*> nodes(n);
	std::vector<DeserializedNode> in_nodes(n);
	for (uint32_t i = 0; i < n; i++) {
		const DeserializedNode& op = in_nodes[i] = in.GetNode(i);
		Node* nd = g.addNode();
		nd->Type = op.Type;
		nd->Peer = op.Peer;
		nd->Size = op.Size;
		nd->Tag = op.Tag;
		nd->Proc = op.Proc;
		nd->Nic = op.Nic;
		for (size_t c = 0; c < custom.size(); c++) {
			nd->Custom.push_back(std::make_pair(names.get(custom[c]), in.GetCustom(i, (uint16_t) slot_of_custom[c])));
		}
		nodes[i] = nd;
	}
	for (uint32_t i = 0; i < n; i++) {
		const std::vector<uint32_t>& done = in_nodes[i].DependOnMe;
		for (size_t k = 0; k < done.size(); k++) g.addDependency(nodes[done[k]], nodes[i]);
		const std::vector<uint32_t>& start = in_nodes[i].StartDependOnMe;
		for (size_t k = 0; k < start.size(); k++) g.addStartDependency(nodes[start[k]], nodes[i]);
	}
	for (uint32_t i = 0; i < n; i++) {
		if (nodes[i]->DependenciesCnt != in_nodes[i].DependenciesCnt) {
			fprintf(stderr, "Error: rank node %u: depcnt %u does not match its %u incoming edges\n",
			        i, in_nodes[i].DependenciesCnt, nodes[i]->DependenciesCnt);
			exit(EXIT_FAILURE);
		}
	}
}

int main(int argc, char** argv) {
	gengetopt_args_info args;
	if (cmdline_parser(argc, argv, &args) != 0) exit(EXIT_FAILURE);
	atexit(remove_partial_output);

	std::string err;
	SchemaSpec spec;
	if (!Schema::parse_spec(args.schema_arg, &spec, &err)) {
		fprintf(stderr, "Invalid --schema: %s\n", err.c_str());
		exit(EXIT_FAILURE);
	}

	// the second pass re-reads the mapped input while writing: never write onto it
	struct stat si, so;
	if (stat(args.input_arg, &si) == 0 && stat(args.output_arg, &so) == 0 && si.st_dev == so.st_dev && si.st_ino == so.st_ino) {
		fprintf(stderr, "Error: input and output are the same file (%s); write to a new file\n", args.output_arg);
		exit(EXIT_FAILURE);
	}

	std::vector<Requirement> none;
	Parser in(args.input_arg, false, none, "interp", true);
	const std::vector<std::string>& custom = in.GetCustomSlots();
	std::vector<int> slot_of_custom;
	for (size_t c = 0; c < custom.size(); c++) slot_of_custom.push_back(in.GetCustomSlot(custom[c].c_str()));
	uint32_t num_ranks = in.schedules.size();

	// first pass: maxima
	FieldStats stats;
	CustomNames names;
	for (uint32_t r = 0; r < num_ranks; r++) {
		Graph g;
		load_rank(in.schedules[r], slot_of_custom, names, custom, g);
		g.collect_stats(stats, names, num_ranks);
	}

	Schema schema;
	Layout layout = strcmp(args.layout_arg, "aligned") == 0 ? LAYOUT_ALIGNED : LAYOUT_PACKED;
	if (!Schema::build(spec, stats, layout, &schema, &err)) {
		fprintf(stderr, "Error: %s\n", err.c_str());
		exit(EXIT_FAILURE);
	}
	struct stat st;
	stat(args.input_arg, &st);
	std::string in_path = args.input_arg;
	schema.meta.push_back(std::make_pair(std::string("producer"), std::string("goalx-convert")));
	schema.meta.push_back(std::make_pair(std::string("converted_from"), std::string(basename(&in_path[0]))));
	schema.meta.push_back(std::make_pair(std::string("source_bytes"), std::to_string((unsigned long long) st.st_size)));
	for (unsigned int i = 0; i < args.meta_given; i++) {
		std::string kv = args.meta_arg[i];
		size_t eq = kv.find('=');
		if (eq == std::string::npos || eq == 0) {
			fprintf(stderr, "Invalid --meta '%s', expected key=value\n", kv.c_str());
			exit(EXIT_FAILURE);
		}
		schema.meta.push_back(std::make_pair(kv.substr(0, eq), kv.substr(eq + 1)));
	}

	// second pass: write
	partial_output = args.output_arg;
	int fd = open(args.output_arg, O_RDWR | O_CREAT | O_TRUNC, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
	if (fd == -1) {
		perror(args.output_arg);
		exit(EXIT_FAILURE);
	}
	for (uint32_t r = 0; r < num_ranks; r++) {
		Graph g;
		load_rank(in.schedules[r], slot_of_custom, names, custom, g);
		g.serialize_goalx(fd, r, num_ranks, stats.max_cpu, stats.max_nic, schema, names);
	}
	close(fd);
	partial_output = NULL;
	return 0;
}
