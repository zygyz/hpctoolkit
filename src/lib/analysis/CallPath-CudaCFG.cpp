// -*-Mode: C++;-*-

// * BeginRiceCopyright *****************************************************
//
// $HeadURL$
// $Id$
//
// --------------------------------------------------------------------------
// Part of HPCToolkit (hpctoolkit.org)
//
// Information about sources of support for research and development of
// HPCToolkit is at 'hpctoolkit.org' and in 'README.Acknowledgments'.
// --------------------------------------------------------------------------
//
// Copyright ((c)) 2002-2018, Rice University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
// * Neither the name of Rice University (RICE) nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// This software is provided by RICE and contributors "as is" and any
// express or implied warranties, including, but not limited to, the
// implied warranties of merchantability and fitness for a particular
// purpose are disclaimed. In no event shall RICE or contributors be
// liable for any direct, indirect, incidental, special, exemplary, or
// consequential damages (including, but not limited to, procurement of
// substitute goods or services; loss of use, data, or profits; or
// business interruption) however caused and on any theory of liability,
// whether in contract, strict liability, or tort (including negligence
// or otherwise) arising in any way out of the use of this software, even
// if advised of the possibility of such damage.
//
// ******************************************************* EndRiceCopyright *

//***************************************************************************
//
// File:
//   $HeadURL$
//
// Purpose:
//   [The purpose of this file]
//
// Description:
//   [The set of functions, macros, etc. defined in the file]
//
//***************************************************************************

//************************* System Include Files ****************************

#include <iostream>
#include <fstream>

#include <string>
#include <climits>
#include <cstring>

#include <typeinfo>
#include <unordered_map>

#include <sys/stat.h>

//*************************** User Include Files ****************************

#include <boost/graph/strong_components.hpp>
#include <boost/graph/adjacency_list.hpp>

#include <include/uint.h>
#include <include/gcc-attr.h>

#include "CallPath-CudaCFG.hpp"

using std::string;

#include <lib/prof/CCT-Tree.hpp>
#include <lib/prof/Metric-Mgr.hpp>
#include <lib/prof/Metric-ADesc.hpp>

#include <lib/profxml/XercesUtil.hpp>
#include <lib/profxml/PGMReader.hpp>

#include <lib/prof-lean/hpcrun-metric.h>

#include <lib/binutils/LM.hpp>
#include <lib/binutils/VMAInterval.hpp>

#include <lib/xml/xml.hpp>

#include <lib/support/diagnostics.h>
#include <lib/support/Logic.hpp>
#include <lib/support/IOUtil.hpp>
#include <lib/support/StrUtil.hpp>

#include <vector>
#include <iostream>


#define DEBUG_CALLPATH_CUDACFG 1
#define OUTPUT_SCC_FRAME 1
#define SIMULATE_SCC_WITH_LOOP 1


namespace Analysis {

namespace CallPath {

typedef std::map<VMA, std::vector<Prof::CCT::ANode *> > CallMap;

typedef std::map<Prof::CCT::ANode *, std::map<Prof::CCT::ANode *, double> > IncomingSamplesMap;

// Static functions
static void
constructCallMap(Prof::CCT::ANode *root, CallMap &call_map);

static void
constructCCTGraph(Prof::CCT::ANode *root, CallMap &call_map, CCTGraph &cct_graph);

static void
findGPURoots(CCTGraph &cct_graph, std::vector<Prof::CCT::ANode *> &gpu_roots);

static bool
findRecursion(CCTGraph &cct_graph,
  std::unordered_map<Prof::CCT::ANode *, std::vector<Prof::CCT::ANode *> > &cct_groups);

static void
mergeSCCNodes(CCTGraph &cct_graph, CCTGraph &old_cct_graph,
  std::unordered_map<Prof::CCT::ANode *, std::vector<Prof::CCT::ANode *> > &cct_groups);

static void
gatherIncomingSamples(CCTGraph &cct_graph,
  Prof::Metric::Mgr &mgr, IncomingSamplesMap &node_map, bool find_recursion);

static void
constructCallingContext(IncomingSamplesMap &incoming_samples,
  CCTGraph &cct_graph, std::vector<Prof::CCT::ANode *> &gpu_roots);

static void
copyPath(IncomingSamplesMap &incoming_samples,
  CCTGraph &cct_graph,
  Prof::CCT::ANode *cur, 
  Prof::CCT::ANode *prev,
  double adjust_factor);

static void
deleteTree(Prof::CCT::ANode *root);


static inline bool
isNVIDIADevice(const std::string &device) {
  return device.find("NVIDIA") != std::string::npos;
}


static inline bool
isSCCNode(Prof::CCT::ANode *node) {
  return node->type() == Prof::CCT::ANode::TySCC;
}


// Static inline functions
static inline Prof::Struct::Stmt *
getCudaCallStmt(Prof::CCT::ANode *node) {
  auto *strct = node->structure();
  if (strct != NULL && strct->type() == Prof::Struct::ANode::TyStmt) {
    auto *stmt = dynamic_cast<Prof::Struct::Stmt *>(strct);
    if (stmt->stmtType() == Prof::Struct::Stmt::STMT_CALL) {
      if (isNVIDIADevice(stmt->device())) {
        return stmt;
      }
    }
  }
  return NULL;
}


static inline Prof::Struct::Proc *
getProcStmt(Prof::CCT::ANode *node) {
  auto *strct = node->structure();
  if (strct != NULL && strct->type() == Prof::Struct::ANode::TyProc) {
    auto *proc = dynamic_cast<Prof::Struct::Proc *>(strct);
    return proc;
  }
  return NULL;
}


static inline void
debugCallGraph(CCTGraph &cct_graph) {
  for (auto it = cct_graph.edgeBegin(); it != cct_graph.edgeEnd(); ++it) {
    std::string from, to;
    if (getProcStmt(it->from) != NULL) {
      from = "(P)";
    } else if (isSCCNode(it->from)) {
      from = "(S)";
    } else if (getCudaCallStmt(it->from)) {
      from = "(C)";
    }
    if (getProcStmt(it->to) != NULL) {
      to = "(P)";
    } else if (isSCCNode(it->to)) {
      to = "(S)";
    } else if (getCudaCallStmt(it->to)) {
      to = "(C)";
    }
    std::cout << it->from->id()  << from << "->" << it->to->id() << to << std::endl;
  }
}


void
transformCudaCFGMain(Prof::CallPath::Profile& prof) {
  // Construct a map to pair vmas and calls
  Prof::CCT::ANode *root = prof.cct()->root();
  CallMap call_map;
  constructCallMap(root, call_map);

  // Construct a cct call graph on GPU
  CCTGraph *cct_graph = new CCTGraph();
  constructCCTGraph(root, call_map, *cct_graph);

  if (DEBUG_CALLPATH_CUDACFG) {
    debugCallGraph(*cct_graph);
  }

  // TODO(keren): Handle SCCs
  std::unordered_map<Prof::CCT::ANode *, std::vector<Prof::CCT::ANode *> > cct_groups;
  bool find_recursion = false;
  if (findRecursion(*cct_graph, cct_groups)) {
    find_recursion = true;
    if (DEBUG_CALLPATH_CUDACFG) {
      std::cout << "Find recursive calls" << std::endl;
    }
    CCTGraph *old_cct_graph = cct_graph;
    cct_graph = new CCTGraph();
    mergeSCCNodes(*cct_graph, *old_cct_graph, cct_groups);
    delete old_cct_graph;
    if (DEBUG_CALLPATH_CUDACFG) {
      debugCallGraph(*cct_graph);
    }
  }

  // Find gpu global functions
  std::vector<Prof::CCT::ANode *> gpu_roots;
  findGPURoots(*cct_graph, gpu_roots);

  // Record input samples for each node
  IncomingSamplesMap incoming_samples;
  gatherIncomingSamples(*cct_graph, *(prof.metricMgr()), incoming_samples, find_recursion);

  // Copy from every gpu_root to leafs
  if (DEBUG_CALLPATH_CUDACFG) {
    std::cout << "Construct calling context tree" << std::endl;
  }
  constructCallingContext(incoming_samples, *cct_graph, gpu_roots);

  delete cct_graph;
}


static void
constructCallMap(Prof::CCT::ANode *root, CallMap &call_map) {
  Prof::CCT::ANodeIterator it(root, NULL/*filter*/, false/*leavesOnly*/,
    IteratorStack::PreOrder);
  // Find call nodes where device = nvidia and construct a graph
  for (Prof::CCT::ANode *n = NULL; (n = it.current()); ++it) {
    auto *stmt = getCudaCallStmt(n);
    if (stmt != NULL) {
      call_map[stmt->target()].push_back(n);
    }
  }
}


static void
constructCCTGraph(Prof::CCT::ANode *root, CallMap &call_map,
  CCTGraph &cct_graph) {
  // Step1: Add proc->call
  for (auto &entry : call_map) {
    auto &vec = entry.second;
    for (auto *call_node : vec) {
      auto *parent = call_node->ancestorProcFrm();
      cct_graph.addEdge(parent, call_node);
    }
  }

  // Step2: Add call->proc if statement and vma matches, 
  Prof::CCT::ANodeIterator it(root, NULL/*filter*/, false/*leavesOnly*/,
    IteratorStack::PreOrder);
  for (Prof::CCT::ANode *n = NULL; (n = it.current()); ++it) {
    auto *proc = getProcStmt(n);
    if (proc != NULL) {
      auto vma_set = *(proc->vmaSet().begin());
      VMA vma = vma_set.beg();
      if (call_map.find(vma) != call_map.end()) {
        auto call_nodes = call_map[vma];
        for (auto *call_node : call_nodes) {
          cct_graph.addEdge(call_node, n);
        }
      }
    }
  }
}


static bool
findRecursion(CCTGraph &cct_graph,
  std::unordered_map<Prof::CCT::ANode *, std::vector<Prof::CCT::ANode *> > &cct_groups) {
  std::unordered_map<int, int> graph_index_converter;
  std::unordered_map<int, Prof::CCT::ANode *> graph_index_reverse_converter;
  int start_index = 0;
  typedef boost::adjacency_list <boost::vecS, boost::vecS, boost::directedS> Graph;
  const int N = cct_graph.size();
  Graph G(N);

  // Find recursive calls
  for (auto it = cct_graph.edgeBegin(); it != cct_graph.edgeEnd(); ++it) {
    auto from_id = it->from->id();
    if (graph_index_converter.find(from_id) != graph_index_converter.end()) {
      from_id = graph_index_converter[from_id];
    } else {
      graph_index_converter[from_id] = start_index;
      graph_index_reverse_converter[start_index] = it->from;
      from_id = start_index++;
    }

    auto to_id = it->to->id();
    if (graph_index_converter.find(to_id) != graph_index_converter.end()) {
      to_id = graph_index_converter[to_id];
    } else {
      graph_index_converter[to_id] = start_index;
      graph_index_reverse_converter[start_index] = it->to;
      to_id = start_index++;
    }

    if (from_id == to_id) {
      std::cout << "Self recursion" << std::endl;
    }
    add_edge(from_id, to_id, G);
  }

  // Find mutual recursive calls
  std::vector<int> c(num_vertices(G));
  int num = boost::strong_components(G,
    boost::make_iterator_property_map(c.begin(), boost::get(boost::vertex_index, G)));

  for (size_t i = 0; i < c.size(); ++i) {
    cct_groups[graph_index_reverse_converter[c[i]]].push_back(graph_index_reverse_converter[i]);
  }

  if (DEBUG_CALLPATH_CUDACFG) {
    std::cout << "CCT graph vertices " << cct_graph.size() << std::endl;
    std::cout << "Num vertices " << num_vertices(G) << std::endl;
    std::cout << "Find scc " << num << std::endl;
    for (size_t i = 0; i != c.size(); ++i) {
      std::cout << "Vertex " << i
        <<" is in component " << c[i] << std::endl;
    }
  }

  return num != static_cast<int>(cct_graph.size());
}


void
mergeSCCNodes(CCTGraph &cct_graph, CCTGraph &old_cct_graph,
  std::unordered_map<Prof::CCT::ANode *, std::vector<Prof::CCT::ANode *> > &cct_groups) {
  // Map node to group
  std::unordered_map<Prof::CCT::ANode *, Prof::CCT::ANode *> cct_group_reverse_map;
  for (auto it = cct_groups.begin(); it != cct_groups.end(); ++it) {
    auto &vec = it->second;
    auto *group = it->first;
    for (auto *n : vec) {
      cct_group_reverse_map[n] = group;
    }
  }

  // Add edges between SCCs
  for (auto it = cct_groups.begin(); it != cct_groups.end(); ++it) {
    Prof::CCT::ANode *scc_node = new Prof::CCT::SCC(NULL);
    auto &vec = it->second;
    for (auto *n : vec) {
      if (getProcStmt(n) != NULL) {
        // scc_node->proc
        cct_graph.addEdge(scc_node, n);
      } 
    }
    auto *group = it->first;
    for (auto nit = old_cct_graph.edgeBegin(); nit != old_cct_graph.edgeEnd(); ++nit) {
      // call->scc
      if (cct_group_reverse_map[nit->from] != group && getCudaCallStmt(nit->from) != NULL) {
        bool find = false;
        for (auto *n : vec) {
          if (nit->to == n) {
            find = true;
            break;
          }
        }
        if (find) {
          // from->scc_in_node
          cct_graph.addEdge(nit->from, scc_node);
        }
      } else if (cct_group_reverse_map[nit->to] != group && getCudaCallStmt(nit->to) != NULL) {
        // proc->call
        cct_graph.addEdge(nit->from, nit->to);
      }
    }
  }
}


static void
findGPURoots(CCTGraph &cct_graph, std::vector<Prof::CCT::ANode *> &gpu_roots) {
  for (auto it = cct_graph.nodeBegin(); it != cct_graph.nodeEnd(); ++it) {
    if (cct_graph.incoming_nodes(*it) == cct_graph.incoming_nodes_end()) {
      gpu_roots.push_back(*it);
    }
  }
}


static void
gatherIncomingSamples(CCTGraph &cct_graph,
  Prof::Metric::Mgr &mgr, IncomingSamplesMap &node_map,
  bool find_recursion) {
  int sample_metric_idx = -1;
  for (size_t i = 0; i < mgr.size(); ++i) {
    if (mgr.metric(i)->namePfxBase() == "GPU_ISAMP") {
      sample_metric_idx = i;
      break;
    }
  }
  // Gather call and scc samples
  for (auto it = cct_graph.nodeBegin(); it != cct_graph.nodeEnd(); ++it) {
    Prof::CCT::ANode *node = *it;
    if ((find_recursion && isSCCNode(node)) || (!find_recursion && getProcStmt(node) != NULL)) {
      if (cct_graph.incoming_nodes(node) != cct_graph.incoming_nodes_end()) {
        std::vector<Prof::CCT::ANode *> &vec = cct_graph.incoming_nodes(node)->second;
        if (sample_metric_idx == -1) {
          for (auto *neighbor : vec) {
            // By default, set it to one
            node_map[node][neighbor] = 1.0;
          }
        } else {
          for (auto *neighbor : vec) {
            node_map[node][neighbor] = std::max(neighbor->metric(sample_metric_idx), 1.0);
          }
        }
      }
    }
  }
}


static void
constructCallingContext(IncomingSamplesMap &incoming_samples,
  CCTGraph &cct_graph, std::vector<Prof::CCT::ANode *> &gpu_roots) {
  for (auto *gpu_root : gpu_roots) {
    if (isSCCNode(gpu_root)) {  // root is scc
      auto edge_iterator = cct_graph.outgoing_nodes(gpu_root);
      for (auto *n : edge_iterator->second) {
        if (cct_graph.outgoing_nodes_size(gpu_root) > 1 && OUTPUT_SCC_FRAME) {  // keep SCC frame
          auto *new_node = gpu_root->clone();
          auto *parent = n->parent();  
          new_node->link(parent);
          copyPath(incoming_samples, cct_graph, n, new_node, 1.0);
        } else {
          auto *parent = n->parent();  
          copyPath(incoming_samples, cct_graph, n, parent, 1.0);
        }
      }
    } else {
      auto *parent = gpu_root->parent(); 
      copyPath(incoming_samples, cct_graph, gpu_root, parent, 1.0);
    }
  }
  for (auto it = cct_graph.nodeBegin(); it != cct_graph.nodeEnd(); ++it) {
    Prof::CCT::ANode *node = *it;
    if (getProcStmt(node) != NULL) {
      node->unlink();
    }
  }
  for (auto *gpu_root : gpu_roots) {
    deleteTree(gpu_root);
  }
}


static void
copyPath(IncomingSamplesMap &incoming_samples,
  CCTGraph &cct_graph, 
  Prof::CCT::ANode *cur, Prof::CCT::ANode *prev,
  double adjust_factor) {
  bool isSCC = isSCCNode(cur);
  // Copy node
  Prof::CCT::ANode *new_node;
  if (isSCC && SIMULATE_SCC_WITH_LOOP) {
    new_node = new Prof::CCT::Loop(prev);
  } else {
    new_node = cur->clone();
    new_node->link(prev);
  }
  // Adjust metrics
  for (size_t i = 0; i < new_node->numMetrics(); ++i) {
    new_node->metric(i) *= adjust_factor;
  }

  bool isCall = getCudaCallStmt(cur) == NULL ? false : true;
  if (isCall) {
    // Case 1: Call node, skip to the procedure
    auto edge_iterator = cct_graph.outgoing_nodes(cur);
    if (edge_iterator != cct_graph.outgoing_nodes_end()) {  // some calls won't have outgoing edges
      for (auto *n : edge_iterator->second) {
        double sum_samples = 0.0;
        for (auto &neighor : incoming_samples[n]) {
          sum_samples += neighor.second;
        }
        double cur_samples = incoming_samples[n][cur];
        adjust_factor *= cur_samples / sum_samples;
        if (DEBUG_CALLPATH_CUDACFG) {
          std::cout << "Lay over a call" << std::endl;
        }
        copyPath(incoming_samples, cct_graph, n, new_node, adjust_factor);
      }
    }
  } else if (isSCC) {
    // Case 2: SCC node
    auto edge_iterator = cct_graph.outgoing_nodes(cur);
    for (auto *n : edge_iterator->second) {
      if (DEBUG_CALLPATH_CUDACFG) {
        std::cout << "Lay over a SCC" << std::endl;
      }
      // Keep scc in frame
      if (cct_graph.outgoing_nodes_size(cur) > 1 && OUTPUT_SCC_FRAME) {
        copyPath(incoming_samples, cct_graph, n, new_node, adjust_factor);
      } else {
        copyPath(incoming_samples, cct_graph, n, prev, adjust_factor);
      }
    }
  } else if (!cur->isLeaf()) {
    if (DEBUG_CALLPATH_CUDACFG) {
      std::cout << "Not a call or SCC" << std::endl;
    }
    // Case 3: Iterate through children
    Prof::CCT::ANodeChildIterator it(cur, NULL/*filter*/);
    for (Prof::CCT::ANode *n = NULL; (n = it.current()); ++it) {
      copyPath(incoming_samples, cct_graph, n, new_node, adjust_factor);
    }
  }
}


static void
deleteTree(Prof::CCT::ANode *root) {
  // TODO(keren): delete the original structure to reduce memory consumption
}


} // namespace CallPath

} // namespace Analysis