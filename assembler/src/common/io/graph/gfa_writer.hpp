//***************************************************************************
//* Copyright (c) 2018 Saint Petersburg State University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************

#pragma once

#include "assembly_graph/core/graph.hpp"
#include "assembly_graph/components/graph_component.hpp"
#include "io/utils/edge_namer.hpp"
#include "io/utils/id_mapper.hpp"

#include <memory>
#include <string>
#include <ostream>

namespace gfa {

class GFAWriter {
  protected:
    typedef debruijn_graph::DeBruijnGraph Graph;
    typedef omnigraph::GraphComponent<Graph> Component;

public:
    GFAWriter(const Graph &graph, std::ostream &os,
              io::EdgeNamingF<Graph> naming_f = io::IdNamingF<Graph>())
            : graph_(graph),
              edge_namer_(graph_, naming_f),
              os_(os) {
    }

    void WriteSegmentsAndLinks() {
        WriteSegments();
        WriteLinks();
    }

    void WriteSegmentsAndLinks(const Component &gc) {
        //todo remove and add optional check?
        auto rc_closure = Component::FromEdges(graph_, gc.edges().begin(),
                                               gc.edges().end(), /*add_conjugate*/true);
        WriteSegments(rc_closure);
        WriteLinks(rc_closure);
    }

  private:
    void WriteSegments();
    void WriteLinks();

    void WriteSegments(const Component &gc);
    void WriteLinks(const Component &gc);

  protected:
    const Graph &graph_;
    io::CanonicalEdgeHelper<Graph> edge_namer_;
    std::ostream &os_;

};

}

