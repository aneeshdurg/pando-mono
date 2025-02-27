// SPDX-License-Identifier: MIT
// Copyright (c) 2023. University of Texas at Austin. All rights reserved.

#ifndef PANDO_LIB_GALOIS_GRAPHS_DIST_LOCAL_CSR_HPP_
#define PANDO_LIB_GALOIS_GRAPHS_DIST_LOCAL_CSR_HPP_

#include <pando-rt/export.h>

#include <algorithm>
#include <utility>

#include <pando-lib-galois/containers/array.hpp>
#include <pando-lib-galois/containers/host_indexed_map.hpp>
#include <pando-lib-galois/containers/host_local_storage.hpp>
#include <pando-lib-galois/containers/per_thread.hpp>
#include <pando-lib-galois/graphs/local_csr.hpp>
#include <pando-lib-galois/import/wmd_graph_importer.hpp>
#include <pando-lib-galois/loops/do_all.hpp>
#include <pando-lib-galois/utility/gptr_monad.hpp>
#include <pando-rt/containers/array.hpp>
#include <pando-rt/containers/vector.hpp>
#include <pando-rt/memory/memory_guard.hpp>
#include <pando-rt/pando-rt.hpp>

#define FREE 1

namespace galois {

namespace internal {

template <typename VertexType, typename EdgeType>
struct DLCSR_InitializeState {
  using CSR = LCSR<VertexType, EdgeType>;

  DLCSR_InitializeState() = default;
  DLCSR_InitializeState(HostLocalStorage<HostIndexedMap<CSR>> arrayOfCSRs_,
                        galois::PerThreadVector<VertexType> vertices_,
                        galois::PerThreadVector<EdgeType> edges_,
                        galois::PerThreadVector<uint64_t> edgeCounts_)
      : arrayOfCSRs(arrayOfCSRs_), vertices(vertices_), edges(edges_), edgeCounts(edgeCounts_) {}

  HostLocalStorage<HostIndexedMap<CSR>> arrayOfCSRs;
  galois::PerThreadVector<VertexType> vertices;
  galois::PerThreadVector<EdgeType> edges;
  galois::PerThreadVector<uint64_t> edgeCounts;
};

} // namespace internal

template <typename VertexType, typename EdgeType>
class MirrorDistLocalCSR;

template <typename VertexType = WMDVertex, typename EdgeType = WMDEdge>
class DistLocalCSR {
public:
  friend MirrorDistLocalCSR<VertexType, EdgeType>;
  using VertexTokenID = std::uint64_t;
  using VertexTopologyID = pando::GlobalPtr<Vertex>;
  using EdgeHandle = pando::GlobalPtr<HalfEdge>;
  using VertexData = VertexType;
  using EdgeData = EdgeType;
  using EdgeRange = RefSpan<HalfEdge>;
  using EdgeDataRange = pando::Span<EdgeData>;
  using CSR = LCSR<VertexType, EdgeType>;
  using CSRCache = HostLocalStorage<HostIndexedMap<CSR>>;

  class VertexIt {
    CSRCache arrayOfCSRs{};
    pando::GlobalPtr<Vertex> m_pos;

  public:
    using iterator_category = std::bidirectional_iterator_tag;
    using difference_type = std::int64_t;
    using value_type = typename std::pointer_traits<decltype(&m_pos)>::element_type;
    using pointer = pando::GlobalPtr<Vertex>;
    using reference = VertexTopologyID;

    VertexIt(CSRCache arrayOfCSRs, VertexTopologyID pos) : arrayOfCSRs(arrayOfCSRs), m_pos(pos) {}

    constexpr VertexIt() noexcept = default;
    constexpr VertexIt(VertexIt&&) noexcept = default;
    constexpr VertexIt(const VertexIt&) noexcept = default;
    ~VertexIt() = default;

    constexpr VertexIt& operator=(const VertexIt&) noexcept = default;
    constexpr VertexIt& operator=(VertexIt&&) noexcept = default;

    reference operator*() const noexcept {
      return m_pos;
    }

    reference operator*() noexcept {
      return m_pos;
    }

    pointer operator->() {
      return m_pos;
    }

    VertexIt& operator++() {
      auto currNode = static_cast<std::uint64_t>(galois::localityOf(m_pos).node.id);
      pointer ptr = m_pos + 1;
      CSR csrCurr = getCachedCSR(arrayOfCSRs, currNode);
      if (csrCurr.vertexEdgeOffsets.end() - 1 > ptr ||
          (int64_t)currNode == pando::getPlaceDims().node.id - 1) {
        m_pos = ptr;
      } else {
        csrCurr = getCachedCSR(arrayOfCSRs, currNode + 1);
        this->m_pos = csrCurr.vertexEdgeOffsets.begin();
      }
      return *this;
    }

    VertexIt operator++(int) {
      VertexIt tmp = *this;
      ++(*this);
      return tmp;
    }

    VertexIt& operator--() {
      auto currNode = static_cast<std::uint64_t>(galois::localityOf(m_pos).node.id);
      pointer ptr = m_pos - 1;
      CSR csrCurr = getCachedCSR(arrayOfCSRs, currNode);
      if (csrCurr.vertexEdgeOffsets.begin() <= ptr || currNode == 0) {
        m_pos = ptr;
      } else {
        csrCurr = getCachedCSR(arrayOfCSRs, currNode - 1);
        m_pos = csrCurr.vertexEdgeOffsets.end() - 2;
      }
      return *this;
    }

    VertexIt operator--(int) {
      VertexIt tmp = *this;
      --(*this);
      return tmp;
    }

    friend bool operator==(const VertexIt& a, const VertexIt& b) {
      return a.m_pos == b.m_pos;
    }

    friend bool operator!=(const VertexIt& a, const VertexIt& b) {
      return !(a == b);
    }

    friend bool operator<(const VertexIt& a, const VertexIt& b) {
      return localityOf(a.m_pos).node.id < localityOf(b.m_pos).node.id || a.m_pos < b.m_pos;
    }

    friend bool operator>(const VertexIt& a, const VertexIt& b) {
      return localityOf(a.m_pos).node.id > localityOf(b.m_pos).node.id || a.m_pos > b.m_pos;
    }

    friend bool operator<=(const VertexIt& a, const VertexIt& b) {
      return !(a > b);
    }

    friend bool operator>=(const VertexIt& a, const VertexIt& b) {
      return !(a < b);
    }

    friend pando::Place localityOf(VertexIt& a) {
      return pando::localityOf(a.m_pos);
    }
  };

  class VertexDataIt {
    CSRCache arrayOfCSRs;
    typename CSR::VertexDataRange::iterator m_pos;

  public:
    using iterator_category = std::bidirectional_iterator_tag;
    using difference_type = std::int64_t;
    using value_type = VertexData;
    using pointer = pando::GlobalPtr<VertexData>;
    using reference = pando::GlobalRef<VertexData>;

    constexpr VertexDataIt(CSRCache arrayOfCSRs, typename pando::GlobalPtr<VertexData> pos)
        : arrayOfCSRs(arrayOfCSRs), m_pos(pos) {}

    constexpr VertexDataIt() noexcept = default;
    constexpr VertexDataIt(VertexDataIt&&) noexcept = default;
    constexpr VertexDataIt(const VertexDataIt&) noexcept = default;
    ~VertexDataIt() = default;

    constexpr VertexDataIt& operator=(const VertexDataIt&) noexcept = default;
    constexpr VertexDataIt& operator=(VertexDataIt&&) noexcept = default;

    reference operator*() const noexcept {
      return *m_pos;
    }

    reference operator*() noexcept {
      return *m_pos;
    }

    pointer operator->() {
      return m_pos;
    }

    VertexDataIt& operator++() {
      auto currNode = static_cast<std::uint64_t>(galois::localityOf(m_pos).node.id);
      pointer ptr = m_pos + 1;
      CSR csrCurr = getCachedCSR(arrayOfCSRs, currNode);
      if (csrCurr.vertexData.end() > ptr ||
          currNode == static_cast<std::uint64_t>(pando::getPlaceDims().node.id - 1)) {
        m_pos = ptr;
      } else {
        csrCurr = getCachedCSR(arrayOfCSRs, currNode + 1);
        m_pos = csrCurr.vertexData.begin();
      }
      return *this;
    }

    VertexDataIt operator++(int) {
      VertexDataIt tmp = *this;
      ++(*this);
      return tmp;
    }

    VertexDataIt& operator--() {
      auto currNode = static_cast<std::uint64_t>(galois::localityOf(m_pos).node.id);
      pointer ptr = m_pos - 1;
      CSR csrCurr = getCachedCSR(arrayOfCSRs, currNode);
      if (csrCurr.vertexData.begin() <= ptr || currNode == 0) {
        m_pos = *ptr;
      } else {
        csrCurr = getCachedCSR(arrayOfCSRs, currNode - 1);
        m_pos = *csrCurr.vertexData.end() - 1;
      }
      return *this;
    }

    VertexDataIt operator--(int) {
      VertexDataIt tmp = *this;
      --(*this);
      return tmp;
    }

    friend bool operator==(const VertexDataIt& a, const VertexDataIt& b) {
      return a.m_pos == b.m_pos;
    }

    friend bool operator!=(const VertexDataIt& a, const VertexDataIt& b) {
      return !(a == b);
    }

    friend bool operator<(const VertexDataIt& a, const VertexDataIt& b) {
      return localityOf(a.m_pos).node.id < localityOf(b.m_pos).node.id || a.m_pos < b.m_pos;
    }

    friend bool operator>(const VertexDataIt& a, const VertexDataIt& b) {
      return localityOf(a.m_pos).node.id > localityOf(b.m_pos).node.id || a.m_pos > b.m_pos;
    }

    friend bool operator<=(const VertexDataIt& a, const VertexDataIt& b) {
      return !(a > b);
    }

    friend bool operator>=(const VertexDataIt& a, const VertexDataIt& b) {
      return !(a < b);
    }

    friend pando::Place localityOf(VertexDataIt& a) {
      return galois::localityOf(a.m_pos);
    }
  };

  struct VertexRange {
    CSRCache arrayOfCSRs{};
    VertexTopologyID m_beg;
    VertexTopologyID m_end;
    std::uint64_t m_size;

    constexpr VertexRange(decltype(arrayOfCSRs) csrs, decltype(m_beg) begin, decltype(m_end) end,
                          std::uint64_t size)
        : arrayOfCSRs(csrs), m_beg(begin), m_end(end), m_size(size) {}

    constexpr VertexRange() noexcept = default;
    constexpr VertexRange(VertexRange&&) noexcept = default;
    constexpr VertexRange(const VertexRange&) noexcept = default;
    ~VertexRange() = default;

    constexpr VertexRange& operator=(const VertexRange&) noexcept = default;
    constexpr VertexRange& operator=(VertexRange&&) noexcept = default;

    using iterator = VertexIt;
    iterator begin() noexcept {
      return iterator(arrayOfCSRs, m_beg);
    }
    iterator begin() const noexcept {
      return iterator(arrayOfCSRs, m_beg);
    }
    iterator end() noexcept {
      return iterator(arrayOfCSRs, m_end);
    }
    iterator end() const noexcept {
      return iterator(arrayOfCSRs, m_end);
    }
    std::uint64_t size() const {
      return m_size;
    }
  };

  struct VertexDataRange {
    CSRCache arrayOfCSRs{};
    pando::GlobalPtr<VertexData> m_beg;
    pando::GlobalPtr<VertexData> m_end;
    std::uint64_t m_size;

    using iterator = VertexDataIt;
    iterator begin() noexcept {
      return iterator(arrayOfCSRs, m_beg);
    }
    iterator begin() const noexcept {
      return iterator(arrayOfCSRs, m_beg);
    }
    iterator end() noexcept {
      return iterator(arrayOfCSRs, m_end);
    }
    iterator end() const noexcept {
      return iterator(arrayOfCSRs, m_end);
    }
    std::uint64_t size() const {
      return m_size;
    }
  };

private:
  constexpr static pando::GlobalRef<CSR> getCachedCSR(CSRCache cache, std::uint64_t i) {
    return fmap(cache.getLocalRef(), operator[], i);
  }

  template <typename T>
  pando::GlobalRef<CSR> getCSR(pando::GlobalPtr<T> ptr) {
    const std::uint64_t nodeIdx = pando::localityOf(ptr).node.id;
    return getCSR(nodeIdx);
  }

  EdgeHandle halfEdgeBegin(VertexTopologyID vertex) {
    return fmap(getCSR(vertex), halfEdgeBegin, vertex);
  }

  EdgeHandle halfEdgeEnd(VertexTopologyID vertex) {
    return static_cast<Vertex>(*(vertex + 1)).edgeBegin;
  }

  std::uint64_t numVHosts() {
    return lift(this->virtualToPhysicalMap.getLocalRef(), size);
  }

  struct InitializeEdgeState {
    InitializeEdgeState() = default;
    InitializeEdgeState(DistLocalCSR<VertexType, EdgeType> dlcsr_,
                        galois::PerThreadVector<EdgeType> edges_,
                        galois::PerThreadVector<VertexTokenID> edgeDsts_)
        : dlcsr(dlcsr_), edges(edges_), edgeDsts(edgeDsts_) {}

    DistLocalCSR<VertexType, EdgeType> dlcsr;
    galois::PerThreadVector<EdgeType> edges;
    galois::PerThreadVector<VertexTokenID> edgeDsts;
  };

  template <typename, typename>
  friend class DistLocalCSR;
  template <typename, typename>
  friend class MirrorDistLocalCSR;

public:
  constexpr DistLocalCSR() noexcept = default;
  constexpr DistLocalCSR(DistLocalCSR&&) noexcept = default;
  constexpr DistLocalCSR(const DistLocalCSR&) noexcept = default;
  ~DistLocalCSR() = default;

  constexpr DistLocalCSR& operator=(const DistLocalCSR&) noexcept = default;
  constexpr DistLocalCSR& operator=(DistLocalCSR&&) noexcept = default;

  /** Official Graph APIS **/
  void deinitialize() {
    for (std::uint64_t i = 0; i < arrayOfCSRs.size(); i++) {
      liftVoid(getCSR(i), deinitialize);
    }
    arrayOfCSRs.deinitialize();
    for (pando::Array<std::uint64_t> v : virtualToPhysicalMap) {
      v.deinitialize();
    }
    virtualToPhysicalMap.deinitialize();
  }

  /** size stuff **/
  std::uint64_t size() noexcept {
    return numVertices;
  }
  std::uint64_t size() const noexcept {
    return numVertices;
  }
  std::uint64_t sizeEdges() noexcept {
    return numEdges;
  }
  std::uint64_t sizeEdges() const noexcept {
    return numEdges;
  }
  std::uint64_t getNumEdges(VertexTopologyID vertex) {
    return halfEdgeEnd(vertex) - halfEdgeBegin(vertex);
  }

  /** Vertex Manipulation **/
  VertexTopologyID getTopologyID(VertexTokenID tid) {
    auto [ret, found] = fmap(getLocalCSR(), relaxedGetTopologyID, tid);
    if (!found) {
      return getGlobalTopologyID(tid);
    } else {
      return ret;
    }
  }

  // This function is for mirrored dist local csr, or classes which will directly use it. Don't use
  // it externally. getLocalTopologyID with non-existing tokenID will return failure.
  Pair<VertexTopologyID, bool> getLocalTopologyID(VertexTokenID tid) {
    return fmap(getLocalCSR(), relaxedGetTopologyID, tid);
  }

private:
  VertexTopologyID getGlobalTopologyID(VertexTokenID tid) {
    std::uint64_t virtualHostID = tid % this->numVHosts();
    std::uint64_t physicalHost = fmap(virtualToPhysicalMap.getLocalRef(), get, virtualHostID);
    return fmap(getCSR(physicalHost), getTopologyID, tid);
  }

public:
  VertexTopologyID getTopologyIDFromIndex(std::uint64_t index) {
    std::uint64_t hostNum = 0;
    std::uint64_t hostSize;
    for (; index > (hostSize = localSize(hostNum)); hostNum++, index -= hostSize) {}
    return fmap(getCSR(hostNum), getTopologyIDFromIndex, index);
  }
  VertexTokenID getTokenID(VertexTopologyID tid) {
    return fmap(getCSR(tid), getTokenID, tid);
  }
  std::uint64_t getVertexIndex(VertexTopologyID vertex) {
    std::uint64_t vid = fmap(getCSR(vertex), getVertexIndex, vertex);
    for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(getLocalityVertex(vertex).node.id);
         i++) {
      vid += lift(getCSR(i), size);
    }
    return vid;
  }
  pando::Place getLocalityVertex(VertexTopologyID vertex) {
    // All edges must be local to the vertex
    return galois::localityOf(vertex);
  }

  /** Edge Manipulation **/
  EdgeHandle mintEdgeHandle(VertexTopologyID vertex, std::uint64_t off) {
    return halfEdgeBegin(vertex) + off;
  }
  VertexTopologyID getEdgeDst(EdgeHandle eh) {
    HalfEdge e = *eh;
    return e.dst;
  }

  /** Data Manipulations **/
  void setData(VertexTopologyID vertex, VertexData data) {
    fmapVoid(getCSR(vertex), setData, vertex, data);
  }
  pando::GlobalRef<VertexData> getData(VertexTopologyID vertex) {
    return fmap(getCSR(vertex), getData, vertex);
  }
  void setEdgeData(EdgeHandle eh, EdgeData data) {
    fmapVoid(getCSR(eh), setEdgeData, eh, data);
  }
  pando::GlobalRef<EdgeData> getEdgeData(EdgeHandle eh) {
    return fmap(getCSR(eh), getEdgeData, eh);
  }

  /** Ranges **/
  VertexRange vertices() {
    return VertexRange{arrayOfCSRs, lift(getCSR(0), vertexEdgeOffsets.begin),
                       lift(getCSR(arrayOfCSRs.size() - 1), vertexEdgeOffsets.end) - 1,
                       numVertices};
  }

  EdgeRange edges(pando::GlobalPtr<galois::Vertex> vPtr) {
    Vertex v = *vPtr;
    Vertex v1 = *(vPtr + 1);
    return RefSpan<galois::HalfEdge>(v.edgeBegin, v1.edgeBegin - v.edgeBegin);
  }

  EdgeRange edges(pando::GlobalPtr<galois::Vertex> vPtr, uint64_t offset_st, uint64_t window_sz) {
    Vertex v = *vPtr;
    Vertex v1 = *(vPtr + 1);

    auto beg = v.edgeBegin + offset_st;
    if (beg > v1.edgeBegin)
      return RefSpan<galois::HalfEdge>(v.edgeBegin, 0);

    auto clipped_window_sz = std::min(window_sz, (uint64_t)(v1.edgeBegin - beg));
    return RefSpan<galois::HalfEdge>(beg, clipped_window_sz);
  }

  VertexDataRange vertexDataRange() noexcept {
    return VertexDataRange{arrayOfCSRs, lift(getCSR(0), vertexData.begin),
                           lift(getCSR(arrayOfCSRs.size() - 1), vertexData.end), numVertices};
  }
  EdgeDataRange edgeDataRange(VertexTopologyID vertex) noexcept {
    return fmap(getCSR(vertex), edgeDataRange, vertex);
  }

  /** Host Information **/
  std::uint64_t getVirtualHostID(VertexTokenID tid) {
    std::uint64_t virtualHostID = tid % this->numVHosts();
    return virtualHostID;
  }

  std::uint64_t getPhysicalHostID(VertexTokenID tid) {
    std::uint64_t virtualHostID = this->getVirtualHostID(tid);
    std::uint64_t physicalHostID = fmap(virtualToPhysicalMap.getLocalRef(), get, virtualHostID);
    return physicalHostID;
  }

  /** Topology Modifications **/
  VertexTopologyID addVertexTopologyOnly(VertexTokenID token) {
    return vertices().end();
  }
  VertexTopologyID addVertex(VertexTokenID token, VertexData data) {
    return vertices().end();
  }
  pando::Status addEdgesTopologyOnly(VertexTopologyID src, pando::Vector<VertexTopologyID> dsts) {
    return addEdges(src, dsts, pando::Vector<EdgeData>());
  }
  pando::Status addEdges(VertexTopologyID src, pando::Vector<VertexTopologyID> dsts,
                         pando::Vector<EdgeData> data) {
    return pando::Status::Error;
  }
  pando::Status deleteEdges(VertexTopologyID src, pando::Vector<EdgeHandle> edges) {
    return pando::Status::Error;
  }

  /**
   * @brief This initializer is used to deal with the outputs of partitioning
   */

  template <typename ReadVertexType, typename ReadEdgeType>
  pando::Status initializeAfterGather(
      galois::HostLocalStorage<pando::Vector<ReadVertexType>> vertexData, std::uint64_t numVertices,
      galois::HostLocalStorage<pando::Vector<pando::Vector<ReadEdgeType>>> edgeData,
      galois::HostLocalStorage<galois::HashTable<std::uint64_t, std::uint64_t>> edgeMap,
      galois::HostIndexedMap<std::uint64_t> numEdges,
      HostLocalStorage<pando::Array<std::uint64_t>> virtualToPhysical) {
    this->virtualToPhysicalMap = virtualToPhysical;
    this->numVertices = numVertices;
    std::uint64_t numHosts = static_cast<std::uint64_t>(pando::getPlaceDims().node.id);
    PANDO_CHECK_RETURN(arrayOfCSRs.initialize());

    galois::WaitGroup wg;
    PANDO_CHECK_RETURN(wg.initialize(numHosts));
    auto wgh = wg.getHandle();

    auto createCSRFuncs = +[](galois::HostLocalStorage<galois::HostIndexedMap<CSR>> arrayOfCSRs,
                              galois::HostLocalStorage<pando::Vector<ReadVertexType>> vertexData,
                              galois::HostIndexedMap<std::uint64_t> numEdges, std::uint64_t i,
                              galois::WaitGroup::HandleType wgh) {
      PANDO_CHECK(lift(arrayOfCSRs.getLocalRef(), initialize));
      CSR currentCSR;
      PANDO_CHECK(
          currentCSR.initializeTopologyMemory(lift(vertexData.getLocalRef(), size), numEdges[i]));

      PANDO_CHECK(
          currentCSR.initializeDataMemory(lift(vertexData.getLocalRef(), size), numEdges[i]));

      std::uint64_t j = 0;
      pando::Vector<ReadVertexType> vertexDataVec = vertexData.getLocalRef();
      for (ReadVertexType data : vertexDataVec) {
        currentCSR.topologyToToken[j] = data.id;
        currentCSR.vertexData[j] = VertexData(data);
        PANDO_CHECK(currentCSR.tokenToTopology.put(data.id, &currentCSR.vertexEdgeOffsets[j]));
        j++;
      }
      lift(arrayOfCSRs.getLocalRef(), getLocalRef) = currentCSR;
      wgh.done();
    };

    for (std::uint64_t i = 0; i < numHosts; i++) {
      pando::Place place = pando::Place{pando::NodeIndex{static_cast<std::int64_t>(i)},
                                        pando::anyPod, pando::anyCore};
      PANDO_CHECK(
          pando::executeOn(place, createCSRFuncs, this->arrayOfCSRs, vertexData, numEdges, i, wgh));
    }
    for (std::uint64_t i = 0; i < numEdges.size(); i++) {
      this->numEdges += numEdges[i];
    }
    PANDO_CHECK_RETURN(wg.wait());
    PANDO_CHECK_RETURN(generateCache());
    wgh.add(numHosts);

    auto fillCSRFuncs =
        +[](DistLocalCSR<VertexType, EdgeType> dlcsr,
            galois::HostLocalStorage<pando::Vector<ReadVertexType>> vertexData,
            galois::HostLocalStorage<pando::Vector<pando::Vector<ReadEdgeType>>> edgeData,
            galois::HostLocalStorage<galois::HashTable<std::uint64_t, std::uint64_t>> edgeMap,
            std::uint64_t i, galois::WaitGroup::HandleType wgh) {
          CSR currentCSR = fmap(dlcsr.arrayOfCSRs[i], operator[], i);
          pando::Vector<pando::Vector<ReadEdgeType>> currEdgeData = edgeData[i];
          std::uint64_t numVertices = lift(vertexData[i], size);
          galois::HashTable<std::uint64_t, std::uint64_t> currEdgeMap = edgeMap[i];
          std::uint64_t edgeCurr = 0;
          currentCSR.vertexEdgeOffsets[0] = Vertex{currentCSR.edgeDestinations.begin()};
          for (std::uint64_t vertexCurr = 0; vertexCurr < numVertices; vertexCurr++) {
            std::uint64_t vertexTokenID =
                currentCSR.getTokenID(&currentCSR.vertexEdgeOffsets[vertexCurr]);

            std::uint64_t edgeMapID;
            if (currEdgeMap.get(vertexTokenID, edgeMapID)) {
              pando::Vector<ReadEdgeType> edges = currEdgeData[edgeMapID];
              std::uint64_t size = edges.size();

              for (std::uint64_t j = 0; j < size; j++, edgeCurr++) {
                HalfEdge e;
                ReadEdgeType eData = edges[j];
                e.dst = dlcsr.getTopologyID(eData.dst);
                currentCSR.edgeDestinations[edgeCurr] = e;
                pando::GlobalPtr<HalfEdge> eh = &currentCSR.edgeDestinations[edgeCurr];
                currentCSR.setEdgeData(eh, EdgeData(static_cast<ReadEdgeType>(edges[j])));
              }
            }
            currentCSR.vertexEdgeOffsets[vertexCurr + 1] =
                Vertex{&currentCSR.edgeDestinations[edgeCurr]};
          }
          fmap(dlcsr.arrayOfCSRs[i], operator[], i) = currentCSR;
          wgh.done();
        };
    for (std::uint64_t i = 0; i < numHosts; i++) {
      pando::Place place = pando::Place{pando::NodeIndex{static_cast<std::int64_t>(i)},
                                        pando::anyPod, pando::anyCore};
      PANDO_CHECK_RETURN(
          pando::executeOn(place, fillCSRFuncs, *this, vertexData, edgeData, edgeMap, i, wgh));
    }

    PANDO_CHECK_RETURN(wg.wait());
    wg.deinitialize();

    return pando::Status::Success;
  }

  template <bool isEdgeList>
  pando::Status initializeAfterImport(galois::PerThreadVector<VertexType>&& localVertices,
                                      galois::PerThreadVector<pando::Vector<EdgeType>>&& localEdges,
                                      uint64_t numVerticesRead) {
    std::uint64_t numHosts = static_cast<std::uint64_t>(pando::getPlaceDims().node.id);
    std::uint64_t hosts = static_cast<std::uint64_t>(pando::getPlaceDims().node.id);
    std::uint16_t scale_factor = 8;
    std::uint64_t numVHosts = numHosts * scale_factor;

    pando::GlobalPtr<pando::Array<galois::Pair<std::uint64_t, std::uint64_t>>> labeledEdgeCounts;
    labeledEdgeCounts =
        PANDO_EXPECT_CHECK(galois::internal::buildEdgeCountToSend<EdgeType>(numVHosts, localEdges));

    auto [v2PM, numEdges] = PANDO_EXPECT_RETURN(
        galois::internal::buildVirtualToPhysicalMapping(hosts, *labeledEdgeCounts));

#if FREE
    auto freeLabeledEdgeCounts =
        +[](pando::Array<galois::Pair<std::uint64_t, std::uint64_t>> labeledEdgeCounts) {
          labeledEdgeCounts.deinitialize();
        };
    PANDO_CHECK_RETURN(pando::executeOn(
        pando::anyPlace, freeLabeledEdgeCounts,
        static_cast<pando::Array<galois::Pair<std::uint64_t, std::uint64_t>>>(*labeledEdgeCounts)));
#endif

    galois::HostIndexedMap<pando::Vector<VertexType>> pHV{};

    if constexpr (isEdgeList) {
      pando::GlobalPtr<galois::HostIndexedMap<pando::Vector<VertexType>>> readPart{};
      pando::LocalStorageGuard readPartGuard(readPart, 1);
      PANDO_CHECK_RETURN(localVertices.hostFlatten((*readPart)));

#if FREE
      auto freeLocalVertices = +[](PerThreadVector<VertexType> localVertices) {
        localVertices.deinitialize();
      };
      PANDO_CHECK_RETURN(pando::executeOn(pando::anyPlace, freeLocalVertices, localVertices));
#endif

      PANDO_CHECK_RETURN(pHV.initialize());
    } else {
      pHV = galois::internal::partitionVerticesParallel(std::move(localVertices), v2PM);
    }

    auto [partEdges, renamePerHost] = internal::partitionEdgesPerHost(std::move(localEdges), v2PM);

    std::uint64_t numVertices = 0;
    if constexpr (isEdgeList) {
      for (uint64_t h = 0; h < numHosts; h++) {
        PANDO_CHECK(fmap(pHV[h], initialize, 0));
      }
      struct PHPV {
        HostIndexedMap<pando::Vector<pando::Vector<EdgeType>>> partEdges;
        HostIndexedMap<pando::Vector<VertexType>> pHV;
      };
      PHPV phpv{partEdges, pHV};
      galois::doAllEvenlyPartition(
          phpv, numHosts, +[](PHPV phpv, uint64_t host_id, uint64_t) {
            pando::Vector<pando::Vector<EdgeType>> edgeVec = phpv.partEdges[host_id];
            pando::GlobalRef<pando::Vector<VertexType>> vertexVec = phpv.pHV[host_id];
            for (pando::Vector<EdgeType> vec : edgeVec) {
              EdgeType e = vec[0];
              VertexType v = VertexType(e.src, agile::TYPES::NONE);
              PANDO_CHECK(fmap(vertexVec, pushBack, v));
            }
          });

      for (uint64_t h = 0; h < numHosts; h++) {
        numVertices += lift(pHV[h], size);
      }
    } else {
      numVertices = numVerticesRead;
    }

    PANDO_CHECK_RETURN(
        initializeAfterGather(pHV, numVertices, partEdges, renamePerHost, numEdges,
                              PANDO_EXPECT_CHECK(galois::copyToAllHosts(std::move(v2PM)))));
#if FREE
    auto freeTheRest = +[](decltype(pHV) pHV, decltype(partEdges) partEdges,
                           decltype(renamePerHost) renamePerHost, decltype(numEdges) numEdges) {
      for (pando::Vector<VertexType> vV : pHV) {
        vV.deinitialize();
      }
      pHV.deinitialize();
      for (pando::Vector<pando::Vector<EdgeType>> vVE : partEdges) {
        for (pando::Vector<EdgeType> vE : vVE) {
          vE.deinitialize();
        }
        vVE.deinitialize();
      }
      partEdges.deinitialize();
      renamePerHost.deinitialize();
      numEdges.deinitialize();
    };

    PANDO_CHECK_RETURN(
        pando::executeOn(pando::anyPlace, freeTheRest, pHV, partEdges, renamePerHost, numEdges));
#endif
    return pando::Status::Success;
  }

  /**
   * @brief This function creates a mirror list for each host. Currently it implements full
   * mirroring
   */
  template <typename ReadEdgeType>
  HostLocalStorage<pando::Vector<std::uint64_t>> getMirrorList(
      HostLocalStorage<pando::Vector<pando::Vector<ReadEdgeType>>> partEdges,
      HostLocalStorage<pando::Array<std::uint64_t>> V2PM) {
    HostLocalStorage<pando::Vector<std::uint64_t>> mirrorList;
    PANDO_CHECK(mirrorList.initialize());
    auto createMirrors = +[](HostLocalStorage<pando::Vector<pando::Vector<ReadEdgeType>>> partEdges,
                             HostLocalStorage<pando::Vector<std::uint64_t>> mirrorList,
                             HostLocalStorage<pando::Array<std::uint64_t>> V2PM, std::uint64_t i,
                             galois::WaitGroup::HandleType wgh) {
      pando::Vector<uint64_t> mirrors;
      PANDO_CHECK(mirrors.initialize(0));

      // Populating the mirror list in a hashtable to avoid duplicates
      galois::HashTable<uint64_t, bool> mirrorMap;
      // initialize hashtable to size 1?
      PANDO_CHECK(mirrorMap.initialize(1));
      pando::Array<uint64_t> localV2PM = V2PM.getLocalRef();
      for (std::uint64_t k = 0; k < lift(partEdges.getLocalRef(), size); k++) {
        pando::Vector<ReadEdgeType> currentEdge = fmap(partEdges.getLocalRef(), get, k);
        for (ReadEdgeType tmp : currentEdge) {
          std::uint64_t dstVHost = tmp.dst % localV2PM.size();
          std::uint64_t dstPHost = fmap(localV2PM, get, dstVHost);
          if (dstPHost != i) {
            // not in mirror list yet
            if (mirrorMap.contains(tmp.dst) == false) {
              mirrorMap.put(tmp.dst, true);
              PANDO_CHECK(mirrors.pushBack(tmp.dst));
            }
          }
        }
      }

      mirrorList.getLocalRef() = mirrors;
      wgh.done();
    };

    std::uint64_t numHosts = static_cast<std::uint64_t>(pando::getPlaceDims().node.id);
    galois::WaitGroup wg;
    PANDO_CHECK(wg.initialize(numHosts));
    auto wgh = wg.getHandle();
    for (std::uint64_t i = 0; i < numHosts; i++) {
      pando::Place place = pando::Place{pando::NodeIndex{static_cast<std::int64_t>(i)},
                                        pando::anyPod, pando::anyCore};
      PANDO_CHECK(pando::executeOn(place, createMirrors, partEdges, mirrorList, V2PM, i, wgh));
    }
    PANDO_CHECK(wg.wait());
    return mirrorList;
  }

  /**
   * @brief This initializer for workflow 4's edge lists
   */
  pando::Status initializeWMD(pando::Vector<galois::EdgeParser<EdgeType>>&& edgeParsers,
                              const uint64_t chunk_size = 10000, const uint64_t scale_factor = 8) {
    std::uint64_t numHosts = static_cast<std::uint64_t>(pando::getPlaceDims().node.id);
    std::uint64_t numVHosts = numHosts * scale_factor;
    galois::ThreadLocalVector<EdgeType> localEdges{};
    PANDO_CHECK_RETURN(localEdges.initialize());

    for (galois::EdgeParser<EdgeType> parser : edgeParsers) {
      galois::ifstream graphFile;
      PANDO_CHECK_RETURN(graphFile.open(parser.filename));
      uint64_t fileSize = graphFile.size();
      uint64_t segments = (fileSize / chunk_size) + 1;
      graphFile.close();
      PANDO_CHECK_RETURN(
          galois::doAllEvenlyPartition(internal::ImportState<EdgeType>(parser, localEdges),
                                       segments, &internal::loadGraphFile<EdgeType>));
    }

    edgeParsers.deinitialize();

    pando::Array<galois::Pair<std::uint64_t, std::uint64_t>> labeledEdgeCounts =
        PANDO_EXPECT_CHECK(galois::internal::buildEdgeCountToSend<EdgeType>(numVHosts, localEdges));

    auto [v2PM, numEdges] = PANDO_EXPECT_RETURN(
        galois::internal::buildVirtualToPhysicalMapping(numHosts, labeledEdgeCounts));

    galois::HostLocalStorage<pando::Vector<pando::Vector<EdgeType>>> partEdges{};
    PANDO_CHECK_RETURN(partEdges.initialize());

    for (pando::GlobalRef<pando::Vector<pando::Vector<EdgeType>>> vvec : partEdges) {
      PANDO_CHECK_RETURN(fmap(vvec, initialize, 0));
    }

    galois::HostLocalStorage<galois::HashTable<std::uint64_t, std::uint64_t>> renamePerHost{};
    PANDO_CHECK_RETURN(renamePerHost.initialize());

    PANDO_CHECK_RETURN(galois::internal::partitionEdgesSerially<EdgeType>(
        localEdges, v2PM, partEdges, renamePerHost));
    galois::HostLocalStorage<pando::Vector<VertexType>> pHV{};
    PANDO_CHECK_RETURN(pHV.initialize());

    PANDO_CHECK_RETURN(galois::doAll(
        partEdges, pHV,
        +[](HostLocalStorage<pando::Vector<pando::Vector<EdgeType>>> partEdges,
            pando::GlobalRef<pando::Vector<VertexType>> pHV) {
          PANDO_CHECK(fmap(pHV, initialize, 0));
          pando::Vector<pando::Vector<EdgeType>> localEdges = partEdges.getLocalRef();
          for (pando::Vector<EdgeType> e : localEdges) {
            EdgeType e0 = e[0];
            VertexType v0 = VertexType(e0.src, e0.srcType);
            PANDO_CHECK(fmap(pHV, pushBack, v0));
          }
        }));
    uint64_t srcVertices = 0;
    for (pando::Vector<VertexType> hostVertices : pHV) {
      srcVertices += hostVertices.size();
    }

    return initializeAfterGather(pHV, srcVertices, partEdges, renamePerHost, numEdges,
                                 PANDO_EXPECT_CHECK(galois::copyToAllHosts(std::move(v2PM))));
  }

  /**
   * @brief Creates a DistLocalCSR from an explicit graph definition, intended only for tests
   *
   * @param[in] vertices This is a vector of vertex values.
   * @param[in] edges This is vector global src id, dst id, and edge data.
   */
  [[nodiscard]] pando::Status initialize(pando::Vector<VertexType> vertices,
                                         pando::Vector<GenericEdge<EdgeType>> edges) {
    numVertices = vertices.size();
    numEdges = edges.size();
    PANDO_CHECK_RETURN(arrayOfCSRs.initialize());
    pando::Array<std::uint64_t> v2PM;
    PANDO_CHECK_RETURN(v2PM.initialize(vertices.size()));
    std::uint64_t hosts = static_cast<std::uint64_t>(pando::getPlaceDims().node.id);
    uint64_t verticesPerHost = numVertices / hosts;
    if (hosts * verticesPerHost < numVertices) {
      verticesPerHost++;
    }
    pando::Vector<uint64_t> edgeCounts;
    PANDO_CHECK_RETURN(edgeCounts.initialize(verticesPerHost));
    uint64_t edgesStart = 0;
    for (uint64_t host = 0; host < hosts; host++) {
      uint64_t vertex;
      uint64_t edgesEnd = edgesStart;
      for (vertex = 0; vertex < verticesPerHost && vertex + host * verticesPerHost < numVertices;
           vertex++) {
        uint64_t currLocalVertex = vertex + host * verticesPerHost;
        v2PM[currLocalVertex] = host;
        uint64_t vertexEdgeStart = edgesEnd;
        for (; edgesEnd < numEdges && (&edges[edgesEnd])->src <= (&vertices[currLocalVertex])->id;
             edgesEnd++) {}
        edgeCounts[vertex] = edgesEnd - vertexEdgeStart;
      }
      CSR currentCSR{};
      uint64_t numLocalEdges = edgesEnd - edgesStart;
      PANDO_CHECK(currentCSR.initializeTopologyMemory(
          vertex, numLocalEdges,
          pando::Place{pando::NodeIndex{(int64_t)host}, pando::anyPod, pando::anyCore},
          pando::MemoryType::Main));
      PANDO_CHECK(currentCSR.initializeDataMemory(
          vertex, numLocalEdges,
          pando::Place{pando::NodeIndex{(int64_t)host}, pando::anyPod, pando::anyCore},
          pando::MemoryType::Main));

      uint64_t currLocalEdge = 0;
      for (uint64_t v = 0; v < vertex; v++) {
        uint64_t currLocalVertex = v + host * verticesPerHost;
        VertexData data = vertices[currLocalVertex];
        currentCSR.topologyToToken[v] = data.id;
        currentCSR.vertexData[v] = data;
        currentCSR.vertexEdgeOffsets[v] = Vertex{&currentCSR.edgeDestinations[currLocalEdge]};
        PANDO_CHECK_RETURN(
            currentCSR.tokenToTopology.put(data.id, &currentCSR.vertexEdgeOffsets[v]));
        currLocalEdge += edgeCounts[v];
      }
      currentCSR.vertexEdgeOffsets[vertex] = Vertex{&currentCSR.edgeDestinations[currLocalEdge]};

      PANDO_CHECK(fmap(arrayOfCSRs[host], initialize,
                       pando::Place{pando::NodeIndex{(int64_t)host}, pando::anyPod, pando::anyCore},
                       pando::MemoryType::Main));
      fmap(arrayOfCSRs[host], operator[], host) = currentCSR;
      edgesStart = edgesEnd;
    }
    edgeCounts.deinitialize();
    PANDO_CHECK_RETURN(generateCache());
    virtualToPhysicalMap = PANDO_EXPECT_RETURN(galois::copyToAllHosts(std::move(v2PM)));

    edgesStart = 0;
    for (uint64_t host = 0; host < hosts; host++) {
      CSR currentCSR = fmap(arrayOfCSRs[host], operator[], host);

      uint64_t lastLocalVertexIndex = verticesPerHost * (host + 1) - 1;
      if (lastLocalVertexIndex >= numVertices) {
        lastLocalVertexIndex = numVertices - 1;
      }
      uint64_t lastLocalVertex = (&vertices[lastLocalVertexIndex])->id;

      uint64_t currLocalEdge = 0;
      GenericEdge<EdgeType> currEdge = edges[edgesStart];
      for (; edgesStart + currLocalEdge < numEdges && currEdge.src <= lastLocalVertex;
           currLocalEdge++) {
        EdgeType data = currEdge.data;
        HalfEdge edge{};
        edge.dst = getTopologyID(currEdge.dst);
        currentCSR.edgeDestinations[currLocalEdge] = edge;
        currentCSR.setEdgeData(&currentCSR.edgeDestinations[currLocalEdge], data);

        if (currLocalEdge + edgesStart < numEdges - 1) {
          currEdge = edges[edgesStart + currLocalEdge + 1];
        }
      }
      fmap(arrayOfCSRs[host], operator[], host) = currentCSR;

      edgesStart += currLocalEdge;
    }
    PANDO_CHECK_RETURN(generateCache());
    return pando::Status::Success;
  }

  /**
   * @brief Creates a DistArrayCSR from an explicit graph definition, intended only for tests
   *
   * @param[in] vertices This is a vector of vertex values with TokenIDs in an `id` field.
   * @param[in] edges This is a vector of edge data.
   * @param[in] edgeDsts This is a vector of token dst id
   * @param[in] edgeCounts This is a vector of edge counts
   * @warning Edges must be ordered by vertex, but vertex IDs may not contiguous
   */
  template <typename OldGraph>
  [[nodiscard]] pando::Status initialize(OldGraph& oldGraph,
                                         galois::PerThreadVector<VertexType> vertices,
                                         galois::PerThreadVector<EdgeType> edges,
                                         galois::PerThreadVector<VertexTokenID> edgeDsts,
                                         galois::PerThreadVector<uint64_t> edgeCounts) {
    numVertices = vertices.sizeAll();
    numEdges = edges.sizeAll();
    pando::Array<std::uint64_t> oldV2PM = oldGraph.virtualToPhysicalMap.getLocalRef();
    pando::Array<std::uint64_t> v2PM;
    PANDO_CHECK_RETURN(v2PM.initialize(oldV2PM.size()));
    for (uint64_t i = 0; i < oldV2PM.size(); i++) {
      v2PM[i] = oldV2PM[i];
    }
    this->virtualToPhysicalMap = PANDO_EXPECT_RETURN(galois::copyToAllHosts(std::move(v2PM)));

    std::uint64_t hosts = static_cast<std::uint64_t>(pando::getPlaceDims().node.id);
    PANDO_CHECK_RETURN(arrayOfCSRs.initialize());
    PANDO_CHECK_RETURN(vertices.computeIndices());
    PANDO_CHECK_RETURN(edges.computeIndices());
    PANDO_CHECK_RETURN(edgeDsts.computeIndices());
    internal::DLCSR_InitializeState<VertexType, EdgeType> state(arrayOfCSRs, vertices, edges,
                                                                edgeCounts);
    galois::doAllEvenlyPartition(
        state, hosts,
        +[](internal::DLCSR_InitializeState<VertexType, EdgeType>& state, std::uint64_t host,
            uint64_t hosts) {
          CSR currentCSR{};
          uint64_t numLocalVertices;
          uint64_t numLocalEdges;
          PANDO_CHECK(state.vertices.localElements(numLocalVertices));
          PANDO_CHECK(state.edges.localElements(numLocalEdges));
          PANDO_CHECK(currentCSR.initializeTopologyMemory(numLocalVertices, numLocalEdges));
          PANDO_CHECK(currentCSR.initializeDataMemory(numLocalVertices, numLocalEdges));

          uint64_t currLocalVertex = 0;
          uint64_t currLocalEdge = 0;
          const uint64_t numLocalVectors = state.vertices.size() / hosts;
          for (uint64_t i = host * numLocalVectors; i < (host + 1) * numLocalVectors; i++) {
            pando::Vector<VertexData> vertexData = state.vertices[i];
            pando::Vector<uint64_t> edgeCounts = state.edgeCounts[i];
            for (uint64_t j = 0; j < vertexData.size(); j++) {
              VertexData data = vertexData[j];
              currentCSR.topologyToToken[currLocalVertex] = data.id;
              currentCSR.vertexData[currLocalVertex] = data;
              currentCSR.vertexEdgeOffsets[currLocalVertex] =
                  Vertex{&currentCSR.edgeDestinations[currLocalEdge]};
              PANDO_CHECK(currentCSR.tokenToTopology.put(
                  data.id, &currentCSR.vertexEdgeOffsets[currLocalVertex]));
              currLocalVertex++;
              currLocalEdge += edgeCounts[j];
            }
          }
          currentCSR.vertexEdgeOffsets[currLocalVertex] =
              Vertex{&currentCSR.edgeDestinations[currLocalEdge]};
          PANDO_CHECK(lift(state.arrayOfCSRs.getLocalRef(), initialize));
          lift(state.arrayOfCSRs.getLocalRef(), getLocalRef) = currentCSR;
        });
    arrayOfCSRs = state.arrayOfCSRs;
    PANDO_CHECK_RETURN(generateCache());

    InitializeEdgeState state2(*this, edges, edgeDsts);
    galois::onEach(
        state2, +[](InitializeEdgeState& state, uint64_t thread, uint64_t) {
          uint64_t host = static_cast<std::uint64_t>(pando::getCurrentNode().id);
          CSR currentCSR = state.dlcsr.getCSR(host);

          uint64_t hostOffset;
          PANDO_CHECK(state.edges.currentHostIndexOffset(hostOffset));
          uint64_t threadOffset;
          PANDO_CHECK(state.edges.indexOnThread(thread, threadOffset));
          threadOffset -= hostOffset;

          uint64_t currLocalEdge = 0;
          pando::Vector<EdgeData> edgeData = state.edges[thread];
          pando::Vector<VertexTokenID> edgeDsts = state.edgeDsts[thread];
          for (EdgeData data : edgeData) {
            HalfEdge edge;
            edge.dst = state.dlcsr.getTopologyID(edgeDsts[currLocalEdge]);
            currentCSR.edgeDestinations[threadOffset + currLocalEdge] = edge;
            currentCSR.setEdgeData(&currentCSR.edgeDestinations[threadOffset + currLocalEdge],
                                   data);
            currLocalEdge++;
          }
          lift(state.dlcsr.arrayOfCSRs.getLocalRef(), getLocalRef) = currentCSR;
        });
    *this = state2.dlcsr;
    PANDO_CHECK_RETURN(generateCache());

    return pando::Status::Success;
  }

  pando::Status initialize(pando::Vector<galois::VertexParser<VertexType>>&& vertexParsers,
                           pando::Vector<galois::EdgeParser<EdgeType>>&& edgeParsers) {
    std::uint64_t numThreads = 32;
    galois::PerThreadVector<pando::Vector<EdgeType>> localEdges{};
    PANDO_CHECK_RETURN(localEdges.initialize());

    galois::ThreadLocalStorage<galois::HashTable<std::uint64_t, std::uint64_t>> perThreadRename{};
    PANDO_CHECK(perThreadRename.initialize());

    for (auto hashRef : perThreadRename) {
      hashRef = galois::HashTable<std::uint64_t, std::uint64_t>{};
      PANDO_CHECK(fmap(hashRef, initialize, 0));
    }

    galois::PerThreadVector<VertexType> localVertices{};
    PANDO_CHECK_RETURN(localVertices.initialize());

    for (VertexParser<VertexType> vertexParser : vertexParsers) {
      pando::NotificationArray dones;
      PANDO_CHECK_RETURN(dones.init(numThreads));

      std::uint64_t hosts = static_cast<std::uint64_t>(pando::getPlaceDims().node.id);
      for (std::uint64_t i = 0; i < numThreads; i++) {
        pando::Place place = pando::Place{pando::NodeIndex{static_cast<std::int64_t>(i % hosts)},
                                          pando::anyPod, pando::anyCore};
        PANDO_CHECK_RETURN(
            pando::executeOn(place, &galois::internal::loadVertexFilePerThread<VertexType>,
                             dones.getHandle(i), vertexParser, 1, numThreads, i, localVertices));
      }
      dones.wait();
    }
    for (galois::EdgeParser<EdgeType> edgeParser : edgeParsers) {
      pando::NotificationArray dones;
      PANDO_CHECK_RETURN(dones.init(numThreads));

      std::uint64_t hosts = static_cast<std::uint64_t>(pando::getPlaceDims().node.id);
      for (std::uint64_t i = 0; i < numThreads; i++) {
        pando::Place place = pando::Place{pando::NodeIndex{static_cast<std::int64_t>(i % hosts)},
                                          pando::anyPod, pando::anyCore};
        PANDO_CHECK_RETURN(pando::executeOn(
            place, &galois::internal::loadEdgeFilePerThread<EdgeType>, dones.getHandle(i),
            edgeParser, 1, numThreads, i, localEdges, perThreadRename));
      }
      dones.wait();
    }

    vertexParsers.deinitialize();
    edgeParsers.deinitialize();

#ifdef FREE
    auto freePerThreadRename =
        +[](galois::ThreadLocalStorage<galois::HashTable<std::uint64_t, std::uint64_t>>
                perThreadRename) {
          for (galois::HashTable<std::uint64_t, std::uint64_t> hash : perThreadRename) {
            hash.deinitialize();
          }
        };
    PANDO_CHECK(pando::executeOn(pando::anyPlace, freePerThreadRename, perThreadRename));
    perThreadRename.deinitialize();
#endif

    const bool isEdgeList = false;
    PANDO_CHECK_RETURN(initializeAfterImport<isEdgeList>(
        std::move(localVertices), std::move(localEdges), localVertices.sizeAll()));
    return pando::Status::Success;
  }

  /**
   * @brief get vertex local dense ID
   */
  std::uint64_t getVertexLocalIndex(VertexTopologyID vertex) {
    std::uint64_t hostNum = static_cast<std::uint64_t>(galois::localityOf(vertex).node.id);
    return fmap(arrayOfCSRs[hostNum], getVertexIndex, vertex);
  }

  /**
   * @brief gives the number of vertices
   */

  std::uint64_t localSize(std::uint64_t host) noexcept {
    return lift(getCSR(host), size);
  }

  /**
   * @brief Sets the value of the edge provided
   */
  void setEdgeData(VertexTopologyID vertex, std::uint64_t off, EdgeData data) {
    setEdgeData(mintEdgeHandle(vertex, off), data);
  }

  /**
   * @brief gets the reference to the vertex provided
   */
  pando::GlobalRef<EdgeData> getEdgeData(VertexTopologyID vertex, std::uint64_t off) {
    return getEdgeData(mintEdgeHandle(vertex, off));
  }

  /**
   * @brief get the vertex at the end of the edge provided by vertex at the offset from the start
   */
  VertexTopologyID getEdgeDst(VertexTopologyID vertex, std::uint64_t off) {
    return getEdgeDst(mintEdgeHandle(vertex, off));
  }

  bool isLocal(VertexTopologyID vertex) {
    return getLocalityVertex(vertex).node.id == pando::getCurrentPlace().node.id;
  }

  bool isOwned(VertexTopologyID vertex) {
    return isLocal(vertex);
  }

  /**
   * @brief Get the local csr
   */
  pando::GlobalRef<CSR> getLocalCSR() {
    std::uint64_t nodeIdx = static_cast<std::uint64_t>(pando::getCurrentPlace().node.id);
    return getCSR(nodeIdx);
  }

  /**
   * @brief Get the local csr on a specific host
   */
  pando::GlobalRef<CSR> getCSR(std::uint64_t hostID) {
    return getCachedCSR(arrayOfCSRs, hostID);
  }

  /**
   * @brief create CSR Caches
   */
  pando::Status generateCache() {
    return galois::doAll(
        arrayOfCSRs, arrayOfCSRs,
        +[](decltype(arrayOfCSRs) arrayOfCSRs, HostIndexedMap<CSR> localCSRs) {
          for (std::uint64_t i = 0; i < localCSRs.size(); i++) {
            if (i == static_cast<std::uint64_t>(pando::getCurrentPlace().node.id)) {
              continue;
            }
            localCSRs[i] = fmap(arrayOfCSRs[i], operator[], i);
          }
        });
  }

private:
  HostLocalStorage<HostIndexedMap<CSR>> arrayOfCSRs;
  std::uint64_t numVertices;
  std::uint64_t numEdges;
  galois::HostLocalStorage<pando::Array<std::uint64_t>> virtualToPhysicalMap;
};

static_assert(graph_checker<DistLocalCSR<std::uint64_t, std::uint64_t>>::value);
static_assert(graph_checker<DistLocalCSR<WMDVertex, WMDEdge>>::value);

} // namespace galois

#endif // PANDO_LIB_GALOIS_GRAPHS_DIST_LOCAL_CSR_HPP_
