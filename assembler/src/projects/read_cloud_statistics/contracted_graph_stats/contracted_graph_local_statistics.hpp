#pragma once
#include "cluster_storage_analyzer.hpp"

namespace contracted_graph {
    struct LoopPathClusterStatistics: public read_cloud_statistics::Statistic {
        size_t correct_events_;
        size_t missed_events_;
        size_t correct_covered_;
        size_t overall_transitions_;

      LoopPathClusterStatistics()
          : Statistic("loop_path_cluster_statistic"),
            correct_events_(0),
            missed_events_(0),
            correct_covered_(0),
            overall_transitions_(0) {}

      void Serialize(const string& path) override {
          ofstream fout(path);
          string sep = "\t";
          fout << "Correct events" << sep << "Missed events" << sep << "Covered targets" << sep << "Number of targets" << std::endl;
          fout << correct_events_ << sep << missed_events_ << sep << correct_covered_ << sep << overall_transitions_ << std::endl;
      }
    };

    class ContractedGraphClusterStatistics: public read_cloud_statistics::StatisticProcessor {
     public:
        typedef cluster_storage::Cluster Cluster;
        typedef transitions::Transition Transition;
     private:
        const Graph& g_;
        const ContractedGraph& contracted_graph_;
        const cluster_storage::ClusterGraphAnalyzer ordering_analyzer_;
        const vector<Cluster>& clusters_;
        const transitions::ContigTransitionStorage& reference_storage_;
     public:
        ContractedGraphClusterStatistics(const Graph& g_,
                                         const ContractedGraph& contracted_graph_,
                                         const cluster_storage::ClusterGraphAnalyzer& ordering_analyzer_,
                                         const vector<Cluster>& clusters_,
                                         const transitions::ContigTransitionStorage& reference_storage_)
            : StatisticProcessor("contracted_cluster_statistics"),
              g_(g_),
              contracted_graph_(contracted_graph_),
              ordering_analyzer_(ordering_analyzer_),
              clusters_(clusters_),
              reference_storage_(reference_storage_) {}

        void FillStatistics() override {
            cluster_storage::ClusterStorageExtractor cluster_extractor;
            auto path_cluster_filter = make_shared<cluster_storage::PathClusterFilter>(ordering_analyzer_);
            auto path_clusters = cluster_extractor.FilterClusters(clusters_, path_cluster_filter);
            auto loop_path_stats = make_shared<LoopPathClusterStatistics>(GetLoopPathClusterStatistics(path_clusters));
            AddStatistic(loop_path_stats);
        }

     private:

        struct LoopNeighbourhood {
          EdgeId loop_;
          EdgeId prev_;
          EdgeId next_;

          LoopNeighbourhood(const EdgeId& loop_, const EdgeId& prev_, const EdgeId& next_)
              : loop_(loop_), prev_(prev_), next_(next_) {}
        };

        LoopPathClusterStatistics GetLoopPathClusterStatistics (const vector<Cluster>& clusters) const {
            DEBUG("Extracting loop edges")
            auto loop_edges = ExtractLoopEdges(contracted_graph_);
            DEBUG(loop_edges.size() << " loop edges");
            auto edge_to_loop_neighbourhood = GetEdgeToLoopNeighbourhood(loop_edges, reference_storage_);
            DEBUG("Edge to loop neighbourhood size: " << edge_to_loop_neighbourhood.size());

            LoopPathClusterStatistics loop_path_stats;

            unordered_set<transitions::Transition> covered_targets;
            auto target_transitions = GetTargetTransitions(edge_to_loop_neighbourhood);
            DEBUG(target_transitions.size() << " target transitions");
            for (const auto& cluster: clusters) {
                transitions::ClusterTransitionExtractor transition_extractor(ordering_analyzer_);
                auto transitions = transition_extractor.ExtractTransitionsFromPathCluster(cluster);
                for (const auto& transition: transitions) {
                    UpdateLoopPathStats(loop_path_stats, covered_targets, edge_to_loop_neighbourhood,
                                        target_transitions, transition);
                }
            }
            loop_path_stats.correct_covered_ = covered_targets.size();
            loop_path_stats.overall_transitions_ = target_transitions.size();
            DEBUG("Finished loop path statistics extraction");
            return loop_path_stats;
        };

        void UpdateLoopPathStats(LoopPathClusterStatistics& statistics,
                                 unordered_set<Transition>& covered_targets,
                                 const unordered_map<EdgeId, LoopNeighbourhood>& edge_to_loop_neighbourhood,
                                 const unordered_set<Transition>& target_transitions,
                                 const Transition& transition) const {
            EdgeId first = transition.first_;
            EdgeId second = transition.second_;
            if (target_transitions.find(transition) != target_transitions.end()) {
                covered_targets.insert(transition);
            }
            if (edge_to_loop_neighbourhood.find(first) != edge_to_loop_neighbourhood.end()) {
                auto neighbourhood = edge_to_loop_neighbourhood.at(first);
                EdgeId prev = neighbourhood.prev_;
                EdgeId loop = neighbourhood.loop_;
                EdgeId next = neighbourhood.next_;
                if (first == prev and second == next) {
                    ++statistics.missed_events_;
                }
                if ((first == prev and second == loop) or (first == loop and second == next)) {
                    ++statistics.correct_events_;
                }
            }
        }

        unordered_set<EdgeId> ExtractLoopEdges(const ContractedGraph& contracted_graph) const {
            unordered_set<EdgeId> result;
            for (const auto& vertex: contracted_graph) {
                for (auto it = contracted_graph.outcoming_begin(vertex); it != contracted_graph.outcoming_end(vertex); ++it) {
                    if (it->first == vertex) {
                        for (const auto& edge: it->second) {
                            result.insert(edge);
                        }
                    }
                }
            }
            return result;
        }

        unordered_map<EdgeId, LoopNeighbourhood>
        GetEdgeToLoopNeighbourhood(unordered_set<EdgeId>& loops,
                                   const transitions::ContigTransitionStorage& reference_storage) const {
            unordered_map<EdgeId, EdgeId> loop_to_prev;
            unordered_map<EdgeId, EdgeId> loop_to_next;
            unordered_map<EdgeId, LoopNeighbourhood> loop_to_neighbourhood;
            for (const auto& transition: reference_storage) {
                EdgeId first = transition.first_;
                EdgeId second = transition.second_;
                if (loops.find(first) != loops.end()) {
                    loop_to_next[first] = second;
                }
                if (loops.find(second) != loops.end()) {
                    loop_to_prev[second] = first;
                }
            }
            DEBUG("Loop to prev size: " << loop_to_prev.size());
            DEBUG("Loop to next size: " << loop_to_next.size());
            for (const auto& entry: loop_to_prev) {
                EdgeId loop = entry.first;
                if (loop_to_next.find(loop) != loop_to_next.end()) {
                    EdgeId prev = loop_to_prev.at(loop);
                    EdgeId next = loop_to_next.at(loop);
                    LoopNeighbourhood loop_neighbourhood(loop, prev, next);
                    loop_to_neighbourhood.insert({loop, loop_neighbourhood});
                    loop_to_neighbourhood.insert({prev, loop_neighbourhood});
                    loop_to_neighbourhood.insert({next, loop_neighbourhood});
                }
            }
            return loop_to_neighbourhood;
        }

        unordered_set<transitions::Transition> GetTargetTransitions(unordered_map<EdgeId, LoopNeighbourhood>& edge_to_neighbourhood) const {
            unordered_set<transitions::Transition> target_transitions;
            for (const auto& entry: edge_to_neighbourhood) {
                auto neighbourhood = entry.second;
                target_transitions.emplace(neighbourhood.prev_, neighbourhood.loop_);
                target_transitions.emplace(neighbourhood.loop_, neighbourhood.next_);
            }
            return target_transitions;
        }
        DECL_LOGGER("ContractedGraphClusterStatistics");
    };
}