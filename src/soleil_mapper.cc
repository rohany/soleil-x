#include <iostream>
#include <fstream>
#include <regex>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "mappers/default_mapper.h"
#include "realm/logging.h"

#include "config_schema.h"
#include "soleil_mapper.h"

using namespace Legion;
using namespace Legion::Mapping;

//=============================================================================

static Realm::Logger LOG("soleil_mapper");

#define CHECK(cond, ...)                        \
  do {                                          \
    if (!(cond)) {                              \
      LOG.error(__VA_ARGS__);                   \
      exit(1);                                  \
    }                                           \
  } while(0)

#define STARTS_WITH(str, prefix)                \
  (strncmp((str), (prefix), sizeof(prefix) - 1) == 0)

//=============================================================================

struct SampleMapping {
  AddressSpace first_rank;
};

// Maps every sample to a disjoint set of ranks. Each sample gets one rank for
// every tile, as specified in its config file.
class SoleilMapper : public DefaultMapper {
public:
  SoleilMapper(MapperRuntime* rt,
               Machine machine,
               Processor local)
    : DefaultMapper(rt, machine, local, "soleil_mapper") {
    // Set the umask of the process to clear S_IWGRP and S_IWOTH.
    umask(022);
    // Assign ranks sequentially to samples, each sample getting one rank for
    // each tile.
    InputArgs args = Runtime::get_input_args();
    unsigned allocated_ranks = 0;
    auto process_config = [&](char* config_file) {
      Config config = parse_config(config_file);
      CHECK(config.Mapping.xTiles > 0 &&
            config.Mapping.yTiles > 0 &&
            config.Mapping.zTiles > 0,
            "Invalid tiling");
      sample_mappings_.push_back(SampleMapping{
        .first_rank = allocated_ranks,
      });
      allocated_ranks += config.Mapping.xTiles
                       * config.Mapping.yTiles
                       * config.Mapping.zTiles;
    };
    for (int i = 0; i < args.argc; ++i) {
      if (strcmp(args.argv[i], "-i") == 0 && i < args.argc-1) {
        process_config(args.argv[i+1]);
      } else if (strcmp(args.argv[i], "-I") == 0 && i < args.argc-1) {
        std::ifstream csv_file(args.argv[i+1]);
        std::string json_filename;
        while (std::getline(csv_file, json_filename)) {
          process_config(&json_filename[0]);
        }
      }
    }
    unsigned total_ranks = remote_cpus.size();
    CHECK(allocated_ranks <= total_ranks,
          "%d rank(s) required, but %d rank(s) supplied to Legion",
          allocated_ranks, total_ranks);
    if (allocated_ranks < total_ranks) {
      LOG.warning() << total_ranks - allocated_ranks << " rank(s) are unused";
    }
    // TODO: Verify we're running with 1 OpenMP/CPU processor per rank.
  }

public:
  virtual Processor default_policy_select_initial_processor(
                              MapperContext ctx,
                              const Task& task) {
#if 0
    // DOM sweep & boundary tasks are individually launched; find the tile on
    // which they're centered and send them to the rank responsible for that.
    // TODO: Cache the decision.
    bool is_sweep = STARTS_WITH(task.get_task_name(), "sweep_");
    bool is_bound = STARTS_WITH(task.get_task_name(), "bound_");
    if (is_sweep || is_bound) {
      CHECK(task.parent_task != NULL &&
            strcmp(task.parent_task->get_task_name(), "work") == 0,
            "DOM tasks should only be launched from the work task directly.");
      // Retrieve sample information from parent work task.
      const Config* config = find_config(&task);
      unsigned sample_id = config->Mapping.sampleId;
      assert(sample_id < sample_mappings_.size());
      const SampleMapping& mapping = sample_mappings_[sample_id];
      // Find the tile this task launch is centered on.
      DomainPoint tile = DomainPoint::nil();
      for (const RegionRequirement& req : task.regions) {
        assert(req.region.exists());
        const char* name = NULL;
        runtime->retrieve_name(ctx, req.region.get_field_space(), name);
        if ((is_sweep && strcmp(name, "Radiation_columns") == 0) ||
            (is_bound && strcmp(name, "face") == 0)) {
          tile = runtime->get_logical_region_color_point(ctx, req.region);
          break;
        }
      }
      CHECK(!tile.is_null(),
            "Cannot retrieve tile from DOM task launch -- did you change the"
            " names of the field spaces?");
      CHECK(tile.get_dim() == 3,
            "DOM task launches should only use the top-level tiling.");
      // A task that updates the far boundary on some direction is called with
      // a face tile one-over on that direction, so we have to subtract 1 to
      // find the actual tile the launch is centered on.
      if (is_bound) {
        std::regex regex("bound_([xyz])_(lo|hi)");
        std::cmatch match;
        CHECK(std::regex_match(task.get_task_name(), match, regex),
              "Cannot parse name of DOM boundary task -- did you change it?");
        if (match[2].str().compare("hi") == 0) {
          if (match[1].str().compare("x") == 0) { tile[0]--; }
          if (match[1].str().compare("y") == 0) { tile[1]--; }
          if (match[1].str().compare("z") == 0) { tile[2]--; }
        }
      }
      CHECK(tile[0] < config->Mapping.xTiles &&
            tile[1] < config->Mapping.yTiles &&
            tile[2] < config->Mapping.zTiles,
            "DOM task launches should only use the top-level tiling.");
      // Select the 1st (and only) processor of the preferred kind on each
      // target rank.
      VariantInfo info =
        default_find_preferred_variant(task, ctx, false/*needs tight*/);
      const std::vector<Processor>& procs = remote_procs(info.proc_kind);
      // Assign tasks to this sample's ranks in row-major order.
      int rank =
	mapping.first_rank +
	tile[2] +
	config->Mapping.zTiles * tile[1] +
	config->Mapping.zTiles * config->Mapping.yTiles * tile[0];
      LOG.debug() << "Sample " << sample_id << ":"
                  << " Sequential launch: Task " << task.get_task_name()
		  << " on tile " << tile
		  << " mapped to rank " << rank;
      return procs[rank];
    }
#endif
    // Send each work task to the first in the set of ranks allocated to the
    // corresponding sample.
    if (strcmp(task.get_task_name(), "work") == 0) {
      const Config* config = find_config(&task);
      unsigned sample_id = config->Mapping.sampleId;
      assert(sample_id < sample_mappings_.size());
      AddressSpace target_rank = sample_mappings_[sample_id].first_rank;
      Processor target_proc = remote_cpus[target_rank];
      LOG.debug() << "Sample " << sample_id << ":"
                  << " Sequential launch: Work task"
		  << " for sample " << sample_id
		  << " mapped to rank " << target_rank;
      return target_proc;
    }
    // For other tasks, defer to the default mapping policy.
    return DefaultMapper::default_policy_select_initial_processor(ctx, task);
  }

  // TODO: Assign priorities to sweep tasks such that we prioritize the tile
  // that has more dependencies downstream.
  virtual TaskPriority default_policy_select_task_priority(
                              MapperContext ctx,
                              const Task& task) {
    return DefaultMapper::default_policy_select_task_priority(ctx, task);
  }

  // TODO: Select appropriate memories for instances that will be communicated,
  // (e.g. parallelizer-created ghost partitions), such as RDMA memory,
  // zero-copy memory.
  virtual Memory default_policy_select_target_memory(
                              MapperContext ctx,
                              Processor target_proc,
                              const RegionRequirement& req) {
    return DefaultMapper::default_policy_select_target_memory
      (ctx, target_proc, req);
  }

  // Disable an optimization done by the default mapper (attempts to reuse an
  // instance that covers a superset of the requested index space, by searching
  // higher up the partition tree).
  virtual LogicalRegion default_policy_select_instance_region(
                              MapperContext ctx,
                              Memory target_memory,
                              const RegionRequirement& req,
                              const LayoutConstraintSet& constraints,
                              bool force_new_instances,
                              bool meets_constraints) {
    return req.region;
  }

#if 0
  // Farm index-space launches made by work tasks across all the ranks
  // allocated to the corresponding sample.
  // TODO: Cache the decision.
  virtual void slice_task(const MapperContext ctx,
                          const Task& task,
                          const SliceTaskInput& input,
                          SliceTaskOutput& output) {
    CHECK(task.parent_task != NULL &&
          strcmp(task.parent_task->get_task_name(), "work") == 0,
          "Index-space launches only allowed in work task");
    output.verify_correctness = false;
    // Retrieve sample information from parent task.
    const Config* config = find_config(&task);
    unsigned sample_id = config->Mapping.sampleId;
    assert(sample_id < sample_mappings_.size());
    const SampleMapping& mapping = sample_mappings_[sample_id];
    CHECK(input.domain.get_dim() == 3 &&
          input.domain.lo()[0] == 0 &&
          input.domain.lo()[1] == 0 &&
          input.domain.lo()[2] == 0 &&
          input.domain.hi()[0] == config->Mapping.xTiles - 1 &&
          input.domain.hi()[1] == config->Mapping.yTiles - 1 &&
          input.domain.hi()[2] == config->Mapping.zTiles - 1,
          "Index-space launches in the work task should only use the"
          " top-level tiling.");
    // Select the 1st (and only) processor of the same kind as the original
    // target, on each rank allocated to this sample.
    const std::vector<Processor>& procs =
      remote_procs(task.target_proc.kind());
    // Distribute tiles to the ranks in row-major order.
    int next_rank = mapping.first_rank;
    for (int x = 0; x < config->Mapping.xTiles; ++x) {
      for (int y = 0; y < config->Mapping.yTiles; ++y) {
        for (int z = 0; z < config->Mapping.zTiles; ++z) {
          output.slices.emplace_back(Rect<3>(Point<3>(x,y,z), Point<3>(x,y,z)),
                                     procs[next_rank],
                                     false /*recurse*/,
                                     false /*stealable*/);
          LOG.debug() << "Sample " << sample_id << ":"
                      << " Index-space launch: Task " << task.get_task_name()
		      << " on tile (" << x << "," << y << "," << z << ")"
		      << " mapped to rank " << next_rank;
          next_rank++;
        }
      }
    }
  }
#endif

private:
  // TODO: This interface only returns the first processor of the desired kind
  // on each machine; also handle the case where there's more (e.g. >1 GPUs).
  const std::vector<Processor>& remote_procs(Processor::Kind kind) {
    switch (kind) {
    case TOC_PROC: return remote_gpus;
    case LOC_PROC: return remote_cpus;
    case IO_PROC:  return remote_ios;
    case PROC_SET: return remote_procsets;
    case OMP_PROC: return remote_omps;
    case PY_PROC:  return remote_pys;
    default:       assert(false);
    }
    // Should never reach here
    return remote_cpus;
  }

  const Config* find_config(const Task* task) {
    for(; task != NULL; task = task->parent_task) {
      if (strcmp(task->get_task_name(), "work") == 0) {
        const char* ptr = static_cast<const char*>(task->args);
        return reinterpret_cast<const Config*>(ptr + sizeof(uint64_t));
      }
    }
    return NULL;
  }

private:
  std::vector<SampleMapping> sample_mappings_;
};

//=============================================================================

static void create_mappers(Machine machine,
                           HighLevelRuntime* runtime,
                           const std::set<Processor>& local_procs) {
  for (Processor proc : local_procs) {
    SoleilMapper* mapper =
      new SoleilMapper(runtime->get_mapper_runtime(), machine, proc);
    runtime->replace_default_mapper(mapper, proc);
  }
}

void register_mappers() {
  Runtime::add_registration_callback(create_mappers);
}
