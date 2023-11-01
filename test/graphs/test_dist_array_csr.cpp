// SPDX-License-Identifier: MIT
// Copyright (c) 2023. University of Texas at Austin. All rights reserved.

#include <gtest/gtest.h>

#include <variant>

#include "pando-rt/export.h"
#include <pando-lib-galois/containers/dist_array.hpp>
#include <pando-lib-galois/graphs/dist_array_csr.hpp>
#include <pando-lib-galois/loops/do_all.hpp>
#include <pando-rt/containers/vector.hpp>
#include <pando-rt/pando-rt.hpp>
#include <pando-rt/sync/notification.hpp>

TEST(DistArrayCSR, FullyConnected) {
  // This paradigm is necessary because the CP system is not part of the PGAS system
  pando::Notification necessary;
  EXPECT_EQ(necessary.init(), pando::Status::Success);

  auto f = +[](pando::Notification::HandleType done) {
    constexpr std::uint64_t SIZE = 100;
    galois::DistArray<std::uint64_t> array;
    pando::Vector<pando::Vector<std::uint64_t>> vec;
    EXPECT_EQ(vec.initialize(SIZE), pando::Status::Success);
    for (pando::GlobalRef<pando::Vector<std::uint64_t>> edges : vec) {
      pando::Vector<std::uint64_t> inner;
      EXPECT_EQ(inner.initialize(0), pando::Status::Success);
      edges = inner;
    }

    galois::doAll(
        std::monostate(), vec,
        +[](std::monostate _s, pando::GlobalRef<pando::Vector<std::uint64_t>> innerRef) {
          (void)_s;
          pando::Vector<std::uint64_t> inner = innerRef;
          for (std::uint64_t i = 0; i < SIZE; i++) {
            EXPECT_EQ(inner.pushBack(i), pando::Status::Success);
          }
          innerRef = inner;
        });
    galois::DistArrayCSR<std::uint64_t> graph;
    graph.initialize(vec);
    auto err = galois::doAll(
        std::monostate(), vec,
        +[](std::monostate _s, pando::GlobalRef<pando::Vector<std::uint64_t>> innerRef) {
          (void)_s;
          pando::Vector<std::uint64_t> inner = innerRef;
          inner.deinitialize();
          innerRef = inner;
        });
    vec.deinitialize();
    EXPECT_EQ(err, pando::Status::Success);
    for (std::uint64_t i = 0; i < SIZE; i++) {
      EXPECT_EQ(graph.getNumEdges(i), SIZE);
      for (std::uint64_t j = 0; j < SIZE; j++) {
        EXPECT_EQ(graph.getEdgeDst(i, j), j);
      }
    }
    graph.deinitialize();

    done.notify();
  };

  EXPECT_EQ(pando::executeOn(pando::Place{pando::NodeIndex{0}, pando::anyPod, pando::anyCore}, f,
                             necessary.getHandle()),
            pando::Status::Success);
  necessary.wait();
}
