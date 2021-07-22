// Copyright (c) 2009-2011, Tor M. Aamodt, Wilson W.L. Fung, George L. Yuan,
// Ali Bakhoda, Andrew Turner, Ivan Sham
// The University of British Columbia
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution. Neither the name of
// The University of British Columbia nor the names of its contributors may be
// used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "gpu-sim.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include "zlib.h"

#include "dram.h"
#include "mem_fetch.h"
#include "shader.h"
#include "shader_trace.h"

#include <time.h>
#include "addrdec.h"
#include "delayqueue.h"
#include "dram.h"
#include "gpu-cache.h"
#include "gpu-misc.h"
#include "icnt_wrapper.h"
#include "l2cache.h"
#include "shader.h"
#include "stat-tool.h"

#include "../../libcuda/gpgpu_context.h"
#include "../abstract_hardware_model.h"
#include "../cuda-sim/cuda-sim.h"
#include "../cuda-sim/cuda_device_runtime.h"
#include "../cuda-sim/ptx-stats.h"
#include "../cuda-sim/ptx_ir.h"
#include "../debug.h"
#include "../gpgpusim_entrypoint.h"
#include "../statwrapper.h"
#include "../trace.h"
#include "mem_latency_stat.h"
#include "power_stat.h"
#include "stats.h"
#include "visualizer.h"

#ifdef GPGPUSIM_POWER_MODEL
#include "power_interface.h"
#else
class gpgpu_sim_wrapper {};
#endif

#include <stdio.h>
#include <string.h>
#include <iostream>
#include <sstream>
#include <string>

#define MAX(a, b) (((a) > (b)) ? (a) : (b))

bool g_interactive_debugger_enabled = false;

tr1_hash_map<new_addr_type, unsigned> address_random_interleaving;

/* Clock Domains */

#define CORE 0x01
#define L2 0x02
#define DRAM 0x04
#define ICNT 0x08

#define MEM_LATENCY_STAT_IMPL

#include "mem_latency_stat.h"

void power_config::reg_options(class OptionParser *opp) {
  option_parser_register(opp, "-gpuwattch_xml_file", OPT_CSTR,
                         &g_power_config_name, "GPUWattch XML file",
                         "gpuwattch.xml");

  option_parser_register(opp, "-power_simulation_enabled", OPT_BOOL,
                         &g_power_simulation_enabled,
                         "Turn on power simulator (1=On, 0=Off)", "0");

  option_parser_register(opp, "-power_per_cycle_dump", OPT_BOOL,
                         &g_power_per_cycle_dump,
                         "Dump detailed power output each cycle", "0");

  // Output Data Formats
  option_parser_register(
      opp, "-power_trace_enabled", OPT_BOOL, &g_power_trace_enabled,
      "produce a file for the power trace (1=On, 0=Off)", "0");

  option_parser_register(
      opp, "-power_trace_zlevel", OPT_INT32, &g_power_trace_zlevel,
      "Compression level of the power trace output log (0=no comp, 9=highest)",
      "6");

  option_parser_register(
      opp, "-steady_power_levels_enabled", OPT_BOOL,
      &g_steady_power_levels_enabled,
      "produce a file for the steady power levels (1=On, 0=Off)", "0");

  option_parser_register(opp, "-steady_state_definition", OPT_CSTR,
                         &gpu_steady_state_definition,
                         "allowed deviation:number of samples", "8:4");
}

void memory_config::reg_options(class OptionParser *opp) {
  option_parser_register(opp, "-perf_sim_memcpy", OPT_BOOL, &m_perf_sim_memcpy,
                         "Fill the L2 cache on memcpy", "1");
  option_parser_register(opp, "-simple_dram_model", OPT_BOOL,
                         &simple_dram_model,
                         "simple_dram_model with fixed latency and BW", "0");
  option_parser_register(opp, "-gpgpu_dram_scheduler", OPT_INT32,
                         &scheduler_type, "0 = fifo, 1 = FR-FCFS (defaul)",
                         "1");
  option_parser_register(opp, "-gpgpu_dram_partition_queues", OPT_CSTR,
                         &gpgpu_L2_queue_config, "i2$:$2d:d2$:$2i", "8:8:8:8");

  option_parser_register(opp, "-l2_ideal", OPT_BOOL, &l2_ideal,
                         "Use a ideal L2 cache that always hit", "0");
  option_parser_register(opp, "-gpgpu_cache:dl2", OPT_CSTR,
                         &m_L2_config.m_config_string,
                         "unified banked L2 data cache config "
                         " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_"
                         "alloc>,<mshr>:<N>:<merge>,<mq>}",
                         "64:128:8,L:B:m:N,A:16:4,4");
  option_parser_register(opp, "-gpgpu_cache:dl2_texture_only", OPT_BOOL,
                         &m_L2_texure_only, "L2 cache used for texture only",
                         "1");
  option_parser_register(
      opp, "-gpgpu_n_mem", OPT_UINT32, &m_n_mem,
      "number of memory modules (e.g. memory controllers) in gpu", "8");
  option_parser_register(opp, "-gpgpu_n_sub_partition_per_mchannel", OPT_UINT32,
                         &m_n_sub_partition_per_memory_channel,
                         "number of memory subpartition in each memory module",
                         "1");
  option_parser_register(opp, "-gpgpu_n_mem_per_ctrlr", OPT_UINT32,
                         &gpu_n_mem_per_ctrlr,
                         "number of memory chips per memory controller", "1");
  option_parser_register(opp, "-gpgpu_memlatency_stat", OPT_INT32,
                         &gpgpu_memlatency_stat,
                         "track and display latency statistics 0x2 enables MC, "
                         "0x4 enables queue logs",
                         "0");
  option_parser_register(opp, "-gpgpu_frfcfs_dram_sched_queue_size", OPT_INT32,
                         &gpgpu_frfcfs_dram_sched_queue_size,
                         "0 = unlimited (default); # entries per chip", "0");
  option_parser_register(opp, "-gpgpu_dram_return_queue_size", OPT_INT32,
                         &gpgpu_dram_return_queue_size,
                         "0 = unlimited (default); # entries per chip", "0");
  option_parser_register(opp, "-gpgpu_dram_buswidth", OPT_UINT32, &busW,
                         "default = 4 bytes (8 bytes per cycle at DDR)", "4");
  option_parser_register(
      opp, "-gpgpu_dram_burst_length", OPT_UINT32, &BL,
      "Burst length of each DRAM request (default = 4 data bus cycle)", "4");
  option_parser_register(opp, "-dram_data_command_freq_ratio", OPT_UINT32,
                         &data_command_freq_ratio,
                         "Frequency ratio between DRAM data bus and command "
                         "bus (default = 2 times, i.e. DDR)",
                         "2");
  option_parser_register(
      opp, "-gpgpu_dram_timing_opt", OPT_CSTR, &gpgpu_dram_timing_opt,
      "DRAM timing parameters = "
      "{nbk:tCCD:tRRD:tRCD:tRAS:tRP:tRC:CL:WL:tCDLR:tWR:nbkgrp:tCCDL:tRTPL}",
      "4:2:8:12:21:13:34:9:4:5:13:1:0:0");
  option_parser_register(opp, "-rop_latency", OPT_UINT32, &rop_latency,
                         "ROP queue latency (default 85)", "85");
  option_parser_register(opp, "-dram_latency", OPT_UINT32, &dram_latency,
                         "DRAM latency (default 30)", "30");
  option_parser_register(opp, "-dual_bus_interface", OPT_UINT32,
                         &dual_bus_interface,
                         "dual_bus_interface (default = 0) ", "0");
  option_parser_register(opp, "-dram_bnk_indexing_policy", OPT_UINT32,
                         &dram_bnk_indexing_policy,
                         "dram_bnk_indexing_policy (0 = normal indexing, 1 = "
                         "Xoring with the higher bits) (Default = 0)",
                         "0");
  option_parser_register(opp, "-dram_bnkgrp_indexing_policy", OPT_UINT32,
                         &dram_bnkgrp_indexing_policy,
                         "dram_bnkgrp_indexing_policy (0 = take higher bits, 1 "
                         "= take lower bits) (Default = 0)",
                         "0");
  option_parser_register(opp, "-Seperate_Write_Queue_Enable", OPT_BOOL,
                         &seperate_write_queue_enabled,
                         "Seperate_Write_Queue_Enable", "0");
  option_parser_register(opp, "-Write_Queue_Size", OPT_CSTR,
                         &write_queue_size_opt, "Write_Queue_Size", "32:28:16");
  option_parser_register(
      opp, "-Elimnate_rw_turnaround", OPT_BOOL, &elimnate_rw_turnaround,
      "elimnate_rw_turnaround i.e set tWTR and tRTW = 0", "0");
  option_parser_register(opp, "-icnt_flit_size", OPT_UINT32, &icnt_flit_size,
                         "icnt_flit_size", "32");
  m_address_mapping.addrdec_setoption(opp);
}

void shader_core_config::reg_options(class OptionParser *opp) {
  option_parser_register(opp, "-gpgpu_simd_model", OPT_INT32, &model,
                         "1 = post-dominator", "1");
  option_parser_register(
      opp, "-gpgpu_shader_core_pipeline", OPT_CSTR,
      &gpgpu_shader_core_pipeline_opt,
      "shader core pipeline config, i.e., {<nthread>:<warpsize>}", "1024:32");
  option_parser_register(opp, "-gpgpu_tex_cache:l1", OPT_CSTR,
                         &m_L1T_config.m_config_string,
                         "per-shader L1 texture cache  (READ-ONLY) config "
                         " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_"
                         "alloc>,<mshr>:<N>:<merge>,<mq>:<rf>}",
                         "8:128:5,L:R:m:N,F:128:4,128:2");
  option_parser_register(
      opp, "-gpgpu_const_cache:l1", OPT_CSTR, &m_L1C_config.m_config_string,
      "per-shader L1 constant memory cache  (READ-ONLY) config "
      " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_alloc>,<mshr>:<N>:<"
      "merge>,<mq>} ",
      "64:64:2,L:R:f:N,A:2:32,4");
  option_parser_register(opp, "-gpgpu_cache:il1", OPT_CSTR,
                         &m_L1I_config.m_config_string,
                         "shader L1 instruction cache config "
                         " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_"
                         "alloc>,<mshr>:<N>:<merge>,<mq>} ",
                         "4:256:4,L:R:f:N,A:2:32,4");
  option_parser_register(opp, "-gpgpu_cache:dl1", OPT_CSTR,
                         &m_L1D_config.m_config_string,
                         "per-shader L1 data cache config "
                         " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_"
                         "alloc>,<mshr>:<N>:<merge>,<mq> | none}",
                         "none");
  option_parser_register(opp, "-l1_banks", OPT_UINT32, &m_L1D_config.l1_banks,
                         "The number of L1 cache banks", "1");
  option_parser_register(opp, "-l1_latency", OPT_UINT32,
                         &m_L1D_config.l1_latency, "L1 Hit Latency", "1");
  option_parser_register(opp, "-smem_latency", OPT_UINT32, &smem_latency,
                         "smem Latency", "3");
  option_parser_register(opp, "-gpgpu_cache:dl1PrefL1", OPT_CSTR,
                         &m_L1D_config.m_config_stringPrefL1,
                         "per-shader L1 data cache config "
                         " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_"
                         "alloc>,<mshr>:<N>:<merge>,<mq> | none}",
                         "none");
  option_parser_register(opp, "-gpgpu_cache:dl1PrefShared", OPT_CSTR,
                         &m_L1D_config.m_config_stringPrefShared,
                         "per-shader L1 data cache config "
                         " {<nsets>:<bsize>:<assoc>,<rep>:<wr>:<alloc>:<wr_"
                         "alloc>,<mshr>:<N>:<merge>,<mq> | none}",
                         "none");
  option_parser_register(opp, "-gmem_skip_L1D", OPT_BOOL, &gmem_skip_L1D,
                         "global memory access skip L1D cache (implements "
                         "-Xptxas -dlcm=cg, default=no skip)",
                         "0");

  option_parser_register(opp, "-gpgpu_perfect_mem", OPT_BOOL,
                         &gpgpu_perfect_mem,
                         "enable perfect memory mode (no cache miss)", "0");
  option_parser_register(
      opp, "-n_regfile_gating_group", OPT_UINT32, &n_regfile_gating_group,
      "group of lanes that should be read/written together)", "4");
  option_parser_register(
      opp, "-gpgpu_clock_gated_reg_file", OPT_BOOL, &gpgpu_clock_gated_reg_file,
      "enable clock gated reg file for power calculations", "0");
  option_parser_register(
      opp, "-gpgpu_clock_gated_lanes", OPT_BOOL, &gpgpu_clock_gated_lanes,
      "enable clock gated lanes for power calculations", "0");
  option_parser_register(opp, "-gpgpu_shader_registers", OPT_UINT32,
                         &gpgpu_shader_registers,
                         "Number of registers per shader core. Limits number "
                         "of concurrent CTAs. (default 8192)",
                         "8192");
  option_parser_register(
      opp, "-gpgpu_registers_per_block", OPT_UINT32, &gpgpu_registers_per_block,
      "Maximum number of registers per CTA. (default 8192)", "8192");
  option_parser_register(opp, "-gpgpu_ignore_resources_limitation", OPT_BOOL,
                         &gpgpu_ignore_resources_limitation,
                         "gpgpu_ignore_resources_limitation (default 0)", "0");
  option_parser_register(
      opp, "-gpgpu_shader_cta", OPT_UINT32, &max_cta_per_core,
      "Maximum number of concurrent CTAs in shader (default 8)", "8");
  option_parser_register(
      opp, "-gpgpu_num_cta_barriers", OPT_UINT32, &max_barriers_per_cta,
      "Maximum number of named barriers per CTA (default 16)", "16");
  option_parser_register(opp, "-gpgpu_n_clusters", OPT_UINT32, &n_simt_clusters,
                         "number of processing clusters", "10");
  option_parser_register(opp, "-gpgpu_n_cores_per_cluster", OPT_UINT32,
                         &n_simt_cores_per_cluster,
                         "number of simd cores per cluster", "3");
  option_parser_register(opp, "-gpgpu_n_cluster_ejection_buffer_size",
                         OPT_UINT32, &n_simt_ejection_buffer_size,
                         "number of packets in ejection buffer", "8");
  option_parser_register(
      opp, "-gpgpu_n_ldst_response_buffer_size", OPT_UINT32,
      &ldst_unit_response_queue_size,
      "number of response packets in ld/st unit ejection buffer", "2");
  option_parser_register(
      opp, "-gpgpu_shmem_per_block", OPT_UINT32, &gpgpu_shmem_per_block,
      "Size of shared memory per thread block or CTA (default 48kB)", "49152");
  option_parser_register(
      opp, "-gpgpu_shmem_size", OPT_UINT32, &gpgpu_shmem_size,
      "Size of shared memory per shader core (default 16kB)", "16384");
  option_parser_register(opp, "-adaptive_cache_config", OPT_BOOL,
                         &adaptive_cache_config, "adaptive_cache_config", "0");
  option_parser_register(
      opp, "-gpgpu_shmem_sizeDefault", OPT_UINT32, &gpgpu_shmem_sizeDefault,
      "Size of shared memory per shader core (default 16kB)", "16384");
  option_parser_register(
      opp, "-gpgpu_shmem_size_PrefL1", OPT_UINT32, &gpgpu_shmem_sizePrefL1,
      "Size of shared memory per shader core (default 16kB)", "16384");
  option_parser_register(opp, "-gpgpu_shmem_size_PrefShared", OPT_UINT32,
                         &gpgpu_shmem_sizePrefShared,
                         "Size of shared memory per shader core (default 16kB)",
                         "16384");
  option_parser_register(
      opp, "-gpgpu_shmem_num_banks", OPT_UINT32, &num_shmem_bank,
      "Number of banks in the shared memory in each shader core (default 16)",
      "16");
  option_parser_register(
      opp, "-gpgpu_shmem_limited_broadcast", OPT_BOOL, &shmem_limited_broadcast,
      "Limit shared memory to do one broadcast per cycle (default on)", "1");
  option_parser_register(opp, "-gpgpu_shmem_warp_parts", OPT_INT32,
                         &mem_warp_parts,
                         "Number of portions a warp is divided into for shared "
                         "memory bank conflict check ",
                         "2");
  option_parser_register(
      opp, "-mem_unit_ports", OPT_INT32, &mem_unit_ports,
      "The number of memory transactions allowed per core cycle", "1");
  option_parser_register(opp, "-gpgpu_shmem_warp_parts", OPT_INT32,
                         &mem_warp_parts,
                         "Number of portions a warp is divided into for shared "
                         "memory bank conflict check ",
                         "2");
  option_parser_register(
      opp, "-gpgpu_warpdistro_shader", OPT_INT32, &gpgpu_warpdistro_shader,
      "Specify which shader core to collect the warp size distribution from",
      "-1");
  option_parser_register(
      opp, "-gpgpu_warp_issue_shader", OPT_INT32, &gpgpu_warp_issue_shader,
      "Specify which shader core to collect the warp issue distribution from",
      "0");
  option_parser_register(opp, "-gpgpu_local_mem_map", OPT_BOOL,
                         &gpgpu_local_mem_map,
                         "Mapping from local memory space address to simulated "
                         "GPU physical address space (default = enabled)",
                         "1");
  option_parser_register(opp, "-gpgpu_num_reg_banks", OPT_INT32,
                         &gpgpu_num_reg_banks,
                         "Number of register banks (default = 8)", "8");
  option_parser_register(
      opp, "-gpgpu_reg_bank_use_warp_id", OPT_BOOL, &gpgpu_reg_bank_use_warp_id,
      "Use warp ID in mapping registers to banks (default = off)", "0");
  option_parser_register(opp, "-sub_core_model", OPT_BOOL, &sub_core_model,
                         "Sub Core Volta/Pascal model (default = off)", "0");
  option_parser_register(opp, "-enable_specialized_operand_collector", OPT_BOOL,
                         &enable_specialized_operand_collector,
                         "enable_specialized_operand_collector", "1");
  option_parser_register(opp, "-gpgpu_operand_collector_num_units_sp",
                         OPT_INT32, &gpgpu_operand_collector_num_units_sp,
                         "number of collector units (default = 4)", "4");
  option_parser_register(opp, "-gpgpu_operand_collector_num_units_dp",
                         OPT_INT32, &gpgpu_operand_collector_num_units_dp,
                         "number of collector units (default = 0)", "0");
  option_parser_register(opp, "-gpgpu_operand_collector_num_units_sfu",
                         OPT_INT32, &gpgpu_operand_collector_num_units_sfu,
                         "number of collector units (default = 4)", "4");
  option_parser_register(opp, "-gpgpu_operand_collector_num_units_int",
                         OPT_INT32, &gpgpu_operand_collector_num_units_int,
                         "number of collector units (default = 0)", "0");
  option_parser_register(opp, "-gpgpu_operand_collector_num_units_tensor_core",
                         OPT_INT32,
                         &gpgpu_operand_collector_num_units_tensor_core,
                         "number of collector units (default = 4)", "4");
  option_parser_register(opp, "-gpgpu_operand_collector_num_units_mem",
                         OPT_INT32, &gpgpu_operand_collector_num_units_mem,
                         "number of collector units (default = 2)", "2");
  option_parser_register(opp, "-gpgpu_operand_collector_num_units_gen",
                         OPT_INT32, &gpgpu_operand_collector_num_units_gen,
                         "number of collector units (default = 0)", "0");
  option_parser_register(opp, "-gpgpu_operand_collector_num_in_ports_sp",
                         OPT_INT32, &gpgpu_operand_collector_num_in_ports_sp,
                         "number of collector unit in ports (default = 1)",
                         "1");
  option_parser_register(opp, "-gpgpu_operand_collector_num_in_ports_dp",
                         OPT_INT32, &gpgpu_operand_collector_num_in_ports_dp,
                         "number of collector unit in ports (default = 0)",
                         "0");
  option_parser_register(opp, "-gpgpu_operand_collector_num_in_ports_sfu",
                         OPT_INT32, &gpgpu_operand_collector_num_in_ports_sfu,
                         "number of collector unit in ports (default = 1)",
                         "1");
  option_parser_register(opp, "-gpgpu_operand_collector_num_in_ports_int",
                         OPT_INT32, &gpgpu_operand_collector_num_in_ports_int,
                         "number of collector unit in ports (default = 0)",
                         "0");
  option_parser_register(
      opp, "-gpgpu_operand_collector_num_in_ports_tensor_core", OPT_INT32,
      &gpgpu_operand_collector_num_in_ports_tensor_core,
      "number of collector unit in ports (default = 1)", "1");
  option_parser_register(opp, "-gpgpu_operand_collector_num_in_ports_mem",
                         OPT_INT32, &gpgpu_operand_collector_num_in_ports_mem,
                         "number of collector unit in ports (default = 1)",
                         "1");
  option_parser_register(opp, "-gpgpu_operand_collector_num_in_ports_gen",
                         OPT_INT32, &gpgpu_operand_collector_num_in_ports_gen,
                         "number of collector unit in ports (default = 0)",
                         "0");
  option_parser_register(opp, "-gpgpu_operand_collector_num_out_ports_sp",
                         OPT_INT32, &gpgpu_operand_collector_num_out_ports_sp,
                         "number of collector unit in ports (default = 1)",
                         "1");
  option_parser_register(opp, "-gpgpu_operand_collector_num_out_ports_dp",
                         OPT_INT32, &gpgpu_operand_collector_num_out_ports_dp,
                         "number of collector unit in ports (default = 0)",
                         "0");
  option_parser_register(opp, "-gpgpu_operand_collector_num_out_ports_sfu",
                         OPT_INT32, &gpgpu_operand_collector_num_out_ports_sfu,
                         "number of collector unit in ports (default = 1)",
                         "1");
  option_parser_register(opp, "-gpgpu_operand_collector_num_out_ports_int",
                         OPT_INT32, &gpgpu_operand_collector_num_out_ports_int,
                         "number of collector unit in ports (default = 0)",
                         "0");
  option_parser_register(
      opp, "-gpgpu_operand_collector_num_out_ports_tensor_core", OPT_INT32,
      &gpgpu_operand_collector_num_out_ports_tensor_core,
      "number of collector unit in ports (default = 1)", "1");
  option_parser_register(opp, "-gpgpu_operand_collector_num_out_ports_mem",
                         OPT_INT32, &gpgpu_operand_collector_num_out_ports_mem,
                         "number of collector unit in ports (default = 1)",
                         "1");
  option_parser_register(opp, "-gpgpu_operand_collector_num_out_ports_gen",
                         OPT_INT32, &gpgpu_operand_collector_num_out_ports_gen,
                         "number of collector unit in ports (default = 0)",
                         "0");
  option_parser_register(opp, "-gpgpu_coalesce_arch", OPT_INT32,
                         &gpgpu_coalesce_arch,
                         "Coalescing arch (GT200 = 13, Fermi = 20)", "13");
  option_parser_register(opp, "-gpgpu_num_sched_per_core", OPT_INT32,
                         &gpgpu_num_sched_per_core,
                         "Number of warp schedulers per core", "1");
  option_parser_register(opp, "-gpgpu_max_insn_issue_per_warp", OPT_INT32,
                         &gpgpu_max_insn_issue_per_warp,
                         "Max number of instructions that can be issued per "
                         "warp in one cycle by scheduler (either 1 or 2)",
                         "2");
  option_parser_register(opp, "-gpgpu_dual_issue_diff_exec_units", OPT_BOOL,
                         &gpgpu_dual_issue_diff_exec_units,
                         "should dual issue use two different execution unit "
                         "resources (Default = 1)",
                         "1");
  option_parser_register(opp, "-gpgpu_simt_core_sim_order", OPT_INT32,
                         &simt_core_sim_order,
                         "Select the simulation order of cores in a cluster "
                         "(0=Fix, 1=Round-Robin)",
                         "1");
  option_parser_register(
      opp, "-gpgpu_pipeline_widths", OPT_CSTR, &pipeline_widths_string,
      "Pipeline widths "
      "ID_OC_SP,ID_OC_DP,ID_OC_INT,ID_OC_SFU,ID_OC_MEM,OC_EX_SP,OC_EX_DP,OC_EX_"
      "INT,OC_EX_SFU,OC_EX_MEM,EX_WB,ID_OC_TENSOR_CORE,OC_EX_TENSOR_CORE",
      "1,1,1,1,1,1,1,1,1,1,1,1,1");
  option_parser_register(opp, "-gpgpu_tensor_core_avail", OPT_INT32,
                         &gpgpu_tensor_core_avail,
                         "Tensor Core Available (default=0)", "0");
  option_parser_register(opp, "-gpgpu_num_sp_units", OPT_INT32,
                         &gpgpu_num_sp_units, "Number of SP units (default=1)",
                         "1");
  option_parser_register(opp, "-gpgpu_num_dp_units", OPT_INT32,
                         &gpgpu_num_dp_units, "Number of DP units (default=0)",
                         "0");
  option_parser_register(opp, "-gpgpu_num_int_units", OPT_INT32,
                         &gpgpu_num_int_units,
                         "Number of INT units (default=0)", "0");
  option_parser_register(opp, "-gpgpu_num_sfu_units", OPT_INT32,
                         &gpgpu_num_sfu_units, "Number of SF units (default=1)",
                         "1");
  option_parser_register(opp, "-gpgpu_num_tensor_core_units", OPT_INT32,
                         &gpgpu_num_tensor_core_units,
                         "Number of tensor_core units (default=1)", "1");
  option_parser_register(
      opp, "-gpgpu_num_mem_units", OPT_INT32, &gpgpu_num_mem_units,
      "Number if ldst units (default=1) WARNING: not hooked up to anything",
      "1");
  option_parser_register(
      opp, "-gpgpu_scheduler", OPT_CSTR, &gpgpu_scheduler_string,
      "Scheduler configuration: < lrr | gto | two_level_active > "
      "If "
      "two_level_active:<num_active_warps>:<inner_prioritization>:<outer_"
      "prioritization>"
      "For complete list of prioritization values see shader.h enum "
      "scheduler_prioritization_type"
      "Default: gto",
      "gto");

  option_parser_register(
      opp, "-gpgpu_concurrent_kernel_sm", OPT_BOOL, &gpgpu_concurrent_kernel_sm,
      "Support concurrent kernels on a SM (default = disabled)", "0");
}

void gpgpu_sim_config::reg_options(option_parser_t opp) {
  gpgpu_functional_sim_config::reg_options(opp);
  m_shader_config.reg_options(opp);
  m_memory_config.reg_options(opp);
  power_config::reg_options(opp);
  option_parser_register(opp, "-gpgpu_max_cycle", OPT_INT64, &gpu_max_cycle_opt,
                         "terminates gpu simulation early (0 = no limit)", "0");
  option_parser_register(opp, "-gpgpu_max_insn", OPT_INT64, &gpu_max_insn_opt,
                         "terminates gpu simulation early (0 = no limit)", "0");
  option_parser_register(opp, "-gpgpu_max_cta", OPT_INT32, &gpu_max_cta_opt,
                         "terminates gpu simulation early (0 = no limit)", "0");
  option_parser_register(opp, "-gpgpu_max_completed_cta", OPT_INT32,
                         &gpu_max_completed_cta_opt,
                         "terminates gpu simulation early (0 = no limit)", "0");
  option_parser_register(
      opp, "-gpgpu_runtime_stat", OPT_CSTR, &gpgpu_runtime_stat,
      "display runtime statistics such as dram utilization {<freq>:<flag>}",
      "10000:0");
  option_parser_register(opp, "-liveness_message_freq", OPT_INT64,
                         &liveness_message_freq,
                         "Minimum number of seconds between simulation "
                         "liveness messages (0 = always print)",
                         "1");
  option_parser_register(opp, "-gpgpu_compute_capability_major", OPT_UINT32,
                         &gpgpu_compute_capability_major,
                         "Major compute capability version number", "7");
  option_parser_register(opp, "-gpgpu_compute_capability_minor", OPT_UINT32,
                         &gpgpu_compute_capability_minor,
                         "Minor compute capability version number", "0");
  option_parser_register(opp, "-gpgpu_flush_l1_cache", OPT_BOOL,
                         &gpgpu_flush_l1_cache,
                         "Flush L1 cache at the end of each kernel call", "0");
  option_parser_register(opp, "-gpgpu_flush_l2_cache", OPT_BOOL,
                         &gpgpu_flush_l2_cache,
                         "Flush L2 cache at the end of each kernel call", "0");
  option_parser_register(
      opp, "-gpgpu_deadlock_detect", OPT_BOOL, &gpu_deadlock_detect,
      "Stop the simulation at deadlock (1=on (default), 0=off)", "1");
  option_parser_register(
      opp, "-gpgpu_ptx_instruction_classification", OPT_INT32,
      &(gpgpu_ctx->func_sim->gpgpu_ptx_instruction_classification),
      "if enabled will classify ptx instruction types per kernel (Max 255 "
      "kernels now)",
      "0");
  option_parser_register(
      opp, "-gpgpu_ptx_sim_mode", OPT_INT32,
      &(gpgpu_ctx->func_sim->g_ptx_sim_mode),
      "Select between Performance (default) or Functional simulation (1)", "0");
  option_parser_register(opp, "-gpgpu_clock_domains", OPT_CSTR,
                         &gpgpu_clock_domains,
                         "Clock Domain Frequencies in MhZ {<Core Clock>:<ICNT "
                         "Clock>:<L2 Clock>:<DRAM Clock>}",
                         "500.0:2000.0:2000.0:2000.0");
  option_parser_register(
      opp, "-gpgpu_max_concurrent_kernel", OPT_INT32, &max_concurrent_kernel,
      "maximum kernels that can run concurrently on GPU", "8");
  option_parser_register(
      opp, "-gpgpu_cflog_interval", OPT_INT32, &gpgpu_cflog_interval,
      "Interval between each snapshot in control flow logger", "0");
  option_parser_register(opp, "-visualizer_enabled", OPT_BOOL,
                         &g_visualizer_enabled,
                         "Turn on visualizer output (1=On, 0=Off)", "1");
  option_parser_register(opp, "-visualizer_outputfile", OPT_CSTR,
                         &g_visualizer_filename,
                         "Specifies the output log file for visualizer", NULL);
  option_parser_register(
      opp, "-visualizer_zlevel", OPT_INT32, &g_visualizer_zlevel,
      "Compression level of the visualizer output log (0=no comp, 9=highest)",
      "6");
  option_parser_register(opp, "-gpgpu_stack_size_limit", OPT_INT32,
                         &stack_size_limit, "GPU thread stack size", "1024");
  option_parser_register(opp, "-gpgpu_heap_size_limit", OPT_INT32,
                         &heap_size_limit, "GPU malloc heap size ", "8388608");
  option_parser_register(opp, "-gpgpu_runtime_sync_depth_limit", OPT_INT32,
                         &runtime_sync_depth_limit,
                         "GPU device runtime synchronize depth", "2");
  option_parser_register(opp, "-gpgpu_runtime_pending_launch_count_limit",
                         OPT_INT32, &runtime_pending_launch_count_limit,
                         "GPU device runtime pending launch count", "2048");
  option_parser_register(opp, "-trace_enabled", OPT_BOOL, &Trace::enabled,
                         "Turn on traces", "0");
  option_parser_register(opp, "-trace_components", OPT_CSTR, &Trace::config_str,
                         "comma seperated list of traces to enable. "
                         "Complete list found in trace_streams.tup. "
                         "Default none",
                         "none");
  option_parser_register(
      opp, "-trace_sampling_core", OPT_INT32, &Trace::sampling_core,
      "The core which is printed using CORE_DPRINTF. Default 0", "0");
  option_parser_register(opp, "-trace_sampling_memory_partition", OPT_INT32,
                         &Trace::sampling_memory_partition,
                         "The memory partition which is printed using "
                         "MEMPART_DPRINTF. Default -1 (i.e. all)",
                         "-1");
  gpgpu_ctx->stats->ptx_file_line_stats_options(opp);

  // Jin: kernel launch latency
  option_parser_register(opp, "-gpgpu_kernel_launch_latency", OPT_INT32,
                         &(gpgpu_ctx->device_runtime->g_kernel_launch_latency),
                         "Kernel launch latency in cycles. Default: 0", "0");
  option_parser_register(opp, "-gpgpu_cdp_enabled", OPT_BOOL,
                         &(gpgpu_ctx->device_runtime->g_cdp_enabled),
                         "Turn on CDP", "0");

  //Nico: reading max_ctas_per_cluster
  option_parser_register(opp, "-gpgpu_smk_mctas_kernel1", OPT_INT32, 
                        &gpu_smk_mctas_kernel1, "Max ctas per cluster of kernel1", "0");
  option_parser_register(opp, "-gpgpu_smt_SMs_kernel1", OPT_INT32, 
                        &gpu_smt_SMs_kernel1, "SMs assigned to kernel1", "0");                     
  option_parser_register(opp, "-gpgpu_smk_stats_filename", OPT_CSTR, 
                        &gpu_smk_stats_filename, "Output file with coexecution results", NULL);                  

}

/////////////////////////////////////////////////////////////////////////////

void increment_x_then_y_then_z(dim3 &i, const dim3 &bound) {
  i.x++;
  if (i.x >= bound.x) {
    i.x = 0;
    i.y++;
    if (i.y >= bound.y) {
      i.y = 0;
      if (i.z < bound.z) i.z++;
    }
  }
}

void gpgpu_sim::launch(kernel_info_t *kinfo) {

  unsigned cta_size = kinfo->threads_per_cta();

  
  //Nico: Assign max number of ctas running in a cluster
 // switch (kinfo->get_uid()){
  //  case 1: kinfo->set_max_ctas(m_config.gpu_smk_mctas_kernel1);
   //         break;
    //case 2: kinfo->set_max_ctas(m_config.gpu_smk_mctas_kernel2);
     //       break;
   // default:
    //        printf("Too many kernels");
     //       exit(-1);
  //}

  if (cta_size > m_shader_config->n_thread_per_shader) {
    printf(
        "Execution error: Shader kernel CTA (block) size is too large for "
        "microarch config.\n");
    printf("                 CTA size (x*y*z) = %u, max supported = %u\n",
           cta_size, m_shader_config->n_thread_per_shader);
    printf(
        "                 => either change -gpgpu_shader argument in "
        "gpgpusim.config file or\n");
    printf(
        "                 modify the CUDA source to decrease the kernel block "
        "size.\n");
    abort();
  }
  unsigned n = 0;
  for (n = 0; n < m_running_kernels.size(); n++) {
    if ((NULL == m_running_kernels[n]) || m_running_kernels[n]->done()) {
      kinfo->status =  kernel_info_t::t_Kernel_Status::INIT;
      m_running_kernels[n] = kinfo;
      break;
    }
  }
  assert(n < m_running_kernels.size());
}

bool gpgpu_sim::can_start_kernel() {
  for (unsigned n = 0; n < m_running_kernels.size(); n++) {
    if ((NULL == m_running_kernels[n]) || m_running_kernels[n]->done())
      return true;
  }
  return false;
}

bool gpgpu_sim::hit_max_cta_count() const {
  if (m_config.gpu_max_cta_opt != 0) {
    if ((gpu_tot_issued_cta + m_total_cta_launched) >= m_config.gpu_max_cta_opt)
      return true;
  }
  return false;
}

bool gpgpu_sim::kernel_more_cta_left(kernel_info_t *kernel) const {
  if (hit_max_cta_count()) return false;

  if (kernel && !kernel->no_more_ctas_to_run()) return true;

  return false;
}

bool gpgpu_sim::get_more_cta_left() const {
  if (hit_max_cta_count()) return false;

  for (unsigned n = 0; n < m_running_kernels.size(); n++) {
    if (m_running_kernels[n] && !m_running_kernels[n]->no_more_ctas_to_run())
      return true;
  }
  return false;
}

//Nico: select next kernel to launch a CTA 
kernel_info_t *gpgpu_sim::select_alternative_kernel(unsigned idx){
	assert(idx < m_running_kernels.size());
	
	if (m_running_kernels[idx]) { // Nico: If a kernel is available
	
		unsigned launch_uid = m_running_kernels[idx]->get_uid();
		if (std::find(m_executed_kernel_uids.begin(), m_executed_kernel_uids.end(),
                  launch_uid) == m_executed_kernel_uids.end()) {
			m_running_kernels[idx]->start_cycle =  // Nico: Fisrt time kernel is scheduled: annotate 
			gpu_sim_cycle + gpu_tot_sim_cycle;
			m_executed_kernel_uids.push_back(launch_uid);
			m_executed_kernel_names.push_back(
			m_running_kernels[idx]->name());
		}
		return m_running_kernels[idx];
	}
	else
		return NULL;
		
}

kernel_info_t *gpgpu_sim::select_kernel() {
  if (m_running_kernels[m_last_issued_kernel] &&
      !m_running_kernels[m_last_issued_kernel]->no_more_ctas_to_run()) {
    unsigned launch_uid = m_running_kernels[m_last_issued_kernel]->get_uid();
    if (std::find(m_executed_kernel_uids.begin(), m_executed_kernel_uids.end(),
                  launch_uid) == m_executed_kernel_uids.end()) {
      m_running_kernels[m_last_issued_kernel]->start_cycle =
          gpu_sim_cycle + gpu_tot_sim_cycle;
      m_executed_kernel_uids.push_back(launch_uid);
      m_executed_kernel_names.push_back(
          m_running_kernels[m_last_issued_kernel]->name());
    }
    return m_running_kernels[m_last_issued_kernel];
  }

  for (unsigned n = 0; n < m_running_kernels.size(); n++) {
    unsigned idx =
        (n + m_last_issued_kernel + 1) % m_config.max_concurrent_kernel;
    if (kernel_more_cta_left(m_running_kernels[idx])) {
      m_last_issued_kernel = idx;
      m_running_kernels[idx]->start_cycle = gpu_sim_cycle + gpu_tot_sim_cycle;
      // record this kernel for stat print if it is the first time this kernel
      // is selected for execution
      unsigned launch_uid = m_running_kernels[idx]->get_uid();
      assert(std::find(m_executed_kernel_uids.begin(),
                       m_executed_kernel_uids.end(),
                       launch_uid) == m_executed_kernel_uids.end());
      m_executed_kernel_uids.push_back(launch_uid);
      m_executed_kernel_names.push_back(m_running_kernels[idx]->name());

      return m_running_kernels[idx];
    }
  }
  return NULL;
}

unsigned gpgpu_sim::finished_kernel() {
  if (m_finished_kernel.empty()) return 0;
  unsigned result = m_finished_kernel.front();
  m_finished_kernel.pop_front();
  return result;
}

void gpgpu_sim::set_kernel_done(kernel_info_t *kernel) {
  unsigned uid = kernel->get_uid();
  m_finished_kernel.push_back(uid);
  std::vector<kernel_info_t *>::iterator k;
  for (k = m_running_kernels.begin(); k != m_running_kernels.end(); k++) {
    if (*k == kernel) {
      kernel->end_cycle = gpu_sim_cycle + gpu_tot_sim_cycle;
      *k = NULL;
      break;
    }
  }
  assert(k != m_running_kernels.end());
}

void gpgpu_sim::stop_all_running_kernels() {
  std::vector<kernel_info_t *>::iterator k;
  for (k = m_running_kernels.begin(); k != m_running_kernels.end(); ++k) {
    if (*k != NULL) {       // If a kernel is active
      set_kernel_done(*k);  // Stop the kernel
      assert(*k == NULL);
    }
  }
}

gpgpu_sim::gpgpu_sim(const gpgpu_sim_config &config, gpgpu_context *ctx)
    : gpgpu_t(config, ctx), m_config(config) {
  gpgpu_ctx = ctx;
  m_shader_config = &m_config.m_shader_config;
  m_memory_config = &m_config.m_memory_config;
  ctx->ptx_parser->set_ptx_warp_size(m_shader_config);
  ptx_file_line_stats_create_exposed_latency_tracker(m_config.num_shader());

#ifdef GPGPUSIM_POWER_MODEL
  m_gpgpusim_wrapper = new gpgpu_sim_wrapper(config.g_power_simulation_enabled,
                                             config.g_power_config_name);
#endif

  m_shader_stats = new shader_core_stats(m_shader_config);
  m_memory_stats = new memory_stats_t(m_config.num_shader(), m_shader_config,
                                      m_memory_config, this);
  average_pipeline_duty_cycle = (float *)malloc(sizeof(float));
  active_sms = (float *)malloc(sizeof(float));
  m_power_stats =
      new power_stat_t(m_shader_config, average_pipeline_duty_cycle, active_sms,
                       m_shader_stats, m_memory_config, m_memory_stats);

  gpu_sim_insn = 0;
  gpu_tot_sim_insn = 0;
  gpu_tot_issued_cta = 0;
  gpu_completed_cta = 0;
  m_total_cta_launched = 0;
  gpu_deadlock = false;

  gpu_stall_dramfull = 0;
  gpu_stall_icnt2sh = 0;
  partiton_reqs_in_parallel = 0;
  partiton_reqs_in_parallel_total = 0;
  partiton_reqs_in_parallel_util = 0;
  partiton_reqs_in_parallel_util_total = 0;
  gpu_sim_cycle_parition_util = 0;
  gpu_tot_sim_cycle_parition_util = 0;
  partiton_replys_in_parallel = 0;
  partiton_replys_in_parallel_total = 0;

  m_cluster = new simt_core_cluster *[m_shader_config->n_simt_clusters];
  for (unsigned i = 0; i < m_shader_config->n_simt_clusters; i++)
    m_cluster[i] =
        new simt_core_cluster(this, i, m_shader_config, m_memory_config,
                              m_shader_stats, m_memory_stats);

  m_memory_partition_unit =
      new memory_partition_unit *[m_memory_config->m_n_mem];
  m_memory_sub_partition =
      new memory_sub_partition *[m_memory_config->m_n_mem_sub_partition];
  for (unsigned i = 0; i < m_memory_config->m_n_mem; i++) {
    m_memory_partition_unit[i] =
        new memory_partition_unit(i, m_memory_config, m_memory_stats, this);
    for (unsigned p = 0;
         p < m_memory_config->m_n_sub_partition_per_memory_channel; p++) {
      unsigned submpid =
          i * m_memory_config->m_n_sub_partition_per_memory_channel + p;
      m_memory_sub_partition[submpid] =
          m_memory_partition_unit[i]->get_sub_partition(p);
    }
  }

  icnt_wrapper_init();
  icnt_create(m_shader_config->n_simt_clusters,
              m_memory_config->m_n_mem_sub_partition);

  time_vector_create(NUM_MEM_REQ_STAT);
  fprintf(stdout,
          "GPGPU-Sim uArch: performance model initialization complete.\n");

  m_running_kernels.resize(config.max_concurrent_kernel, NULL);
  m_last_issued_kernel = 0;
  m_last_cluster_issue = m_shader_config->n_simt_clusters -
                         1;  // this causes first launch to use simt cluster 0
  *average_pipeline_duty_cycle = 0;
  *active_sms = 0;

  last_liveness_message_time = 0;

  // Nico: cycle and instruction count per kernel
  gpu_sim_insn_per_kernel = new unsigned long long[config.get_max_concurrent_kernel()];
  gpu_tot_sim_insn_per_kernel = new unsigned long long[config.get_max_concurrent_kernel()];
  gpu_sim_start_kernel_cycle = new unsigned long long[config.get_max_concurrent_kernel()];
  gpu_sim_start_kernel_inst = new unsigned long long[config.get_max_concurrent_kernel()];


  // Jin: functional simulation for CDP
  m_functional_sim = false;
  m_functional_sim_kernel = NULL;
}

int gpgpu_sim::shared_mem_size() const {
  return m_shader_config->gpgpu_shmem_size;
}

int gpgpu_sim::shared_mem_per_block() const {
  return m_shader_config->gpgpu_shmem_per_block;
}

int gpgpu_sim::num_registers_per_core() const {
  return m_shader_config->gpgpu_shader_registers;
}

int gpgpu_sim::num_registers_per_block() const {
  return m_shader_config->gpgpu_registers_per_block;
}

int gpgpu_sim::wrp_size() const { return m_shader_config->warp_size; }

int gpgpu_sim::shader_clock() const { return m_config.core_freq / 1000; }

int gpgpu_sim::max_cta_per_core() const {
  return m_shader_config->max_cta_per_core;
}

int gpgpu_sim::get_max_cta(const kernel_info_t &k) const {
  return m_shader_config->max_cta(k);
}

void gpgpu_sim::set_prop(cudaDeviceProp *prop) { m_cuda_properties = prop; }

int gpgpu_sim::compute_capability_major() const {
  return m_config.gpgpu_compute_capability_major;
}

int gpgpu_sim::compute_capability_minor() const {
  return m_config.gpgpu_compute_capability_minor;
}

const struct cudaDeviceProp *gpgpu_sim::get_prop() const {
  return m_cuda_properties;
}

enum divergence_support_t gpgpu_sim::simd_model() const {
  return m_shader_config->model;
}

void gpgpu_sim_config::init_clock_domains(void) {
  sscanf(gpgpu_clock_domains, "%lf:%lf:%lf:%lf", &core_freq, &icnt_freq,
         &l2_freq, &dram_freq);
  core_freq = core_freq MhZ;
  icnt_freq = icnt_freq MhZ;
  l2_freq = l2_freq MhZ;
  dram_freq = dram_freq MhZ;
  core_period = 1 / core_freq;
  icnt_period = 1 / icnt_freq;
  dram_period = 1 / dram_freq;
  l2_period = 1 / l2_freq;
  printf("GPGPU-Sim uArch: clock freqs: %lf:%lf:%lf:%lf\n", core_freq,
         icnt_freq, l2_freq, dram_freq);
  printf("GPGPU-Sim uArch: clock periods: %.20lf:%.20lf:%.20lf:%.20lf\n",
         core_period, icnt_period, l2_period, dram_period);
}

void gpgpu_sim::reinit_clock_domains(void) {
  core_time = 0;
  dram_time = 0;
  icnt_time = 0;
  l2_time = 0;
}

bool gpgpu_sim::active() {
  if (m_config.gpu_max_cycle_opt &&
      (gpu_tot_sim_cycle + gpu_sim_cycle) >= m_config.gpu_max_cycle_opt)
    return false;
  if (m_config.gpu_max_insn_opt &&
      (gpu_tot_sim_insn + gpu_sim_insn) >= m_config.gpu_max_insn_opt)
    return false;
  if (m_config.gpu_max_cta_opt &&
      (gpu_tot_issued_cta >= m_config.gpu_max_cta_opt))
    return false;
  if (m_config.gpu_max_completed_cta_opt &&
      (gpu_completed_cta >= m_config.gpu_max_completed_cta_opt))
    return false;
  if (m_config.gpu_deadlock_detect && gpu_deadlock) return false;
  for (unsigned i = 0; i < m_shader_config->n_simt_clusters; i++)
    if (m_cluster[i]->get_not_completed() > 0) return true;
  ;
  for (unsigned i = 0; i < m_memory_config->m_n_mem; i++)
    if (m_memory_partition_unit[i]->busy() > 0) return true;
  ;
  if (icnt_busy()) return true;
  if (get_more_cta_left()) return true;
  return false;
}

void gpgpu_sim::init() {
  // run a CUDA grid on the GPU microarchitecture simulator
  gpu_sim_cycle = 0;
  gpu_sim_insn = 0;
  last_gpu_sim_insn = 0;
  m_total_cta_launched = 0;
  gpu_completed_cta = 0;
  partiton_reqs_in_parallel = 0;
  partiton_replys_in_parallel = 0;
  partiton_reqs_in_parallel_util = 0;
  gpu_sim_cycle_parition_util = 0;

  reinit_clock_domains();
  gpgpu_ctx->func_sim->set_param_gpgpu_num_shaders(m_config.num_shader());
  for (unsigned i = 0; i < m_shader_config->n_simt_clusters; i++)
    m_cluster[i]->reinit();
  m_shader_stats->new_grid();
  // initialize the control-flow, memory access, memory latency logger
  if (m_config.g_visualizer_enabled) {
    create_thread_CFlogger(gpgpu_ctx, m_config.num_shader(),
                           m_shader_config->n_thread_per_shader, 0,
                           m_config.gpgpu_cflog_interval);
  }
  shader_CTA_count_create(m_config.num_shader(), m_config.gpgpu_cflog_interval);
  if (m_config.gpgpu_cflog_interval != 0) {
    insn_warp_occ_create(m_config.num_shader(), m_shader_config->warp_size);
    shader_warp_occ_create(m_config.num_shader(), m_shader_config->warp_size,
                           m_config.gpgpu_cflog_interval);
    shader_mem_acc_create(m_config.num_shader(), m_memory_config->m_n_mem, 4,
                          m_config.gpgpu_cflog_interval);
    shader_mem_lat_create(m_config.num_shader(), m_config.gpgpu_cflog_interval);
    shader_cache_access_create(m_config.num_shader(), 3,
                               m_config.gpgpu_cflog_interval);
    set_spill_interval(m_config.gpgpu_cflog_interval * 40);
  }

  if (g_network_mode) icnt_init();

    // McPAT initialization function. Called on first launch of GPU
#ifdef GPGPUSIM_POWER_MODEL
  if (m_config.g_power_simulation_enabled) {
    init_mcpat(m_config, m_gpgpusim_wrapper, m_config.gpu_stat_sample_freq,
               gpu_tot_sim_insn, gpu_sim_insn);
  }
#endif
}
 
void gpgpu_sim::update_stats() {
  m_memory_stats->memlatstat_lat_pw();
  gpu_tot_sim_cycle += gpu_sim_cycle;
  gpu_tot_sim_insn += gpu_sim_insn;
  //Nico: accumulating number of executed instruction per kernel
  gpgpu_sim_config const config = get_config();
  for (unsigned k = 0; k<config.get_max_concurrent_kernel(); k++)
	  gpu_tot_sim_insn_per_kernel[k] += gpu_sim_insn_per_kernel[k];
  gpu_tot_issued_cta += m_total_cta_launched;
  partiton_reqs_in_parallel_total += partiton_reqs_in_parallel;
  partiton_replys_in_parallel_total += partiton_replys_in_parallel;
  partiton_reqs_in_parallel_util_total += partiton_reqs_in_parallel_util;
  gpu_tot_sim_cycle_parition_util += gpu_sim_cycle_parition_util;
  gpu_tot_occupancy += gpu_occupancy;

  gpu_sim_cycle = 0;
  partiton_reqs_in_parallel = 0;
  partiton_replys_in_parallel = 0;
  partiton_reqs_in_parallel_util = 0;
  gpu_sim_cycle_parition_util = 0;
  gpu_sim_insn = 0;
  
  //Nico: reseting instrucion per kernel
  for (unsigned k = 0; k<config.get_max_concurrent_kernel(); k++)
	gpu_sim_insn_per_kernel[k] = 0;

  m_total_cta_launched = 0;
  gpu_completed_cta = 0;
  gpu_occupancy = occupancy_stats();
}

void gpgpu_sim::print_stats() {
  gpgpu_ctx->stats->ptx_file_line_stats_write_file();
  gpu_print_stat();

  if (g_network_mode) {
    printf(
        "----------------------------Interconnect-DETAILS----------------------"
        "----------\n");
    icnt_display_stats();
    icnt_display_overall_stats();
    printf(
        "----------------------------END-of-Interconnect-DETAILS---------------"
        "----------\n");
  }
}

void gpgpu_sim::deadlock_check() {
  if (m_config.gpu_deadlock_detect && gpu_deadlock) {
    fflush(stdout);
    printf(
        "\n\nGPGPU-Sim uArch: ERROR ** deadlock detected: last writeback core "
        "%u @ gpu_sim_cycle %u (+ gpu_tot_sim_cycle %u) (%u cycles ago)\n",
        gpu_sim_insn_last_update_sid, (unsigned)gpu_sim_insn_last_update,
        (unsigned)(gpu_tot_sim_cycle - gpu_sim_cycle),
        (unsigned)(gpu_sim_cycle - gpu_sim_insn_last_update));
    unsigned num_cores = 0;
    for (unsigned i = 0; i < m_shader_config->n_simt_clusters; i++) {
      unsigned not_completed = m_cluster[i]->get_not_completed();
      if (not_completed) {
        if (!num_cores) {
          printf(
              "GPGPU-Sim uArch: DEADLOCK  shader cores no longer committing "
              "instructions [core(# threads)]:\n");
          printf("GPGPU-Sim uArch: DEADLOCK  ");
          m_cluster[i]->print_not_completed(stdout);
        } else if (num_cores < 8) {
          m_cluster[i]->print_not_completed(stdout);
        } else if (num_cores >= 8) {
          printf(" + others ... ");
        }
        num_cores += m_shader_config->n_simt_cores_per_cluster;
      }
    }
    printf("\n");
    for (unsigned i = 0; i < m_memory_config->m_n_mem; i++) {
      bool busy = m_memory_partition_unit[i]->busy();
      if (busy)
        printf("GPGPU-Sim uArch DEADLOCK:  memory partition %u busy\n", i);
    }
    if (icnt_busy()) {
      printf("GPGPU-Sim uArch DEADLOCK:  iterconnect contains traffic\n");
      icnt_display_state(stdout);
    }
    printf(
        "\nRe-run the simulator in gdb and use debug routines in .gdbinit to "
        "debug this\n");
    fflush(stdout);
    abort();
  }
}

/// printing the names and uids of a set of executed kernels (usually there is
/// only one)
std::string gpgpu_sim::executed_kernel_info_string() {
  std::stringstream statout;

  statout << "kernel_name = ";
  for (unsigned int k = 0; k < m_executed_kernel_names.size(); k++) {
    statout << m_executed_kernel_names[k] << " ";
  }
  statout << std::endl;
  statout << "kernel_launch_uid = ";
  for (unsigned int k = 0; k < m_executed_kernel_uids.size(); k++) {
    statout << m_executed_kernel_uids[k] << " ";
  }
  statout << std::endl;

  return statout.str();
}
void gpgpu_sim::set_cache_config(std::string kernel_name,
                                 FuncCache cacheConfig) {
  m_special_cache_config[kernel_name] = cacheConfig;
}

FuncCache gpgpu_sim::get_cache_config(std::string kernel_name) {
  for (std::map<std::string, FuncCache>::iterator iter =
           m_special_cache_config.begin();
       iter != m_special_cache_config.end(); iter++) {
    std::string kernel = iter->first;
    if (kernel_name.compare(kernel) == 0) {
      return iter->second;
    }
  }
  return (FuncCache)0;
}

bool gpgpu_sim::has_special_cache_config(std::string kernel_name) {
  for (std::map<std::string, FuncCache>::iterator iter =
           m_special_cache_config.begin();
       iter != m_special_cache_config.end(); iter++) {
    std::string kernel = iter->first;
    if (kernel_name.compare(kernel) == 0) {
      return true;
    }
  }
  return false;
}

void gpgpu_sim::set_cache_config(std::string kernel_name) {
  if (has_special_cache_config(kernel_name)) {
    change_cache_config(get_cache_config(kernel_name));
  } else {
    change_cache_config(FuncCachePreferNone);
  }
}

void gpgpu_sim::change_cache_config(FuncCache cache_config) {
  if (cache_config != m_shader_config->m_L1D_config.get_cache_status()) {
    printf("FLUSH L1 Cache at configuration change between kernels\n");
    for (unsigned i = 0; i < m_shader_config->n_simt_clusters; i++) {
      m_cluster[i]->cache_invalidate();
    }
  }

  switch (cache_config) {
    case FuncCachePreferNone:
      m_shader_config->m_L1D_config.init(
          m_shader_config->m_L1D_config.m_config_string, FuncCachePreferNone);
      m_shader_config->gpgpu_shmem_size =
          m_shader_config->gpgpu_shmem_sizeDefault;
      break;
    case FuncCachePreferL1:
      if ((m_shader_config->m_L1D_config.m_config_stringPrefL1 == NULL) ||
          (m_shader_config->gpgpu_shmem_sizePrefL1 == (unsigned)-1)) {
        printf("WARNING: missing Preferred L1 configuration\n");
        m_shader_config->m_L1D_config.init(
            m_shader_config->m_L1D_config.m_config_string, FuncCachePreferNone);
        m_shader_config->gpgpu_shmem_size =
            m_shader_config->gpgpu_shmem_sizeDefault;

      } else {
        m_shader_config->m_L1D_config.init(
            m_shader_config->m_L1D_config.m_config_stringPrefL1,
            FuncCachePreferL1);
        m_shader_config->gpgpu_shmem_size =
            m_shader_config->gpgpu_shmem_sizePrefL1;
      }
      break;
    case FuncCachePreferShared:
      if ((m_shader_config->m_L1D_config.m_config_stringPrefShared == NULL) ||
          (m_shader_config->gpgpu_shmem_sizePrefShared == (unsigned)-1)) {
        printf("WARNING: missing Preferred L1 configuration\n");
        m_shader_config->m_L1D_config.init(
            m_shader_config->m_L1D_config.m_config_string, FuncCachePreferNone);
        m_shader_config->gpgpu_shmem_size =
            m_shader_config->gpgpu_shmem_sizeDefault;
      } else {
        m_shader_config->m_L1D_config.init(
            m_shader_config->m_L1D_config.m_config_stringPrefShared,
            FuncCachePreferShared);
        m_shader_config->gpgpu_shmem_size =
            m_shader_config->gpgpu_shmem_sizePrefShared;
      }
      break;
    default:
      break;
  }
}

void gpgpu_sim::clear_executed_kernel_info() {
  m_executed_kernel_names.clear();
  m_executed_kernel_uids.clear();
}

// kernel contains the information of the finished kernel. It has been alrady removed from m_running_kernels array. Thus, only one kernel should be in m_running_kernels
void gpgpu_sim::print_only_ipc_stats(kernel_info_t *kernel)
{

  FILE *fp;

  if (m_config.gpu_smk_stats_filename == NULL) {
    printf("Error: ouput file stats in NULL\n");
    return;
  } 

  if ((fp = fopen(m_config.gpu_smk_stats_filename, "a")) == NULL) {
    printf("Error: file %s cannot be opened for writing\n");
    return;
  }

  // Localize coexecutiing ready kernel
  kernel_info_t *co_kernel=NULL;
  for (int k=0; k<get_num_running_kernels();k++){
    if (m_running_kernels[k]!=NULL) {
      if (kernel != m_running_kernels[k]){
          co_kernel = m_running_kernels[k];
          break;
      }
    }
  }
  
  
  
  /*kernel_info_t *co_kernel=NULL;
  int cont =0;
  for (int k=0; k<get_num_running_kernels();k++){
    if (m_running_kernels[k]!=NULL) {
      if (co_kernel == NULL)
        co_kernel = m_running_kernels[k];
      cont++;
    }
  }

  
  if (cont > 1) {
    fprintf(fp,"Error: More than two kernels are coexecuting\n");
    fclose(fp);
    exit(0); 
  }
  */
  
  unsigned int resources1, resources2;
  char type[10];
  if (m_config.get_SMT_SMs_kernel1()!=0) 
    strcpy(type, "SMT");
  else
    if (m_config.get_SMK_ctas_kernel1()!=0) 
      strcpy(type, "SMK");
    else 
      strcpy(type, "NO_CKE");

  // Accumulate RB accesses and hits
  unsigned long long rb_total_accesses1 = 0, rb_total_accesses2 = 0;
  unsigned long long rb_total_hits1 = 0, rb_total_hits2=0;
  for (int chip=0; chip < m_memory_config->m_n_mem; chip++) {
      rb_total_hits1 += m_memory_stats->row_buffer_hits[kernel->get_uid()][chip];
      rb_total_accesses1 += m_memory_stats->row_buffer_access[kernel->get_uid()][chip];
      if (co_kernel != NULL) {
        rb_total_hits2 += m_memory_stats->row_buffer_hits[co_kernel->get_uid()][chip];
        rb_total_accesses2 += m_memory_stats->row_buffer_access[co_kernel->get_uid()][chip];
      }
  }

  // Calculated dram bandwith of kernel
  unsigned int *buf_bwutil=NULL;
  unsigned long long g_bwutil1=0, g_bwutil2=0;
  unsigned long long g_ncmd=0;
  for (int chip=0; chip< m_memory_config->m_n_mem; chip++){
      buf_bwutil = m_memory_partition_unit[chip]->get_dram()->get_Kbwutil();
      g_bwutil1 += buf_bwutil[kernel->get_uid()];
      if (co_kernel != NULL) 
        g_bwutil2 += buf_bwutil[co_kernel->get_uid()];
      g_ncmd += m_memory_partition_unit[chip]->get_dram()->get_ncmd();
  }

  // Imax calculatio: maximum number of instructionsper cycle
  unsigned int ilp = 2 * m_config.m_shader_config.n_simt_cores_per_cluster; 
  unsigned int warpsize = m_config.m_shader_config.warp_size;
  double Imax =  ilp * warpsize * m_config.num_cluster(); //* m_config.m_shader_config.n_simt_cores_per_cluster;
  //double max_bw = 480.0; // In gigabytes/s  Pascal
  double max_bw =  768.0; // TITANV with HBM     
  // Bmax according HSM paper (in GB/s)
  unsigned int request_size = 8*4; // Burst size -> eight words of 32 bits for GDDR5

  // Execution cycles
  unsigned long long exec_cycles = gpu_tot_sim_cycle + gpu_sim_cycle-gpu_sim_start_kernel_cycle[kernel->get_uid()];

  // More values*1
  unsigned long long total_accesses1 = m_memory_stats->total_kernel_accesses[kernel->get_uid()];
  unsigned long long ins1 = gpu_sim_insn_per_kernel[kernel->get_uid()];
  double ipc1 = (double)ins1/(double)(exec_cycles);
  double Bmax1 = ((double)Imax / ipc1) * ((double)total_accesses1/(double)exec_cycles)  * (double)(request_size) * 1.417;
  
  // Nico: number of instructions and ipc perp  kernel
  if (co_kernel == NULL) { // No co-running kernels, print information of the finishing kernel
    resources1 = m_config.num_cluster();
    
    fprintf(fp, "%s,%s,%d,%d,", type, kernel->name().c_str(), resources1, kernel->get_next_cta_id_single());  // Single kernel Info
    fprintf(fp, "None,0,0,"); // No concurrent kernel
    fprintf(fp, "%s,%lld,",  kernel->name().c_str(), exec_cycles); // Single kernel total execution cycles
    fprintf(fp, "%lld,0,", gpu_sim_insn_per_kernel[kernel->get_uid()]); // Single kernel total executed instructions
    fprintf(fp, "%.2f, 0", ipc1); // IPC
    fprintf(fp,",%lld, %lld, 0, 0, %.2f, 0,", rb_total_accesses1, rb_total_hits1, (double)g_bwutil1/(double)g_ncmd * max_bw);
    fprintf(fp, "%.2f, 0\n", Bmax1);
    fprintf(fp, "\n");
    fclose(fp);
    return;
  }

  unsigned long long total_accesses2 = m_memory_stats->total_kernel_accesses[co_kernel->get_uid()];
  unsigned long long ins2 = gpu_sim_insn_per_kernel[co_kernel->get_uid()];
  double ipc2 = (double)ins2/(double)(exec_cycles);
  double Bmax2 = ((double)Imax / ipc2) * ((double)total_accesses2/(double)exec_cycles)  * (double)(request_size) * 1.417;
  
  if (m_config.get_SMT_SMs_kernel1()!=0){ // Calcule comptaional rsources used by kernels
      resources1 = m_config.get_SMT_SMs_kernel1();
      resources2 = m_config.num_cluster() -  m_config.get_SMT_SMs_kernel1();
  }
  else{
      resources1 = kernel->max_ctas_per_core[0]+kernel->max_ctas_per_core[1];
      resources2 = co_kernel->max_ctas_per_core[0]+co_kernel->max_ctas_per_core[1];
  }

  if (kernel->get_uid() < co_kernel->get_uid()){ // If first launched kernel has finished

    fprintf(fp, "%s,%s,%d,%d,", type, kernel->name().c_str(), resources1, kernel->get_next_cta_id_single()); // Print info of first launched kernel
    fprintf(fp, "%s,%d,%d,", co_kernel->name().c_str(), resources2, co_kernel->get_next_cta_id_single()); // Print info of second launched kernel
  }
  else {
    fprintf(fp,"%s,%s,%d,%d,", type, co_kernel->name().c_str(), resources1, co_kernel->get_next_cta_id_single()); // Second launched kernel has finished. Print info 
    fprintf(fp, "%s,%d,%d,", kernel->name().c_str(), resources2, kernel->get_next_cta_id_single()); 
  }
  
  fprintf(fp, "%s, %lld,",  kernel->name().c_str(), exec_cycles); // Name and cycles executed by the finishing kernel 
  
  if (kernel->get_uid() < co_kernel->get_uid()){ // First launched kernel has finished:
    fprintf(fp, "%lld,", ins1); // Number of instructions executed by the first kernel
    fprintf(fp, "%lld,", ins2); // Number of instructions executed by the second kernel
    
    fprintf(fp, "%.2f,", ipc1); // IPC for coexection
    fprintf(fp, "%.2f,", ipc2); // IPC for coexection 
    fprintf(fp,"%lld, %lld, %lld, %lld,  %.2f, %.2f, %.2f, %.2f", rb_total_accesses1, rb_total_hits1, rb_total_accesses2, rb_total_hits2, 
    (double)g_bwutil1/(double)g_ncmd * max_bw, (double)g_bwutil2/(double)g_ncmd * max_bw, Bmax1, Bmax2);
  }
  else
  {
    fprintf(fp, "%lld,", ins2); 
    fprintf(fp, "%lld,", ins1); 
    fprintf(fp, "%.2f,", ipc2);
    fprintf(fp, "%.2f,", ipc1);
    fprintf(fp,"%lld, %lld, %lld, %lld, %.2f, %.2f, %.2f, %.2f", rb_total_accesses2, rb_total_hits2, rb_total_accesses1, rb_total_hits1, 
    (double)g_bwutil2/(double)g_ncmd * max_bw, (double)g_bwutil1/(double)g_ncmd * max_bw, Bmax2, Bmax1);
  }
  
  fprintf(fp, "\n");
  fclose(fp);

 // If coexecution, simulation is stopped.
  exit(0); 
}

void gpgpu_sim::gpu_print_stat() {
  FILE *statfout = stdout;

  std::string kernel_info_str = executed_kernel_info_string();
  fprintf(statfout, "%s", kernel_info_str.c_str());

  printf("gpu_sim_cycle = %lld\n", gpu_sim_cycle);
  printf("gpu_sim_insn = %lld\n", gpu_sim_insn);
  printf("gpu_ipc = %12.4f\n", (float)gpu_sim_insn / gpu_sim_cycle);
  printf("gpu_tot_sim_cycle = %lld\n", gpu_tot_sim_cycle + gpu_sim_cycle);
  printf("gpu_tot_sim_insn = %lld\n", gpu_tot_sim_insn + gpu_sim_insn);
  printf("gpu_tot_ipc = %12.4f\n", (float)(gpu_tot_sim_insn + gpu_sim_insn) /
                                       (gpu_tot_sim_cycle + gpu_sim_cycle));
									  
  // Nico: number of instructions and ipc per kernel
  gpgpu_sim_config const config = get_config();
  printf("Instrucions per kernel: ");
  for (unsigned k=0; k<config.get_max_concurrent_kernel();k++)
	  printf("%d->%lld\t", k, gpu_tot_sim_insn_per_kernel[k] + gpu_sim_insn_per_kernel[k]);
 printf("\n");
 printf("ipc per kernel: ");
  for (unsigned k=0; k<config.get_max_concurrent_kernel();k++)
	  printf("%d->%12.4f\t", k, (double) (gpu_tot_sim_insn_per_kernel[k] + gpu_sim_insn_per_kernel[k])/
		(double)(gpu_tot_sim_cycle + gpu_sim_cycle));
 printf("\n");
 
 printf("gpu_tot_issued_cta = %lld\n",
         gpu_tot_issued_cta + m_total_cta_launched);
  printf("gpu_occupancy = %.4f%% \n", gpu_occupancy.get_occ_fraction() * 100);
  printf("gpu_tot_occupancy = %.4f%% \n",
         (gpu_occupancy + gpu_tot_occupancy).get_occ_fraction() * 100);

  fprintf(statfout, "max_total_param_size = %llu\n",
          gpgpu_ctx->device_runtime->g_max_total_param_size);

  // performance counter for stalls due to congestion.
  printf("gpu_stall_dramfull = %d\n", gpu_stall_dramfull);
  printf("gpu_stall_icnt2sh    = %d\n", gpu_stall_icnt2sh);

  // printf("partiton_reqs_in_parallel = %lld\n", partiton_reqs_in_parallel);
  // printf("partiton_reqs_in_parallel_total    = %lld\n",
  // partiton_reqs_in_parallel_total );
  printf("partiton_level_parallism = %12.4f\n",
         (float)partiton_reqs_in_parallel / gpu_sim_cycle);
  printf("partiton_level_parallism_total  = %12.4f\n",
         (float)(partiton_reqs_in_parallel + partiton_reqs_in_parallel_total) /
             (gpu_tot_sim_cycle + gpu_sim_cycle));
  // printf("partiton_reqs_in_parallel_util = %lld\n",
  // partiton_reqs_in_parallel_util);
  // printf("partiton_reqs_in_parallel_util_total    = %lld\n",
  // partiton_reqs_in_parallel_util_total ); printf("gpu_sim_cycle_parition_util
  // = %lld\n", gpu_sim_cycle_parition_util);
  // printf("gpu_tot_sim_cycle_parition_util    = %lld\n",
  // gpu_tot_sim_cycle_parition_util );
  printf("partiton_level_parallism_util = %12.4f\n",
         (float)partiton_reqs_in_parallel_util / gpu_sim_cycle_parition_util);
  printf("partiton_level_parallism_util_total  = %12.4f\n",
         (float)(partiton_reqs_in_parallel_util +
                 partiton_reqs_in_parallel_util_total) /
             (gpu_sim_cycle_parition_util + gpu_tot_sim_cycle_parition_util));
  // printf("partiton_replys_in_parallel = %lld\n",
  // partiton_replys_in_parallel); printf("partiton_replys_in_parallel_total =
  // %lld\n", partiton_replys_in_parallel_total );
  printf("L2_BW  = %12.4f GB/Sec\n",
         ((float)(partiton_replys_in_parallel * 32) /
          (gpu_sim_cycle * m_config.icnt_period)) /
             1000000000);
  printf("L2_BW_total  = %12.4f GB/Sec\n",
         ((float)((partiton_replys_in_parallel +
                   partiton_replys_in_parallel_total) *
                  32) /
          ((gpu_tot_sim_cycle + gpu_sim_cycle) * m_config.icnt_period)) /
             1000000000);

  time_t curr_time;
  time(&curr_time);
  unsigned long long elapsed_time =
      MAX(curr_time - gpgpu_ctx->the_gpgpusim->g_simulation_starttime, 1);
  printf("gpu_total_sim_rate=%u\n",
         (unsigned)((gpu_tot_sim_insn + gpu_sim_insn) / elapsed_time));

  // shader_print_l1_miss_stat( stdout );
  shader_print_cache_stats(stdout);

  cache_stats core_cache_stats;
  core_cache_stats.clear();
  for (unsigned i = 0; i < m_config.num_cluster(); i++) {
    m_cluster[i]->get_cache_stats(core_cache_stats);
  }
  printf("\nTotal_core_cache_stats:\n");
  core_cache_stats.print_stats(stdout, "Total_core_cache_stats_breakdown");
  printf("\nTotal_core_cache_fail_stats:\n");
  core_cache_stats.print_fail_stats(stdout,
                                    "Total_core_cache_fail_stats_breakdown");
  shader_print_scheduler_stat(stdout, false);

  m_shader_stats->print(stdout);
#ifdef GPGPUSIM_POWER_MODEL
  if (m_config.g_power_simulation_enabled) {
    m_gpgpusim_wrapper->print_power_kernel_stats(
        gpu_sim_cycle, gpu_tot_sim_cycle, gpu_tot_sim_insn + gpu_sim_insn,
        kernel_info_str, true);
    mcpat_reset_perf_count(m_gpgpusim_wrapper);
  }
#endif

  // performance counter that are not local to one shader
  m_memory_stats->memlatstat_print(m_memory_config->m_n_mem,
                                   m_memory_config->nbk);
  for (unsigned i = 0; i < m_memory_config->m_n_mem; i++)
    m_memory_partition_unit[i]->print(stdout);

  // L2 cache stats
  if (!m_memory_config->m_L2_config.disabled()) {
    cache_stats l2_stats;
    struct cache_sub_stats l2_css;
    struct cache_sub_stats total_l2_css;
    l2_stats.clear();
    l2_css.clear();
    total_l2_css.clear();

    printf("\n========= L2 cache stats =========\n");
    for (unsigned i = 0; i < m_memory_config->m_n_mem_sub_partition; i++) {
      m_memory_sub_partition[i]->accumulate_L2cache_stats(l2_stats);
      m_memory_sub_partition[i]->get_L2cache_sub_stats(l2_css);

      fprintf(stdout,
              "L2_cache_bank[%d]: Access = %llu, Miss = %llu, Miss_rate = "
              "%.3lf, Pending_hits = %llu, Reservation_fails = %llu\n",
              i, l2_css.accesses, l2_css.misses,
              (double)l2_css.misses / (double)l2_css.accesses,
              l2_css.pending_hits, l2_css.res_fails);

      total_l2_css += l2_css;
    }
    if (!m_memory_config->m_L2_config.disabled() &&
        m_memory_config->m_L2_config.get_num_lines()) {
      // L2c_print_cache_stat();
      printf("L2_total_cache_accesses = %llu\n", total_l2_css.accesses);
      printf("L2_total_cache_misses = %llu\n", total_l2_css.misses);
      if (total_l2_css.accesses > 0)
        printf("L2_total_cache_miss_rate = %.4lf\n",
               (double)total_l2_css.misses / (double)total_l2_css.accesses);
      printf("L2_total_cache_pending_hits = %llu\n", total_l2_css.pending_hits);
      printf("L2_total_cache_reservation_fails = %llu\n",
             total_l2_css.res_fails);
      printf("L2_total_cache_breakdown:\n");
      l2_stats.print_stats(stdout, "L2_cache_stats_breakdown");
      printf("L2_total_cache_reservation_fail_breakdown:\n");
      l2_stats.print_fail_stats(stdout, "L2_cache_stats_fail_breakdown");
      total_l2_css.print_port_stats(stdout, "L2_cache");
    }
  }

  if (m_config.gpgpu_cflog_interval != 0) {
    spill_log_to_file(stdout, 1, gpu_sim_cycle);
    insn_warp_occ_print(stdout);
  }
  if (gpgpu_ctx->func_sim->gpgpu_ptx_instruction_classification) {
    StatDisp(gpgpu_ctx->func_sim->g_inst_classification_stat
                 [gpgpu_ctx->func_sim->g_ptx_kernel_count]);
    StatDisp(gpgpu_ctx->func_sim->g_inst_op_classification_stat
                 [gpgpu_ctx->func_sim->g_ptx_kernel_count]);
  }

#ifdef GPGPUSIM_POWER_MODEL
  if (m_config.g_power_simulation_enabled) {
    m_gpgpusim_wrapper->detect_print_steady_state(
        1, gpu_tot_sim_insn + gpu_sim_insn);
  }
#endif

  // Interconnect power stat print
  long total_simt_to_mem = 0;
  long total_mem_to_simt = 0;
  long temp_stm = 0;
  long temp_mts = 0;
  for (unsigned i = 0; i < m_config.num_cluster(); i++) {
    m_cluster[i]->get_icnt_stats(temp_stm, temp_mts);
    total_simt_to_mem += temp_stm;
    total_mem_to_simt += temp_mts;
  }
  printf("\nicnt_total_pkts_mem_to_simt=%ld\n", total_mem_to_simt);
  printf("icnt_total_pkts_simt_to_mem=%ld\n", total_simt_to_mem);

  time_vector_print();
  fflush(stdout);

  clear_executed_kernel_info();
}

// performance counter that are not local to one shader
unsigned gpgpu_sim::threads_per_core() const {
  return m_shader_config->n_thread_per_shader;
}

void shader_core_ctx::mem_instruction_stats(const warp_inst_t &inst) {
  unsigned active_count = inst.active_count();
  // this breaks some encapsulation: the is_[space] functions, if you change
  // those, change this.
  switch (inst.space.get_type()) {
    case undefined_space:
    case reg_space:
      break;
    case shared_space:
      m_stats->gpgpu_n_shmem_insn += active_count;
      break;
    case sstarr_space:
      m_stats->gpgpu_n_sstarr_insn += active_count;
      break;
    case const_space:
      m_stats->gpgpu_n_const_insn += active_count;
      break;
    case param_space_kernel:
    case param_space_local:
      m_stats->gpgpu_n_param_insn += active_count;
      break;
    case tex_space:
      m_stats->gpgpu_n_tex_insn += active_count;
      break;
    case global_space:
    case local_space:
      if (inst.is_store())
        m_stats->gpgpu_n_store_insn += active_count;
      else
        m_stats->gpgpu_n_load_insn += active_count;
      break;
    default:
      abort();
  }
}
bool shader_core_ctx::can_issue_1block(kernel_info_t &kernel) {
  // Jin: concurrent kernels on one SM
  if (m_config->gpgpu_concurrent_kernel_sm) {
    //printf("occupy_threads = %d\n", m_occupied_n_threads);
    if (m_config->max_cta(kernel) < 1) return false;

    return occupy_shader_resource_1block(kernel, false);
  } else {
    return (get_n_active_cta() < m_config->max_cta(kernel));
  }
}

int shader_core_ctx::find_available_hwtid(unsigned int cta_size, bool occupy) {
  unsigned int step;
  for (step = 0; step < m_config->n_thread_per_shader; step += cta_size) {
    unsigned int hw_tid;
    for (hw_tid = step; hw_tid < step + cta_size; hw_tid++) {
      if (m_occupied_hwtid.test(hw_tid)) break;
    }
    if (hw_tid == step + cta_size)  // consecutive non-active
      break;
  }
  if (step >= m_config->n_thread_per_shader)  // didn't find
    return -1;
  else {
    if (occupy) {
      for (unsigned hw_tid = step; hw_tid < step + cta_size; hw_tid++)
        m_occupied_hwtid.set(hw_tid);
    }
    return step;
  }
}

bool shader_core_ctx::occupy_shader_resource_1block(kernel_info_t &k,
                                                    bool occupy) {
  unsigned threads_per_cta = k.threads_per_cta();
  const class function_info *kernel = k.entry();
  unsigned int padded_cta_size = threads_per_cta;
  unsigned int warp_size = m_config->warp_size;
  if (padded_cta_size % warp_size)
    padded_cta_size = ((padded_cta_size / warp_size) + 1) * (warp_size);

  if (m_occupied_n_threads + padded_cta_size > m_config->n_thread_per_shader)
    return false;

  if (find_available_hwtid(padded_cta_size, false) == -1) return false;

  const struct gpgpu_ptx_sim_info *kernel_info = ptx_sim_kernel_info(kernel);

  if (m_occupied_shmem + kernel_info->smem > m_config->gpgpu_shmem_size)
    return false;

  unsigned int used_regs = padded_cta_size * ((kernel_info->regs + 3) & ~3);
  if (m_occupied_regs + used_regs > m_config->gpgpu_shader_registers)
    return false;

  if (m_occupied_ctas + 1 > m_config->max_cta_per_core) return false;

  if (occupy) {
    m_occupied_n_threads += padded_cta_size;
    m_occupied_shmem += kernel_info->smem;
    m_occupied_regs += (padded_cta_size * ((kernel_info->regs + 3) & ~3));
    m_occupied_ctas++;

    SHADER_DPRINTF(LIVENESS,
                   "GPGPU-Sim uArch: Occupied %u threads, %u shared mem, %u "
                   "registers, %u ctas\n",
                   m_occupied_n_threads, m_occupied_shmem, m_occupied_regs,
                   m_occupied_ctas);
  }

  return true;
}

void shader_core_ctx::release_shader_resource_1block(unsigned hw_ctaid,
                                                     kernel_info_t &k) {
  if (m_config->gpgpu_concurrent_kernel_sm) {
    unsigned threads_per_cta = k.threads_per_cta();
    const class function_info *kernel = k.entry();
    unsigned int padded_cta_size = threads_per_cta;
    unsigned int warp_size = m_config->warp_size;
    if (padded_cta_size % warp_size)
      padded_cta_size = ((padded_cta_size / warp_size) + 1) * (warp_size);

    assert(m_occupied_n_threads >= padded_cta_size);
    m_occupied_n_threads -= padded_cta_size;

    int start_thread = m_occupied_cta_to_hwtid[hw_ctaid];

    for (unsigned hwtid = start_thread; hwtid < start_thread + padded_cta_size;
         hwtid++)
      m_occupied_hwtid.reset(hwtid);
    m_occupied_cta_to_hwtid.erase(hw_ctaid);

    const struct gpgpu_ptx_sim_info *kernel_info = ptx_sim_kernel_info(kernel);

    assert(m_occupied_shmem >= (unsigned int)kernel_info->smem);
    m_occupied_shmem -= kernel_info->smem;

    unsigned int used_regs = padded_cta_size * ((kernel_info->regs + 3) & ~3);
    assert(m_occupied_regs >= used_regs);
    m_occupied_regs -= used_regs;

    assert(m_occupied_ctas >= 1);
    m_occupied_ctas--;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Launches a cooperative thread array (CTA).
 *
 * @param kernel
 *    object that tells us which kernel to ask for a CTA from
 */

void shader_core_ctx::issue_block2core(kernel_info_t &kernel) {
  
  if (!m_config->gpgpu_concurrent_kernel_sm)
    set_max_cta(kernel);
  else 
    assert(occupy_shader_resource_1block(kernel, true));
  
  kernel.inc_running();

  // find a free CTA context
  unsigned free_cta_hw_id = (unsigned)-1;

  unsigned max_cta_per_core;
  if (!m_config->gpgpu_concurrent_kernel_sm)
    max_cta_per_core = kernel_max_cta_per_shader;
  else
    max_cta_per_core = m_config->max_cta_per_core;
  for (unsigned i = 0; i < max_cta_per_core; i++) {
    if (m_cta_status[i] == 0) {
      free_cta_hw_id = i;
      break;
    }
  }
  assert(free_cta_hw_id != (unsigned)-1);

  // determine hardware threads and warps that will be used for this CTA
  int cta_size = kernel.threads_per_cta();

  // hw warp id = hw thread id mod warp size, so we need to find a range
  // of hardware thread ids corresponding to an integral number of hardware
  // thread ids
  int padded_cta_size = cta_size;
  if (cta_size % m_config->warp_size)
    padded_cta_size =
        ((cta_size / m_config->warp_size) + 1) * (m_config->warp_size);

  unsigned int start_thread, end_thread;

  if (!m_config->gpgpu_concurrent_kernel_sm) {
    start_thread = free_cta_hw_id * padded_cta_size;
    end_thread = start_thread + cta_size;
  } else {
    start_thread = find_available_hwtid(padded_cta_size, true);
    assert((int)start_thread != -1);
    end_thread = start_thread + cta_size;
    assert(m_occupied_cta_to_hwtid.find(free_cta_hw_id) ==
           m_occupied_cta_to_hwtid.end());
    m_occupied_cta_to_hwtid[free_cta_hw_id] = start_thread;
  }

  // reset the microarchitecture state of the selected hardware thread and warp
  // contexts
  reinit(start_thread, end_thread, false);

  // initalize scalar threads and determine which hardware warps they are
  // allocated to bind functional simulation state of threads to hardware
  // resources (simulation)
  warp_set_t warps;
  unsigned nthreads_in_block = 0;
  function_info *kernel_func_info = kernel.entry();
  symbol_table *symtab = kernel_func_info->get_symtab();
  unsigned ctaid = kernel.get_next_cta_id_single();
  checkpoint *g_checkpoint = new checkpoint();
  for (unsigned i = start_thread; i < end_thread; i++) {
    m_threadState[i].m_cta_id = free_cta_hw_id;
    unsigned warp_id = i / m_config->warp_size;
    nthreads_in_block += ptx_sim_init_thread(
        kernel, &m_thread[i], m_sid, i, cta_size - (i - start_thread),
        m_config->n_thread_per_shader, this, free_cta_hw_id, warp_id,
        m_cluster->get_gpu());
    m_threadState[i].m_active = true;
    // load thread local memory and register file
    if (m_gpu->resume_option == 1 && kernel.get_uid() == m_gpu->resume_kernel &&
        ctaid >= m_gpu->resume_CTA && ctaid < m_gpu->checkpoint_CTA_t) {
      char fname[2048];
      snprintf(fname, 2048, "checkpoint_files/thread_%d_%d_reg.txt",
               i % cta_size, ctaid);
      m_thread[i]->resume_reg_thread(fname, symtab);
      char f1name[2048];
      snprintf(f1name, 2048, "checkpoint_files/local_mem_thread_%d_%d_reg.txt",
               i % cta_size, ctaid);
      g_checkpoint->load_global_mem(m_thread[i]->m_local_mem, f1name);
    }
    //
    warps.set(warp_id);
  }
  assert(nthreads_in_block > 0 &&
         nthreads_in_block <=
             m_config->n_thread_per_shader);  // should be at least one, but
                                              // less than max
  m_cta_status[free_cta_hw_id] = nthreads_in_block;
  //Nico: annotate start execution cycle of cta
  cta_start_cycle[free_cta_hw_id] = m_gpu->gpu_sim_cycle;
  if (m_gpu->resume_option == 1 && kernel.get_uid() == m_gpu->resume_kernel &&
      ctaid >= m_gpu->resume_CTA && ctaid < m_gpu->checkpoint_CTA_t) {
    char f1name[2048];
    snprintf(f1name, 2048, "checkpoint_files/shared_mem_%d.txt", ctaid);

    g_checkpoint->load_global_mem(m_thread[start_thread]->m_shared_mem, f1name);
  }
  // now that we know which warps are used in this CTA, we can allocate
  // resources for use in CTA-wide barrier operations
  m_barriers.allocate_barrier(free_cta_hw_id, warps);

  // initialize the SIMT stacks and fetch hardware
  init_warps(free_cta_hw_id, start_thread, end_thread, ctaid, cta_size,
             kernel.get_uid());
  m_n_active_cta++;

  shader_CTA_count_log(m_sid, 1);
  SHADER_DPRINTF(LIVENESS,
                 "GPGPU-Sim uArch: cta:%2u, start_tid:%4u, end_tid:%4u, "
                 "initialized @(%lld,%lld)\n",
                 free_cta_hw_id, start_thread, end_thread, m_gpu->gpu_sim_cycle,
                 m_gpu->gpu_tot_sim_cycle);
}

///////////////////////////////////////////////////////////////////////////////////////////

void dram_t::dram_log(int task) {
  if (task == SAMPLELOG) {
    StatAddSample(mrqq_Dist, que_length());
  } else if (task == DUMPLOG) {
    printf("Queue Length DRAM[%d] ", id);
    StatDisp(mrqq_Dist);
  }
}

// Find next clock domain and increment its time
int gpgpu_sim::next_clock_domain(void) {
  double smallest = min3(core_time, icnt_time, dram_time);
  int mask = 0x00;
  if (l2_time <= smallest) {
    smallest = l2_time;
    mask |= L2;
    l2_time += m_config.l2_period;
  }
  if (icnt_time <= smallest) {
    mask |= ICNT;
    icnt_time += m_config.icnt_period;
  }
  if (dram_time <= smallest) {
    mask |= DRAM;
    dram_time += m_config.dram_period;
  }
  if (core_time <= smallest) {
    mask |= CORE;
    core_time += m_config.core_period;
  }
  return mask;
} 

//Nico: function to set max cta per core when smk is on
void gpgpu_sim::smk_max_cta_per_core() {

  kernel_info_t *kernel1 = NULL, *kernel2 = NULL;

  int cont=0;
  for (unsigned k=0; k<get_num_running_kernels(); k++){
    if (m_running_kernels[k] != NULL && cont <2) {
      if (cont == 0)
        kernel1 = m_running_kernels[k];
      else
        kernel2 = m_running_kernels[k];
      cont++;
    }
  }

  if (cont == 0) // In no active kernels, return
    return;

  assert (cont >0 && cont < 3); // Just one or two kernels

  if (kernel1 != NULL) { // Nico: voy a chequear el id, asuminedo que solo dos kernels se están coejecutando
    if (kernel1->get_uid() == 1) { // Nico: Pongo esto porque con VA/HS se termina el VA y el HS para a ser el primer kernel con id 2
      unsigned int k1_ctas = m_config.gpu_smk_mctas_kernel1; // Config file: ctas per cluster
      unsigned int mcta1 = m_shader_config[0].max_cta(*kernel1); // max number of ctas per core
      //if (mcta1 *  m_shader_config->n_simt_cores_per_cluster < k1_ctas)

      assert (mcta1 *  m_shader_config->n_simt_cores_per_cluster >= k1_ctas);

    //kernel1->max_ctas_per_core =new unsigned int[m_shader_config->n_simt_cores_per_cluster](); // Memory allocation 
    for (unsigned int c=0; c < m_shader_config->n_simt_cores_per_cluster; c++) // Calculate number of ctas per core
      kernel1->max_ctas_per_core[c] = k1_ctas / m_shader_config->n_simt_cores_per_cluster;
    for (unsigned int c=0; c < k1_ctas % m_shader_config->n_simt_cores_per_cluster; c++)
      kernel1->max_ctas_per_core[c]++;
    }
    else printf("Raro: No debería haber entrado aquí porque han terminado uno de los kernels en coejecución\n");
  }

  // Nico: Si se comenta lo de abajo, no se ejecutan ctas de kernel2
  if (kernel2 != NULL) {  
    m_shader_config[0].smk_max_cta(*kernel1, *kernel2);
  }

  // Nico: Esto lo he utilizado para sacar valores de ejecución solo con un número de CTAs per SM dado. LA simulación acaba a los 100 ciclos.
  /*if (gpu_tot_sim_cycle + gpu_sim_cycle >= 100000  && kernel1 != NULL) {
    unsigned long long exec_cycles = gpu_tot_sim_cycle + gpu_sim_cycle;
    unsigned long long total_accesses1 = m_memory_stats->total_kernel_accesses[kernel1->get_uid()];
    unsigned long long ins1 = gpu_sim_insn_per_kernel[kernel1->get_uid()];
    printf("Solo, %s, %d, %f\n",  kernel1->name().c_str(), kernel1->max_ctas_per_core[0]+kernel1->max_ctas_per_core[1], (double)ins1/(double(exec_cycles)));
    exit(0);
  }*/
}


//Nico: Single execution of two kernels using max number of CTAs
//Nico: When fist kernel obtains stable cpi, second kernel is launched
void gpgpu_sim::single_execution_two_kernels() {

  //Nico: calculate the number of concurrent kernels
  unsigned int cont=0;
  double ipc1, ipc2;
  kernel_info_t *kernel1=NULL, *kernel2=NULL;
  for (unsigned k=0; k<get_num_running_kernels(); k++)
    if (m_running_kernels[k] != NULL){
      if (cont == 0)
        kernel1 = m_running_kernels[k] ;
       if (cont == 1)
        kernel2 = m_running_kernels[k] ; 
      cont++;
  }

  if (kernel1 != NULL) {
    if (kernel1->status == kernel_info_t::t_Kernel_Status::INIT) { // First kernel start
      unsigned int mcta1 = m_shader_config[0].max_cta(*kernel1); // Max number of CTAs per shader
      for (unsigned int c=0; c < m_shader_config->n_simt_cores_per_cluster; c++)
       kernel1->max_ctas_per_core[c] = mcta1;
      kernel1->status = kernel_info_t::t_Kernel_Status::READY;
      last_sampl_cycle =  gpu_tot_sim_cycle + gpu_sim_cycle;
      gpu_sim_start_kernel_inst[kernel1->get_uid()] = last_sampl_cycle;
      prev_conf_perf.ipc1 = 0.001;
    }

    if (kernel1->status == kernel_info_t::t_Kernel_Status::READY) { // Get samples
      /*if (gpu_tot_sim_cycle + gpu_sim_cycle -  last_sampl_cycle >= 10000){
        last_sampl_cycle = gpu_tot_sim_cycle + gpu_sim_cycle;
        ipc1 = (double) (gpu_tot_sim_insn_per_kernel[kernel1->get_uid()] + gpu_sim_insn_per_kernel[kernel1->get_uid()] - gpu_sim_start_kernel_inst[kernel1->get_uid()])/
	  	  (double)(gpu_tot_sim_cycle + gpu_sim_cycle-gpu_sim_start_kernel_cycle[kernel1->get_uid()]);
        printf("%d, %f, %lld\n", kernel1->get_uid(), ipc1, gpu_tot_sim_cycle + gpu_sim_cycle);

        if (prev_conf_perf.ipc1/ipc1 > 0.97 && prev_conf_perf.ipc1/ipc1 < 1.03) {
          kernel1->status = kernel_info_t::t_Kernel_Status::EVICTED;
          for (unsigned int c=0; c < m_shader_config->n_simt_cores_per_cluster; c++)
            kernel1->max_ctas_per_core[c] = 0;
        }
        prev_conf_perf.ipc1 = ipc1;
      }*/
      if (gpu_tot_sim_cycle + gpu_sim_cycle % 1000 == 0)
        printf ("*, %d, %lld, %lld\n", kernel1->get_uid(), gpu_tot_sim_insn_per_kernel[kernel1->get_uid()] + gpu_sim_insn_per_kernel[kernel1->get_uid()], gpu_tot_sim_cycle + gpu_sim_cycle-gpu_sim_start_kernel_cycle[kernel1->get_uid()]);
      if (gpu_tot_sim_cycle + gpu_sim_cycle > 100000)
        exit(0);
    }
  }

  if (kernel2!=NULL) {
    if (kernel2->status == kernel_info_t::t_Kernel_Status::READY) { // Get samples
      if (kernel1->num_excedded_ctas==0) { //If all CTAs of previous kernel have finished
        if (gpu_tot_sim_cycle + gpu_sim_cycle -  last_sampl_cycle >= 10000){
          last_sampl_cycle = gpu_tot_sim_cycle + gpu_sim_cycle;
          ipc2 = (double) (gpu_tot_sim_insn_per_kernel[kernel2->get_uid()] + gpu_sim_insn_per_kernel[kernel2->get_uid()] - gpu_sim_start_kernel_inst[kernel2->get_uid()])/
	  	    (double)(gpu_tot_sim_cycle + gpu_sim_cycle-gpu_sim_start_kernel_cycle[kernel2->get_uid()]);
          printf("%d, %f, =%lld \n", kernel2->get_uid(), ipc2, gpu_tot_sim_cycle + gpu_sim_cycle);
          if (prev_conf_perf.ipc2/ipc2 > 0.97 && prev_conf_perf.ipc2/ipc2 < 1.03) {
            kernel1->status = kernel_info_t::t_Kernel_Status::EVICTED;
            exit(0);
          }
          prev_conf_perf.ipc2 = ipc2;
        }
        if (gpu_tot_sim_cycle + gpu_sim_cycle % 1000 == 0)
          printf("*, %d, %lld, %lld\n", kernel2->get_uid(), gpu_tot_sim_insn_per_kernel[kernel2->get_uid()] + gpu_sim_insn_per_kernel[kernel2->get_uid()], gpu_tot_sim_cycle + gpu_sim_cycle-gpu_sim_start_kernel_cycle[kernel2->get_uid()]);
      }
      else 
        if ((gpu_tot_sim_cycle + gpu_sim_cycle) % 2000 ==0 )
          printf("Exceeded=%d, cycle=%lld\n", kernel1->num_excedded_ctas, gpu_tot_sim_cycle + gpu_sim_cycle);
    }
  }

  if (kernel2 != NULL) {
    if (kernel1->status == kernel_info_t::t_Kernel_Status::EVICTED) {// If kernel1 has been evicted
      if (kernel2->status == kernel_info_t::t_Kernel_Status::INIT)  {
        unsigned int mcta2 = m_shader_config[0].max_cta(*kernel2); // Max number of CTAs per shader
        for (unsigned int c=0; c < m_shader_config->n_simt_cores_per_cluster; c++) 
          kernel2->max_ctas_per_core[c] = mcta2;
        kernel2->status = kernel_info_t::t_Kernel_Status::READY;
        last_sampl_cycle = gpu_tot_sim_cycle + gpu_sim_cycle;
        gpu_sim_start_kernel_cycle[kernel2->get_uid()] = last_sampl_cycle;
        prev_conf_perf.ipc2=0.001;
      }
    }
    else { // No CTAs of kernel2 are going to be launched
      for (unsigned int c=0; c < m_shader_config->n_simt_cores_per_cluster; c++)
          kernel2->max_ctas_per_core[c] = 0;
    }
  }

}

void gpgpu_sim::flexsched_smk() {

int confs[64];
memset(confs, 0, 64*sizeof(int));

//int max_ctas1_per_core = 8; // VA
//int max_ctas1_inc = 2;
//confs[2]=14; confs[4]=12; confs[6]=10; confs[8]=8; confs[10]=6; confs[12]=4; confs[14]=2; //VA-GCEDD y VA-MM 
//confs[2]=10; confs[4]=8; confs[6]=8; confs[8]=8; confs[10]=6; confs[12]=4; // VA-HS 

//int max_ctas1_per_core = 16;
//int max_ctas1_inc = 2;
//confs[2]=14; confs[4]=14; confs[6]=12; confs[8]=12; confs[10]=10; confs[12]=10; confs[14]=8; confs[16]=8; confs[18]=6; confs[20]=6; confs[22]=4; confs[24]=4; confs[26]=2; confs[28]=2;// BS-GCEDD y BS-MM 
//confs[2]=10; confs[4]=8; confs[6]=8; confs[8]=8; confs[10]=6; confs[12]=6; confs[14]=6; confs[16]=6; confs[18]=4; confs[20]=4; confs[22]=4; confs[24]=2; confs[26]=2; confs[28]=2;// BS-HS 

int max_ctas1_per_core = 32;
int max_ctas1_inc = 8;
//confs[8]=14; confs[16]=12; confs[24]=10; confs[32]=8; confs[40]=6; confs[48]=4; confs[56]=2; // RCONV-GCEDD y RCONV-MM 
confs[4]=10; confs[8]=8; confs[12]=8; confs[16]=8; confs[20]=6; confs[24]=6; confs[28]=6; confs[32]=4; confs[36]=4; confs[40]=4; confs[44]=2; confs[44]=2; // RCONV-HS 


 t_perf_conf prev_perf, curr_perf;
 kernel_info_t *kernel[2];
 kernel[0] = NULL; kernel[1]=NULL;

  //Nico: calculate the number of concurrent kernels
  unsigned int cont=0;
  for (unsigned k=0; k<get_num_running_kernels(); k++)
    if (m_running_kernels[k] != NULL && cont <2){
      kernel[cont] = m_running_kernels[k];
      cont++;
    }

  if (kernel[0] !=NULL){
    if (kernel[0]->status == kernel_info_t::t_Kernel_Status::INIT){
      int q = m_config.gpu_smk_mctas_kernel1 / m_shader_config->n_simt_cores_per_cluster;
      int r = m_config.gpu_smk_mctas_kernel1 % m_shader_config->n_simt_cores_per_cluster;
      for (unsigned int c=0; c < m_shader_config->n_simt_cores_per_cluster; c++)
          kernel[0]->max_ctas_per_core[c] = q + ((c<r) ? 1 : 0); 
      kernel[0]->status = kernel_info_t::t_Kernel_Status::READY;
      memset (&prev_conf_perf, 0, sizeof(t_perf_conf));
      memset (&curr_conf_perf, 0, sizeof(t_perf_conf));
    }
  }
  else
    return;

  if (kernel[1] != NULL) {
    if (kernel[1]->status == kernel_info_t::t_Kernel_Status::INIT &&  kernel[0]->status == kernel_info_t::t_Kernel_Status::READY){
      int q = confs[m_config.gpu_smk_mctas_kernel1] / m_shader_config->n_simt_cores_per_cluster;
      int r = confs[m_config.gpu_smk_mctas_kernel1] % m_shader_config->n_simt_cores_per_cluster;
      for (unsigned int c=0; c < m_shader_config->n_simt_cores_per_cluster; c++) 
          //kernel[1]->max_ctas_per_core[c] = 8 -  kernel[0]->max_ctas_per_core[c];
          kernel[1]->max_ctas_per_core[c] = q + ((c<r) ? 1 : 0);
      kernel[1]->status = kernel_info_t::t_Kernel_Status::READY;
      last_sampl_cycle = gpu_tot_sim_cycle + gpu_sim_cycle;
      perf_sampl_active = false;
      max_search = 0; // initial_forward WS search
    }
  }
  else
    return;

  if (kernel[0]->num_excedded_ctas != 0 ||  kernel[1]->num_excedded_ctas != 0)
    if ((gpu_tot_sim_cycle + gpu_sim_cycle) % 2000 == 0)
      printf("Excedded= %d, %d\n", kernel[0]->num_excedded_ctas,  kernel[1]->num_excedded_ctas );

  if (kernel[0]->status == kernel_info_t::t_Kernel_Status::READY && kernel[1]->status == kernel_info_t::t_Kernel_Status::READY) {

    // Wait 10000 cycles to get stable ipc, only take samples when correct number of ctas are running
    if (gpu_tot_sim_cycle + gpu_sim_cycle < last_sampl_cycle + 10000 || kernel[0]->num_excedded_ctas != 0 ||  kernel[1]->num_excedded_ctas != 0)
      return;
    else {
      // Intial values ot the sampling interval
      if (gpu_tot_sim_cycle + gpu_sim_cycle >= last_sampl_cycle + 30000 && perf_sampl_active == false) {
        gpu_sim_start_kernel_cycle [kernel[0]->get_uid()] = gpu_sim_cycle+gpu_tot_sim_cycle; // Anotatte co-execution start cycle
        gpu_sim_start_kernel_cycle [kernel[1]->get_uid()] = gpu_sim_cycle+gpu_tot_sim_cycle; // Anotatte co-execution start cycle
        gpu_sim_start_kernel_inst[kernel[0]->get_uid()] = gpu_tot_sim_insn_per_kernel[kernel[0]->get_uid()] + gpu_sim_insn_per_kernel[kernel[0]->get_uid()];
        gpu_sim_start_kernel_inst[kernel[1]->get_uid()] = gpu_tot_sim_insn_per_kernel[kernel[1]->get_uid()] + gpu_sim_insn_per_kernel[kernel[1]->get_uid()];
        last_sampl_cycle = gpu_tot_sim_cycle + gpu_sim_cycle;
        perf_sampl_active = true; 
      }

      // Interval sampling interval: 20000 cycles
      if (gpu_tot_sim_cycle + gpu_sim_cycle >= last_sampl_cycle + 10000 && perf_sampl_active == true) {
        save_configuration_performance(&curr_conf_perf, kernel[0], kernel[1]);
        printf("Sample: %lld, %d, %d, %f, %f \n", gpu_tot_sim_cycle + gpu_sim_cycle, curr_conf_perf.num_ctas1, curr_conf_perf.num_ctas2, curr_conf_perf.ipc1, curr_conf_perf.ipc2);
        last_sampl_cycle = gpu_tot_sim_cycle + gpu_sim_cycle;

        // Calculate weigthed speedup
        int inc_ctas1;
        if (prev_conf_perf.ipc1 !=0 && prev_conf_perf.ipc2 !=0) { // If prev_sample_perf has a valid value (first sample cannot be compared with a previous one)
          double ws = (curr_conf_perf.ipc1/prev_conf_perf.ipc1 + curr_conf_perf.ipc2/prev_conf_perf.ipc2)/ 2.0 ;
          printf("WS=%f prev_ipc1=%f prev_ipc2=%f curr_ipc1=%f curr_ipc2=%f\n", ws, prev_conf_perf.ipc1, prev_conf_perf.ipc2, curr_conf_perf.ipc1, curr_conf_perf.ipc2);
          if (ws > 1) {
            
            if (max_search == 0) {  // Is initial configuration
              inc_ctas1 = max_ctas1_inc; // Add two k1 ctas per shader
              max_search = 2; // forward search
            }
            
            if (max_search == 2){ // Forward 
                inc_ctas1 = max_ctas1_inc;
            }

            if (max_search == 3) { // Backward
              inc_ctas1 = -max_ctas1_inc;
            }

            memcpy (&prev_conf_perf, &curr_conf_perf, sizeof(t_perf_conf)); 

          }
          else {
            
            if (max_search != 0){ // If not iniital search 
              printf("Mejor configuración es %d %d\n", prev_conf_perf.num_ctas1, prev_conf_perf.num_ctas2);
              exit(0);
            }

            if (max_search == 0) { // First tried configuration (forward) must be change to backward
              inc_ctas1 = -2*max_ctas1_inc;
              max_search = 3; // backward search
            }
            
          }
        }
        else {
          inc_ctas1 = max_ctas1_inc; // For first configuration we try forwrd
          max_search = 0;
          memcpy (&prev_conf_perf, &curr_conf_perf, sizeof(t_perf_conf)); 
        }

        // change coexecution configuration
        int new_cta1_num = 0;
        for (unsigned int c=0; c < m_shader_config->n_simt_cores_per_cluster; c++) {
           kernel[0]->max_ctas_per_core[c] += (inc_ctas1/2);
           new_cta1_num += kernel[0]->max_ctas_per_core[c];
        }

        int new_cta2_num = confs[new_cta1_num];
        int q = new_cta2_num / m_shader_config->n_simt_cores_per_cluster;
        int r = new_cta2_num % m_shader_config->n_simt_cores_per_cluster;
        for (unsigned int c=0; c < m_shader_config->n_simt_cores_per_cluster; c++) 
          //kernel[1]->max_ctas_per_core[c] = 8 -  kernel[0]->max_ctas_per_core[c];
          kernel[1]->max_ctas_per_core[c] = q + ((c<r) ? 1 : 0);

        if (kernel[0]->max_ctas_per_core[0] > max_ctas1_per_core - 1 || kernel[0]->max_ctas_per_core[0] <1) { // No valid coexecution configuration
          printf("Last possible configuration %d %d\n", curr_conf_perf.num_ctas1, curr_conf_perf.num_ctas2);
          exit(0);
        }
          
        //for (unsigned int c=0; c < m_shader_config->n_simt_cores_per_cluster; c++) 
         // kernel[1]->max_ctas_per_core[c] = 8 -  kernel[0]->max_ctas_per_core[c]; 

        perf_sampl_active = false;
      }
    }
  }

}

//Nico: ctas per cores is chaning to find the best coexecution configuration 
void gpgpu_sim::dynamic_smk_max_cta_per_core() {

  kernel_info_t *kernel1 = NULL, *kernel2 = NULL;

  //Nico: calculate the number of concurrent kernels
  unsigned int cont=0;
  for (unsigned k=0; k<get_num_running_kernels(); k++)
    if (m_running_kernels[k] != NULL)
      cont++;

  //Nico: if only a kernel is running, get the maximun number of ctas per core
  if (cont == 1) {
    unsigned int k;
    for (k=0; k<get_num_running_kernels(); k++)
      if (m_running_kernels[k] != NULL)
      break;
    kernel1 = m_running_kernels[k];
    if (kernel1->status != kernel_info_t::t_Kernel_Status::READY) { // Only if k1 id not running yet
      if (kernel1->status == kernel_info_t::t_Kernel_Status::INIT){ // If it is a new kernel
        //kernel1->max_ctas_per_core =new unsigned int[m_shader_config->n_simt_cores_per_cluster](); // Memory allocation 
        kernel1->max_ctas_per_core[0]=0;
        kernel1->max_ctas_per_core[1]=0;
        kernel1->save_ipc = 0.0;
        gpu_sim_start_kernel_cycle [kernel1->get_uid()]= gpu_sim_cycle+gpu_tot_sim_cycle;
      }
      unsigned int mcta1 = m_shader_config[0].max_cta(*kernel1);
      for (unsigned int c=0; c < m_shader_config->n_simt_cores_per_cluster; c++)
        kernel1->max_ctas_per_core[c] = mcta1;
      kernel1->status = kernel_info_t::t_Kernel_Status::READY;
      /*if (kernel1->get_uid() == 2 ) {
        kernel1->max_ctas_per_core[0] = 1;
        kernel1->max_ctas_per_core[1] = 0;
      }*/
    }
  }

  if (cont == 2) {
    unsigned int k;
    for (k=0; k<get_num_running_kernels(); k++)
      if (m_running_kernels[k] != NULL)
      break;
    kernel1 = m_running_kernels[k];
    for (k=k+1; k<get_num_running_kernels(); k++)
      if (m_running_kernels[k] != NULL)
      break;
    kernel2 = m_running_kernels[k];
    
    // Nico: updating ctas per core asignement for kernel 1 and 2 
    if ((gpu_tot_sim_cycle + gpu_sim_cycle) -  last_sampl_cycle == perf_sampl_interval) {
      
      last_sampl_cycle = gpu_tot_sim_cycle + gpu_sim_cycle;

      if(kernel1->num_excedded_ctas != 0 || kernel2->num_excedded_ctas != 0) {
          perf_sampl_active = false; 
          printf("**Conf %d %d\n", kernel1->num_excedded_ctas, kernel2->num_excedded_ctas);
      }
      else
      {
         perf_sampl_active = true;
      }
      

      if (perf_sampl_active == true) {

        perf_sampl_interval = 5000;
        save_configuration_performance(&curr_conf_perf, kernel1, kernel2);
      
        if (prev_sampl_perf.ipc1 !=0 && prev_sampl_perf.ipc2 !=0) {

          double ws;
          if (prev_conf_perf.ipc1 != 0) // A previous configuration has been measusred
            ws = ((curr_conf_perf.ipc1/prev_conf_perf.ipc1) +  (curr_conf_perf.ipc2/prev_conf_perf.ipc2))/2.0;
          else
            ws = 0;

          // Accumulate RB accesses and hits
          unsigned long long rb_total_accesses1 = 0, rb_total_accesses2 = 0;
          unsigned long long rb_total_hits1 = 0, rb_total_hits2=0;
          for (int chip=0; chip < m_memory_config->m_n_mem; chip++) {
            rb_total_hits1 += m_memory_stats->row_buffer_hits[kernel1->get_uid()][chip];
            rb_total_accesses1 += m_memory_stats->row_buffer_access[kernel1->get_uid()][chip];
            rb_total_hits2 += m_memory_stats->row_buffer_hits[kernel2->get_uid()][chip];
            rb_total_accesses2 += m_memory_stats->row_buffer_access[kernel2->get_uid()][chip];
          }

          // Imax calculatio: maximum number of instructionsper cycle
          unsigned int ilp = 2; 
          unsigned int warpsize = m_config.m_shader_config.warp_size;
          double Imax =  ilp * warpsize * m_config.num_cluster() * m_config.m_shader_config.n_simt_cores_per_cluster;
          
          // Bmax according HSM paper (in GB/s)
          unsigned int request_size = 8*4; // Burst size -> eight words of 32 bits
          unsigned long long total_accesses1 = m_memory_stats->total_kernel_accesses[kernel1->get_uid()];
          unsigned long long total_accesses2 = m_memory_stats->total_kernel_accesses[kernel2->get_uid()];
          unsigned int sample_cycles = gpu_tot_sim_cycle + gpu_sim_cycle-gpu_sim_start_kernel_cycle[kernel1->get_uid()];
          double Bmax1 = ((double)Imax / curr_conf_perf.ipc1) * ((double)total_accesses1/(double)sample_cycles)  * (double)(request_size) * 1.417;
          double Bmax2 = ((double)Imax / curr_conf_perf.ipc2) * ((double)total_accesses2/(double)sample_cycles)  * (double)(request_size) * 1.417;
          
          printf("**Conf, %.2f, %d, %d, %lld, %.2f, %.2f, %.2f, %.2f, %lld, %lld, %d, %d, %lld, %lld, %f, %f, %f, %f\n", ws, curr_conf_perf.num_ctas1, curr_conf_perf.num_ctas2, gpu_tot_sim_cycle + gpu_sim_cycle, 
                curr_conf_perf.ipc1, curr_conf_perf.ipc2, prev_conf_perf.ipc1, prev_conf_perf.ipc2,  
                curr_conf_perf.ins1, curr_conf_perf.ins2, kernel1->num_excedded_ctas, kernel2->num_excedded_ctas, 
                rb_total_accesses1, rb_total_accesses2, 
                (double)rb_total_hits1/(double)rb_total_accesses1, (double)rb_total_hits2/(double)rb_total_accesses2, Bmax1, Bmax2);  

          /*if (fabs((curr_conf_perf.ipc1 - prev_sampl_perf.ipc1) / prev_sampl_perf.ipc1) < .05 &&
            fabs((curr_conf_perf.ipc2 - prev_sampl_perf.ipc2) / prev_sampl_perf.ipc2) <.05){*/
          if ((gpu_tot_sim_cycle + gpu_sim_cycle) >= 35000 * (kernel1->max_ctas_per_core[0]+kernel1->max_ctas_per_core[1]) ){
            if (kernel1->max_ctas_per_core[0] + kernel1->max_ctas_per_core[1] < 7) {
              perf_sampl_active = false; // When configuration is changed smpling rate is changed
              perf_sampl_interval = 5000;
              if (kernel1->max_ctas_per_core[0] == kernel1->max_ctas_per_core[1])
                kernel1->max_ctas_per_core[0]++;
              else
               kernel1->max_ctas_per_core[1]++;
              kernel2->status = kernel_info_t::t_Kernel_Status::RESCHEDULE; 
              prev_conf_perf.ipc1 = (double)(curr_conf_perf.ins1-prev_sampl_perf.ins1) / (double)perf_sampl_interval;
              prev_conf_perf.ipc2 = (double)(curr_conf_perf.ins2-prev_sampl_perf.ins2) / (double)perf_sampl_interval;
              //memcpy (&prev_conf_perf, &curr_conf_perf, sizeof(t_perf_conf));
            }
          }
        }  
        memcpy (&prev_sampl_perf, &curr_conf_perf, sizeof(t_perf_conf));
        //kernel2->status = kernel_info_t::t_Kernel_Status::RESCHEDULE; // Recalculate ctas per core for kernel 2
      }
      else{
        perf_sampl_interval = 5000;
        perf_sampl_active = true;
        kernel2->status = kernel_info_t::t_Kernel_Status::RESCHEDULE; // Recalculate ctas per core for kernel 2
      }
    }

    if (kernel1->status == kernel_info_t::t_Kernel_Status::INIT){ // If it is a new kernel
      //kernel1->max_ctas_per_core = new unsigned int[m_shader_config->n_simt_cores_per_cluster](); // Memory allocation 
      kernel1->max_ctas_per_core[0]=0;
      kernel1->max_ctas_per_core[1]=0;
      kernel1->save_ipc = 0.0;
    }
    if (kernel2->status == kernel_info_t::t_Kernel_Status::INIT){ // If it is a new kernel
      //kernel2->max_ctas_per_core = new unsigned int[m_shader_config->n_simt_cores_per_cluster](); // Memory allocation 
      kernel2->max_ctas_per_core[0]=0;
      kernel2->max_ctas_per_core[1]=0;
      kernel2->save_ipc = 0.0;
      kernel2->status = kernel_info_t::t_Kernel_Status::RESCHEDULE; // max ctas per coe must be calculated

      // Establish initial cta per core for kernel 1 just when kernel 2 is prepared to run
      kernel1->max_ctas_per_core[0]=1;
      kernel1->max_ctas_per_core[1]=0;
      
      // Initialize performance info
      memset(&prev_conf_perf, 0, sizeof(t_perf_conf));
      memset(&curr_conf_perf, 0, sizeof(t_perf_conf));
      memset(&prev_sampl_perf, 0, sizeof(t_perf_conf));

      // Initialize dram memory counters
      memset(m_memory_stats->row_buffer_access[kernel1->get_uid()], 0,  m_memory_config->m_n_mem*sizeof(unsigned long long));
      memset(m_memory_stats->row_buffer_hits[kernel1->get_uid()], 0,  m_memory_config->m_n_mem*sizeof(unsigned long long));
      memset(m_memory_stats->row_buffer_access[kernel2->get_uid()], 0,  m_memory_config->m_n_mem*sizeof(unsigned long long));
      memset(m_memory_stats->row_buffer_hits[kernel2->get_uid()], 0,  m_memory_config->m_n_mem*sizeof(unsigned long long));

      // performance sample
      perf_sampl_active = false; // Poner a false
      perf_sampl_interval = 2000; // Poner a 4000
    }
    
    if (kernel2->status == kernel_info_t::t_Kernel_Status::RESCHEDULE){ // only if k2 is not runing yet
      // for k1
      /*unsigned int k1_ctas = m_config.gpu_smk_mctas_kernel1; // Config file: ctas per cluster 
      for (unsigned int c=0; c < m_shader_config->n_simt_cores_per_cluster; c++) // Calculate number of ctas per core
        kernel1->max_ctas_per_core[c] = k1_ctas / m_shader_config->n_simt_cores_per_cluster;
      for (unsigned int c=0; c < k1_ctas % m_shader_config->n_simt_cores_per_cluster; c++)
        kernel1->max_ctas_per_core[c]++;*/
      gpu_sim_start_kernel_cycle [kernel1->get_uid()] = gpu_sim_cycle+gpu_tot_sim_cycle; // Anotatte co-execution start cycle
      gpu_sim_start_kernel_cycle [kernel2->get_uid()] = gpu_sim_cycle+gpu_tot_sim_cycle; // Anotatte co-execution start cycle
      gpu_sim_start_kernel_inst[kernel1->get_uid()] = gpu_tot_sim_insn_per_kernel[kernel1->get_uid()] + gpu_sim_insn_per_kernel[kernel1->get_uid()];
      gpu_sim_start_kernel_inst[kernel2->get_uid()] = gpu_tot_sim_insn_per_kernel[kernel2->get_uid()] + gpu_sim_insn_per_kernel[kernel2->get_uid()];

      kernel1->status =kernel_info_t::t_Kernel_Status::READY;
      // for k2
      //kernel2->max_ctas_per_core =new unsigned int[m_shader_config->n_simt_cores_per_cluster](); // Memory allocation 
      m_shader_config[0].smk_max_cta(*kernel1, *kernel2);
      kernel2->status =kernel_info_t::t_Kernel_Status::READY;
      memset(&prev_sampl_perf, 0, sizeof(t_perf_conf));

       // Initialize dram memory counters
      memset(m_memory_stats->row_buffer_access[kernel1->get_uid()], 0,  m_memory_config->m_n_mem*sizeof(unsigned long long));
      memset(m_memory_stats->row_buffer_hits[kernel1->get_uid()], 0,  m_memory_config->m_n_mem*sizeof(unsigned long long));
      memset(m_memory_stats->row_buffer_access[kernel2->get_uid()], 0,  m_memory_config->m_n_mem*sizeof(unsigned long long));
      memset(m_memory_stats->row_buffer_hits[kernel2->get_uid()], 0,  m_memory_config->m_n_mem*sizeof(unsigned long long));
    }
  }

/*
  for (unsigned k=0, cont=0; k<get_num_running_kernels(); k++){
    if (m_running_kernels[k] != NULL && cont <2) {
      if (cont == 0)
        kernel1 = m_running_kernels[k];
      else
        kernel2 = m_running_kernels[k];
      cont++;
    }
  }

  if (kernel1 != NULL && kernel1->is_scheduled == false) { 
    kernel1->is_scheduled= true;
    unsigned int k1_ctas = m_config.gpu_smk_mctas_kernel1; // Config file: ctas per cluster
    unsigned int mcta1 = m_shader_config[0].max_cta(*kernel1); // max number of ctas per core
    if (mcta1 *  m_shader_config->n_simt_cores_per_cluster < k1_ctas)
      assert (mcta1 *  m_shader_config->n_simt_cores_per_cluster >= k1_ctas);

    kernel1->max_ctas_per_core =new unsigned int[m_shader_config->n_simt_cores_per_cluster](); // Memory allocation 
    for (unsigned int c=0; c < m_shader_config->n_simt_cores_per_cluster; c++) // Calculate number of ctas per core
      kernel1->max_ctas_per_core[c] = k1_ctas / m_shader_config->n_simt_cores_per_cluster;
    for (unsigned int c=0; c < k1_ctas % m_shader_config->n_simt_cores_per_cluster; c++)
      kernel1->max_ctas_per_core[c]++;
  }

  if (kernel2 != NULL && kernel2->is_scheduled == false) { 
    kernel2->is_scheduled= true;
    kernel2->max_ctas_per_core =new unsigned int[m_shader_config->n_simt_cores_per_cluster](); // Memory allocation 
    m_shader_config[0].smk_max_cta(*kernel1, *kernel2);
  }*/
}

void gpgpu_sim::smk_reset_excedded_ctas() {

for (unsigned k=0; k< get_num_running_kernels(); k++ )
  if (m_running_kernels[k] != NULL) {
    /*if (m_running_kernels[k]->status == kernel_info_t::t_Kernel_Status::READY)*/ {
      m_running_kernels[k]->num_excedded_ctas = 0;
    }
  }      
}

//Nico: save IPCs of ready kernels
void gpgpu_sim::save_configuration_performance(t_perf_conf *conf, kernel_info_t *kernel1, kernel_info_t *kernel2) {

  conf->id1 = kernel1->get_uid();
  conf->id2 = kernel2->get_uid();

  conf->num_ctas1 = kernel1->max_ctas_per_core[0] + kernel1->max_ctas_per_core[1];
  conf->num_ctas2 = kernel2->max_ctas_per_core[0] + kernel2->max_ctas_per_core[1];

  conf->ins1 = gpu_tot_sim_insn_per_kernel[kernel1->get_uid()] + gpu_sim_insn_per_kernel[kernel1->get_uid()] - gpu_sim_start_kernel_inst[kernel1->get_uid()];
  conf->ins2 = gpu_tot_sim_insn_per_kernel[kernel2->get_uid()] + gpu_sim_insn_per_kernel[kernel2->get_uid()] - gpu_sim_start_kernel_inst[kernel2->get_uid()];

  conf->ipc1 = (double) (gpu_tot_sim_insn_per_kernel[kernel1->get_uid()] + gpu_sim_insn_per_kernel[kernel1->get_uid()] - gpu_sim_start_kernel_inst[kernel1->get_uid()])/
	  	(double)(gpu_tot_sim_cycle + gpu_sim_cycle-gpu_sim_start_kernel_cycle[kernel1->get_uid()]);

  conf->ipc2 = (double) (gpu_tot_sim_insn_per_kernel[kernel2->get_uid()] + gpu_sim_insn_per_kernel[kernel2->get_uid()]-gpu_sim_start_kernel_inst[kernel2->get_uid()])/
	  	(double)(gpu_tot_sim_cycle + gpu_sim_cycle-gpu_sim_start_kernel_cycle[kernel2->get_uid()]);

}

//Nico: weighted sppedup calculations
void gpgpu_sim::coexecution_performace() {

  kernel_info_t *kernels[2];
  unsigned num_conc_kernels = 0;
  // Nico: each 1000 cycles calculate ipc
  if ((gpu_tot_sim_cycle + gpu_sim_cycle) % 1000 == 0) {
    for (unsigned k=0; k< get_num_running_kernels(); k++ )
       if (m_running_kernels[k] != NULL) {
         if (m_running_kernels[k]->status ==  kernel_info_t::t_Kernel_Status::READY){
          assert (num_conc_kernels < 2); // No more than two corruning kernels
          kernels[num_conc_kernels] = m_running_kernels[k];
          num_conc_kernels++;
        }
      }
  }

  // Update
  if (num_conc_kernels== 2) {
    double current_ipc[2];
    for (unsigned k=0; k < num_conc_kernels; k++) {
      current_ipc[k] =  (double) (gpu_tot_sim_insn_per_kernel[kernels[k]->get_uid()] + gpu_sim_insn_per_kernel[kernels[k]->get_uid()])/
	  	(double)(gpu_tot_sim_cycle + gpu_sim_cycle-gpu_sim_start_kernel_cycle[kernels[k]->get_uid()]);
    }

    if (kernels[0]->save_ipc !=0 && kernels[1]->save_ipc != 0)
    {
      // Weighted speedup
      double wgs = (current_ipc[0]/kernels[0]->save_ipc + current_ipc[1]/kernels[1]->save_ipc)/2.0;
      printf("** wgs=%f **\n", wgs);
      prev_ws = wgs;
    }

    kernels[0]->save_ipc = current_ipc[0];
    kernels[1]->save_ipc = current_ipc[1];

  }
}
  
 
void gpgpu_sim::issue_block2core() {
 

  assert(!(m_config.gpu_smk_mctas_kernel1!=0 && m_config.gpu_smt_SMs_kernel1!=0)); // Only one cta distribution startegy can be selected simultaneously
 
  if (m_config.gpu_smk_mctas_kernel1!=0) { // Only for SMK: support for FlexSched
    smk_max_cta_per_core();  //Nico: Update ctas for kernel2 --> SMK coexecution dada la configuración
    // single_execution_two_kernels();
    //dynamic_smk_max_cta_per_core(); --> FlexSched priemra version
    //flexsched_smk();
    //smk_reset_excedded_ctas(); // Reset the number of excedded ctas of ready kernels
  }
  unsigned last_issued = m_last_cluster_issue; 
  for (unsigned i = 0; i < m_shader_config->n_simt_clusters; i++) {
    unsigned idx = (i + last_issued + 1) % m_shader_config->n_simt_clusters;
    unsigned num;
    if (m_config.gpu_smk_mctas_kernel1  == 0 && m_config.gpu_smt_SMs_kernel1 == 0){ // Sslecting CTA distribution scheme
      num = m_cluster[idx]->issue_block2core();
    }
    else { 
      if (m_config.gpu_smk_mctas_kernel1!=0) {
        num = m_cluster[idx]->issue_block2core_SMK();
      }
      else {
        num = m_cluster[idx]->issue_block2core_SMT();
        // Nico: Esto lo he utilizado para sacar valores de ejecución solo con un número de SMs dado. La simulación acaba a los 60000 ciclos.
        /*kernel_info_t *kernel1 = NULL;
        for (unsigned k=0; k<get_num_running_kernels(); k++){
          if (m_running_kernels[k] != NULL) {  
            kernel1 = m_running_kernels[k];
            break;
          }
        }*/
          
        /*if (gpu_tot_sim_cycle + gpu_sim_cycle >= 60000  && kernel1 != NULL) {
           unsigned long long exec_cycles = gpu_tot_sim_cycle + gpu_sim_cycle;
           unsigned long long total_accesses1 = m_memory_stats->total_kernel_accesses[kernel1->get_uid()];
           unsigned long long ins1 = gpu_sim_insn_per_kernel[kernel1->get_uid()];
          printf("Solo (SMT), %s, %d, %f\n",  kernel1->name().c_str(), m_config.gpu_smt_SMs_kernel1, (double)ins1/(double(exec_cycles)));
          exit(0);
        }*/
      }
    }
    if (num) {
      m_last_cluster_issue = idx;
      m_total_cta_launched += num;
    }
  } 
}

unsigned long long g_single_step =
    0;  // set this in gdb to single step the pipeline

void gpgpu_sim::cycle() {
  int clock_mask = next_clock_domain();

  if (clock_mask & CORE) {
    // shader core loading (pop from ICNT into core) follows CORE clock
    for (unsigned i = 0; i < m_shader_config->n_simt_clusters; i++)
      m_cluster[i]->icnt_cycle();
  }
  unsigned partiton_replys_in_parallel_per_cycle = 0;
  if (clock_mask & ICNT) {
    // pop from memory controller to interconnect
    for (unsigned i = 0; i < m_memory_config->m_n_mem_sub_partition; i++) {
      mem_fetch *mf = m_memory_sub_partition[i]->top();
      if (mf) {
        unsigned response_size =
            mf->get_is_write() ? mf->get_ctrl_size() : mf->size();
        if (::icnt_has_buffer(m_shader_config->mem2device(i), response_size)) {
          // if (!mf->get_is_write())
          mf->set_return_timestamp(gpu_sim_cycle + gpu_tot_sim_cycle);
          mf->set_status(IN_ICNT_TO_SHADER, gpu_sim_cycle + gpu_tot_sim_cycle);
          ::icnt_push(m_shader_config->mem2device(i), mf->get_tpc(), mf,
                      response_size);
          /*if (mf) {
            printf("Request_id = %d\n", mf->get_request_id());
            if (mf->get_request_id() == 365)
              printf("Aqui1\n");
          }*/
          m_memory_sub_partition[i]->pop();
          partiton_replys_in_parallel_per_cycle++;
        } else {
          gpu_stall_icnt2sh++;
        }
      } else {
        m_memory_sub_partition[i]->pop();
      }
    }
  }
  partiton_replys_in_parallel += partiton_replys_in_parallel_per_cycle;

  if (clock_mask & DRAM) {
    for (unsigned i = 0; i < m_memory_config->m_n_mem; i++) {
      if (m_memory_config->simple_dram_model)
        m_memory_partition_unit[i]->simple_dram_model_cycle();
      else
        m_memory_partition_unit[i]
            ->dram_cycle();  // Issue the dram command (scheduler + delay model)
      // Update performance counters for DRAM
      m_memory_partition_unit[i]->set_dram_power_stats(
          m_power_stats->pwr_mem_stat->n_cmd[CURRENT_STAT_IDX][i],
          m_power_stats->pwr_mem_stat->n_activity[CURRENT_STAT_IDX][i],
          m_power_stats->pwr_mem_stat->n_nop[CURRENT_STAT_IDX][i],
          m_power_stats->pwr_mem_stat->n_act[CURRENT_STAT_IDX][i],
          m_power_stats->pwr_mem_stat->n_pre[CURRENT_STAT_IDX][i],
          m_power_stats->pwr_mem_stat->n_rd[CURRENT_STAT_IDX][i],
          m_power_stats->pwr_mem_stat->n_wr[CURRENT_STAT_IDX][i],
          m_power_stats->pwr_mem_stat->n_req[CURRENT_STAT_IDX][i]);
    }
  }

  // L2 operations follow L2 clock domain
  unsigned partiton_reqs_in_parallel_per_cycle = 0;
  if (clock_mask & L2) {
    m_power_stats->pwr_mem_stat->l2_cache_stats[CURRENT_STAT_IDX].clear();
    for (unsigned i = 0; i < m_memory_config->m_n_mem_sub_partition; i++) {
      // move memory request from interconnect into memory partition (if not
      // backed up) Note:This needs to be called in DRAM clock domain if there
      // is no L2 cache in the system In the worst case, we may need to push
      // SECTOR_CHUNCK_SIZE requests, so ensure you have enough buffer for them
      if (m_memory_sub_partition[i]->full(SECTOR_CHUNCK_SIZE)) {
        gpu_stall_dramfull++;
      } else {
        mem_fetch *mf = (mem_fetch *)icnt_pop(m_shader_config->mem2device(i));
        //
       /* if (mf) { // Nico
            printf("Pop Request_id = %d\n", mf->get_request_id());
            if (mf->get_request_id() == 365)
              printf("Aqui1\n");
          }*/
        m_memory_sub_partition[i]->push(mf, gpu_sim_cycle + gpu_tot_sim_cycle);
        if (mf) partiton_reqs_in_parallel_per_cycle++;
      }
      m_memory_sub_partition[i]->cache_cycle(gpu_sim_cycle + gpu_tot_sim_cycle);
      m_memory_sub_partition[i]->accumulate_L2cache_stats(
          m_power_stats->pwr_mem_stat->l2_cache_stats[CURRENT_STAT_IDX]);
    }
  }
  partiton_reqs_in_parallel += partiton_reqs_in_parallel_per_cycle;
  if (partiton_reqs_in_parallel_per_cycle > 0) {
    partiton_reqs_in_parallel_util += partiton_reqs_in_parallel_per_cycle;
    gpu_sim_cycle_parition_util++;
  }

  if (clock_mask & ICNT) {
    icnt_transfer();
  }

  if (clock_mask & CORE) {
    // L1 cache + shader core pipeline stages
    m_power_stats->pwr_mem_stat->core_cache_stats[CURRENT_STAT_IDX].clear();
    for (unsigned i = 0; i < m_shader_config->n_simt_clusters; i++) {
      if (m_cluster[i]->get_not_completed() || get_more_cta_left()) {
        m_cluster[i]->core_cycle();
        *active_sms += m_cluster[i]->get_n_active_sms();
      }
      // Update core icnt/cache stats for GPUWattch
      m_cluster[i]->get_icnt_stats(
          m_power_stats->pwr_mem_stat->n_simt_to_mem[CURRENT_STAT_IDX][i],
          m_power_stats->pwr_mem_stat->n_mem_to_simt[CURRENT_STAT_IDX][i]);
      m_cluster[i]->get_cache_stats(
          m_power_stats->pwr_mem_stat->core_cache_stats[CURRENT_STAT_IDX]);
      m_cluster[i]->get_current_occupancy(
          gpu_occupancy.aggregate_warp_slot_filled,
          gpu_occupancy.aggregate_theoretical_warp_slots);
    }
    float temp = 0;
    for (unsigned i = 0; i < m_shader_config->num_shader(); i++) {
      temp += m_shader_stats->m_pipeline_duty_cycle[i];
    }
    temp = temp / m_shader_config->num_shader();
    *average_pipeline_duty_cycle = ((*average_pipeline_duty_cycle) + temp);
    // cout<<"Average pipeline duty cycle:
    // "<<*average_pipeline_duty_cycle<<endl;

    if (g_single_step &&
        ((gpu_sim_cycle + gpu_tot_sim_cycle) >= g_single_step)) {
      raise(SIGTRAP);  // Debug breakpoint
    }
    gpu_sim_cycle++;

    if (g_interactive_debugger_enabled) gpgpu_debug();

      // McPAT main cycle (interface with McPAT)
#ifdef GPGPUSIM_POWER_MODEL
    if (m_config.g_power_simulation_enabled) {
      mcpat_cycle(m_config, getShaderCoreConfig(), m_gpgpusim_wrapper,
                  m_power_stats, m_config.gpu_stat_sample_freq,
                  gpu_tot_sim_cycle, gpu_sim_cycle, gpu_tot_sim_insn,
                  gpu_sim_insn);
    }
#endif

    issue_block2core();

    // Depending on configuration, invalidate the caches once all of threads are
    // completed.
    int all_threads_complete = 1;
    if (m_config.gpgpu_flush_l1_cache) {
      for (unsigned i = 0; i < m_shader_config->n_simt_clusters; i++) {
        if (m_cluster[i]->get_not_completed() == 0)
          m_cluster[i]->cache_invalidate();
        else
          all_threads_complete = 0;
      }
    }

    if (m_config.gpgpu_flush_l2_cache) {
      if (!m_config.gpgpu_flush_l1_cache) {
        for (unsigned i = 0; i < m_shader_config->n_simt_clusters; i++) {
          if (m_cluster[i]->get_not_completed() != 0) {
            all_threads_complete = 0;
            break;
          }
        }
      }

      if (all_threads_complete && !m_memory_config->m_L2_config.disabled()) {
        printf("Flushed L2 caches...\n");
        if (m_memory_config->m_L2_config.get_num_lines()) {
          int dlc = 0;
          for (unsigned i = 0; i < m_memory_config->m_n_mem; i++) {
            dlc = m_memory_sub_partition[i]->flushL2();
            assert(dlc == 0);  // TODO: need to model actual writes to DRAM here
            printf("Dirty lines flushed from L2 %d is %d\n", i, dlc);
          }
        }
      }
    }

    if (!(gpu_sim_cycle % m_config.gpu_stat_sample_freq)) {
      time_t days, hrs, minutes, sec;
      time_t curr_time;
      time(&curr_time);
      unsigned long long elapsed_time =
          MAX(curr_time - gpgpu_ctx->the_gpgpusim->g_simulation_starttime, 1);
      if ((elapsed_time - last_liveness_message_time) >=
              m_config.liveness_message_freq &&
          DTRACE(LIVENESS)) {
        days = elapsed_time / (3600 * 24);
        hrs = elapsed_time / 3600 - 24 * days;
        minutes = elapsed_time / 60 - 60 * (hrs + 24 * days);
        sec = elapsed_time - 60 * (minutes + 60 * (hrs + 24 * days));

        unsigned long long active = 0, total = 0;
        for (unsigned i = 0; i < m_shader_config->n_simt_clusters; i++) {
          m_cluster[i]->get_current_occupancy(active, total);
        }
        DPRINTFG(LIVENESS,
                 "uArch: inst.: %lld (ipc=%4.1f, occ=%0.4f\% [%llu / %llu]) "
                 "sim_rate=%u (inst/sec) elapsed = %u:%u:%02u:%02u / %s",
                 gpu_tot_sim_insn + gpu_sim_insn,
                 (double)gpu_sim_insn / (double)gpu_sim_cycle,
                 float(active) / float(total) * 100, active, total,
                 (unsigned)((gpu_tot_sim_insn + gpu_sim_insn) / elapsed_time),
                 (unsigned)days, (unsigned)hrs, (unsigned)minutes,
                 (unsigned)sec, ctime(&curr_time));
        fflush(stdout);
        last_liveness_message_time = elapsed_time;
      }
	  visualizer_printstat();
      m_memory_stats->memlatstat_lat_pw();
      if (m_config.gpgpu_runtime_stat &&
          (m_config.gpu_runtime_stat_flag != 0)) {
        if (m_config.gpu_runtime_stat_flag & GPU_RSTAT_BW_STAT) {
          for (unsigned i = 0; i < m_memory_config->m_n_mem; i++)
            m_memory_partition_unit[i]->print_stat(stdout);
          printf("maxmrqlatency = %d \n", m_memory_stats->max_mrq_latency);
          printf("maxmflatency = %d \n", m_memory_stats->max_mf_latency);
        }
        if (m_config.gpu_runtime_stat_flag & GPU_RSTAT_SHD_INFO)
          shader_print_runtime_stat(stdout);
        if (m_config.gpu_runtime_stat_flag & GPU_RSTAT_L1MISS)
          shader_print_l1_miss_stat(stdout);
        if (m_config.gpu_runtime_stat_flag & GPU_RSTAT_SCHED)
          shader_print_scheduler_stat(stdout, false);
      }
    }

    if (!(gpu_sim_cycle % 50000)) {
      // deadlock detection
      if (m_config.gpu_deadlock_detect && gpu_sim_insn == last_gpu_sim_insn) {
        gpu_deadlock = true;
      } else {
        last_gpu_sim_insn = gpu_sim_insn;
      }
    }
    try_snap_shot(gpu_sim_cycle);
    spill_log_to_file(stdout, 0, gpu_sim_cycle);

#if (CUDART_VERSION >= 5000)
    // launch device kernel
    gpgpu_ctx->device_runtime->launch_one_device_kernel();
#endif
  }
}

void shader_core_ctx::dump_warp_state(FILE *fout) const {
  fprintf(fout, "\n");
  fprintf(fout, "per warp functional simulation status:\n");
  for (unsigned w = 0; w < m_config->max_warps_per_shader; w++)
    m_warp[w].print(fout);
}

void gpgpu_sim::perf_memcpy_to_gpu(size_t dst_start_addr, size_t count) {
  if (m_memory_config->m_perf_sim_memcpy) {
    assert(dst_start_addr % 32 == 0);

    for (unsigned counter = 0; counter < count; counter += 32) {
      const unsigned wr_addr = dst_start_addr + counter;
      addrdec_t raw_addr;
      mem_access_sector_mask_t mask;
      mask.set(wr_addr % 128 / 32);
      m_memory_config->m_address_mapping.addrdec_tlx(wr_addr, &raw_addr);
      const unsigned partition_id =
          raw_addr.sub_partition /
          m_memory_config->m_n_sub_partition_per_memory_channel;
      m_memory_partition_unit[partition_id]->handle_memcpy_to_gpu(
          wr_addr, raw_addr.sub_partition, mask);
    }
  }
}

void gpgpu_sim::dump_pipeline(int mask, int s, int m) const {
  /*
     You may want to use this function while running GPGPU-Sim in gdb.
     One way to do that is add the following to your .gdbinit file:

        define dp
           call g_the_gpu.dump_pipeline_impl((0x40|0x4|0x1),$arg0,0)
        end

     Then, typing "dp 3" will show the contents of the pipeline for shader
     core 3.
  */

  printf("Dumping pipeline state...\n");
  if (!mask) mask = 0xFFFFFFFF;
  for (unsigned i = 0; i < m_shader_config->n_simt_clusters; i++) {
    if (s != -1) {
      i = s;
    }
    if (mask & 1)
      m_cluster[m_shader_config->sid_to_cluster(i)]->display_pipeline(
          i, stdout, 1, mask & 0x2E);
    if (s != -1) {
      break;
    }
  }
  if (mask & 0x10000) {
    for (unsigned i = 0; i < m_memory_config->m_n_mem; i++) {
      if (m != -1) {
        i = m;
      }
      printf("DRAM / memory controller %u:\n", i);
      if (mask & 0x100000) m_memory_partition_unit[i]->print_stat(stdout);
      if (mask & 0x1000000) m_memory_partition_unit[i]->visualize();
      if (mask & 0x10000000) m_memory_partition_unit[i]->print(stdout);
      if (m != -1) {
        break;
      }
    }
  }
  fflush(stdout);
}

const shader_core_config *gpgpu_sim::getShaderCoreConfig() {
  return m_shader_config;
}

const memory_config *gpgpu_sim::getMemoryConfig() { return m_memory_config; }

simt_core_cluster *gpgpu_sim::getSIMTCluster() { return *m_cluster; }
