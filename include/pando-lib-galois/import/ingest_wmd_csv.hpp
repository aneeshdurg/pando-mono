// SPDX-License-Identifier: MIT
// Copyright (c) 2023. University of Texas at Austin. All rights reserved.

#ifndef PANDO_LIB_GALOIS_IMPORT_INGEST_WMD_CSV_HPP_
#define PANDO_LIB_GALOIS_IMPORT_INGEST_WMD_CSV_HPP_

#include <utility>

#include <pando-lib-galois/containers/thread_local_vector.hpp>
#include <pando-lib-galois/graphs/dist_local_csr.hpp>
#include <pando-lib-galois/sync/wait_group.hpp>
#include <pando-rt/memory/memory_guard.hpp>
#include <pando-rt/tracing.hpp>

namespace galois {

void loadWMDFilePerThread(
    galois::WaitGroup::HandleType wgh, pando::Array<char> filename, std::uint64_t segmentsPerThread,
    std::uint64_t numThreads, std::uint64_t threadID,
    ThreadLocalVector<pando::Vector<WMDEdge>> localReadEdges,
    ThreadLocalStorage<HashTable<std::uint64_t, std::uint64_t>> perThreadRename,
    ThreadLocalVector<WMDVertex> localReadVertices, galois::DAccumulator<std::uint64_t> totVerts);

template <typename VertexFunc, typename EdgeFunc>
pando::Status wmdCSVParse(const char* line, pando::Array<galois::StringView> tokens,
                          VertexFunc vfunc, EdgeFunc efunc) {
  assert(tokens.size() == 10);
  splitLine<10>(line, ',', tokens);
  galois::StringView token1(tokens[1]);
  galois::StringView token2(tokens[2]);
  galois::StringView token3(tokens[3]);
  galois::StringView token4(tokens[4]);
  galois::StringView token5(tokens[5]);
  galois::StringView token6(tokens[6]);

  bool isNode =
      tokens[0] == galois::StringView("Person") || tokens[0] == galois::StringView("ForumEvent") ||
      tokens[0] == galois::StringView("Forum") || tokens[0] == galois::StringView("Publication") ||
      tokens[0] == galois::StringView("Topic");

  if (isNode) {
    PANDO_CHECK_RETURN(vfunc(WMDVertex(tokens)));
    return pando::Status::Success;
  } else { // edge type
    agile::TYPES inverseEdgeType;
    if (tokens[0] == galois::StringView("Sale")) {
      inverseEdgeType = agile::TYPES::PURCHASE;
    } else if (tokens[0] == galois::StringView("Author")) {
      inverseEdgeType = agile::TYPES::WRITTENBY;
    } else if (tokens[0] == galois::StringView("Includes")) {
      inverseEdgeType = agile::TYPES::INCLUDEDIN;
    } else if (tokens[0] == galois::StringView("HasTopic")) {
      inverseEdgeType = agile::TYPES::TOPICIN;
    } else if (tokens[0] == galois::StringView("HasOrg")) {
      inverseEdgeType = agile::TYPES::ORG_IN;
    } else {
      return pando::Status::Error;
    }
    PANDO_CHECK_RETURN(efunc(WMDEdge(tokens), inverseEdgeType));
    return pando::Status::Success;
  }
}

template <typename VertexType, typename EdgeType>
galois::DistLocalCSR<VertexType, EdgeType> initializeWMDDLCSR(pando::Array<char> filename,
                                                              std::uint16_t scale_factor = 8) {
  galois::ThreadLocalVector<pando::Vector<WMDEdge>> localReadEdges;
  PANDO_CHECK(localReadEdges.initialize());

  galois::ThreadLocalVector<WMDVertex> localReadVertices;
  PANDO_CHECK(localReadVertices.initialize());

  const std::uint64_t numThreads = localReadEdges.size() - pando::getPlaceDims().node.id;
  const std::uint64_t hosts = static_cast<std::uint64_t>(pando::getPlaceDims().node.id);
  const std::uint64_t numVHosts = hosts * scale_factor;

  galois::WaitGroup wg;
  PANDO_CHECK(wg.initialize(numThreads));
  auto wgh = wg.getHandle();

  galois::DAccumulator<std::uint64_t> totVerts{};
  PANDO_CHECK(totVerts.initialize());

  galois::ThreadLocalStorage<galois::HashTable<std::uint64_t, std::uint64_t>> perThreadRename{};
  PANDO_CHECK(perThreadRename.initialize());

  for (auto hashRef : perThreadRename) {
    hashRef = galois::HashTable<std::uint64_t, std::uint64_t>{};
    PANDO_CHECK(fmap(hashRef, initialize, 0));
  }

  PANDO_MEM_STAT_NEW_KERNEL("loadWMDFilePerThread Start");
  for (std::uint64_t i = 0; i < numThreads; i++) {
    pando::Place place = pando::Place{pando::NodeIndex{static_cast<std::int64_t>(i % hosts)},
                                      pando::anyPod, pando::anyCore};
    PANDO_CHECK(pando::executeOn(place, &galois::loadWMDFilePerThread, wgh, filename, 1, numThreads,
                                 i, localReadEdges, perThreadRename, localReadVertices, totVerts));
  }

  PANDO_CHECK(wg.wait());
  PANDO_MEM_STAT_NEW_KERNEL("loadWMDFilePerThread End");

#ifdef FREE
  galois::WaitGroup freeWaiter;
  PANDO_CHECK(freeWaiter.initialize(0));
  auto freeWGH = freeWaiter.getHandle();
  galois::doAll(
      freeWGH, perThreadRename, +[](galois::HashTable<std::uint64_t, std::uint64_t> hash) {
        hash.deinitialize();
      });
#endif

  pando::Array<galois::Pair<std::uint64_t, std::uint64_t>> labeledEdgeCounts = PANDO_EXPECT_CHECK(
      galois::internal::buildEdgeCountToSend<WMDEdge>(numVHosts, localReadEdges));

  auto [v2PM, numEdges] =
      PANDO_EXPECT_CHECK(galois::internal::buildVirtualToPhysicalMapping(hosts, labeledEdgeCounts));

#if FREE
  auto freeLabeledEdgeCounts =
      +[](pando::Array<galois::Pair<std::uint64_t, std::uint64_t>> labeledEdgeCounts) {
        labeledEdgeCounts.deinitialize();
      };
  PANDO_CHECK(pando::executeOn(
      pando::anyPlace, freeLabeledEdgeCounts,
      static_cast<pando::Array<galois::Pair<std::uint64_t, std::uint64_t>>>(labeledEdgeCounts)));
  PANDO_CHECK(freeWaiter.wait());
  perThreadRename.deinitialize();
#endif

  auto hostLocalV2PM = PANDO_EXPECT_CHECK(galois::copyToAllHosts(std::move(v2PM)));

  /** Generate Vertex Partition **/
  galois::HostLocalStorage<pando::Vector<WMDVertex>> pHV =
      internal::partitionVerticesParallel(std::move(localReadVertices), hostLocalV2PM);

  /** Generate Edge Partition **/
  auto [partEdges, renamePerHost] =
      internal::partitionEdgesParallely(pHV, std::move(localReadEdges), hostLocalV2PM);

  std::uint64_t numVertices = totVerts.reduce();

  using Graph = galois::DistLocalCSR<VertexType, EdgeType>;
  Graph graph;
  graph.template initializeAfterGather<galois::WMDVertex, galois::WMDEdge>(
      pHV, numVertices, partEdges, renamePerHost, numEdges, hostLocalV2PM);

#if FREE
  auto freeTheRest = +[](decltype(pHV) pHV, decltype(partEdges) partEdges,
                         decltype(renamePerHost) renamePerHost, decltype(numEdges) numEdges) {
    for (pando::Vector<WMDVertex> vV : pHV) {
      vV.deinitialize();
    }
    pHV.deinitialize();
    for (pando::Vector<pando::Vector<WMDEdge>> vVE : partEdges) {
      for (pando::Vector<WMDEdge> vE : vVE) {
        vE.deinitialize();
      }
      vVE.deinitialize();
    }
    partEdges.deinitialize();
    renamePerHost.deinitialize();
    numEdges.deinitialize();
  };

  PANDO_CHECK(
      pando::executeOn(pando::anyPlace, freeTheRest, pHV, partEdges, renamePerHost, numEdges));
  freeWaiter.deinitialize();
#endif
  wg.deinitialize();
  return graph;
}

} // namespace galois

#endif // PANDO_LIB_GALOIS_IMPORT_INGEST_WMD_CSV_HPP_
