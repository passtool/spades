//***************************************************************************
//* Copyright (c) 2015 Saint Petersburg State University
//* Copyright (c) 2011-2014 Saint Petersburg Academic University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************

#ifndef LONG_READ_MAPPER_HPP_
#define LONG_READ_MAPPER_HPP_

#include "long_read_storage.hpp"
#include "sequence_mapper_notifier.hpp"

#include <cassert>
#include <mutex>
#include <iomanip>

namespace debruijn_graph {

//todo: extend interface of Path<EdgeId> and replace to its
struct PathWithMappingInfo { 
    using PathType = std::vector<EdgeId>;
    PathType Path_;
    MappingRange MappingRangeOntoRead_;

    PathWithMappingInfo() = default;
    PathWithMappingInfo(std::vector<EdgeId> && path, MappingRange && range = MappingRange());
};

extern std::mutex PathsWithMappingInfoStorageStorageLock;
extern std::vector<std::unique_ptr<path_extend::BidirectionalPath>> PathsWithMappingInfoStorageStorage;

using PathExtractionF = std::function<std::vector<PathWithMappingInfo> (const MappingPath<EdgeId>&)>;

class LongReadMapper: public SequenceMapperListener {
public:
    LongReadMapper(const Graph& g,
                   PathStorage<Graph>& storage,
                   io::LibraryType lib_type,
                   PathExtractionF path_extractor)
            : g_(g),
              storage_(storage),
              lib_type_(lib_type),
              path_extractor_(path_extractor) {
    }

    void StartProcessLibrary(size_t threads_count) override;

    void StopProcessLibrary() override;

    void MergeBuffer(size_t thread_index) override;

    void ProcessSingleRead(size_t thread_index,
                           const io::SingleRead& r,
                           const MappingPath<EdgeId>& read) override
    {
        ProcessSingleRead(thread_index, read, r);
    }

    void ProcessSingleRead(size_t thread_index,
                           const io::SingleReadSeq&,
                           const MappingPath<EdgeId>& read) override;

    const Graph& g() const {
        return g_;
    }

private:
    void ProcessSingleRead(size_t thread_index, const MappingPath<EdgeId>& mapping, const io::SingleRead& r) {
        DEBUG("Processing single read");
        auto paths = path_extractor_(mapping);
        for (const auto& path : paths)
            buffer_storages_[thread_index].AddPath(path.Path_, 1, false);

        if (lib_type_ == io::LibraryType::TrustedContigs && !paths.empty()) {
            auto path = std::make_unique<path_extend::BidirectionalPath>(g_, paths[0].Path_);
            for (size_t i = 1; i < paths.size(); ++i) {
                auto start_pos = paths[i-1].MappingRangeOntoRead_.initial_range.end_pos;
                start_pos += g_.length(paths[i-1].Path_.back()) - paths[i-1].MappingRangeOntoRead_.mapped_range.end_pos;
                auto end_pos = paths[i].MappingRangeOntoRead_.initial_range.start_pos;
                VERIFY(end_pos >= paths[i].MappingRangeOntoRead_.mapped_range.start_pos);
                end_pos -= paths[i].MappingRangeOntoRead_.mapped_range.start_pos;
                if (end_pos <= start_pos) {
                    std::cout << "start: " << start_pos << " end: " << end_pos << " edgeId: " << paths[i].Path_.front() << '\n';
                }
                std::string gap_seq = (end_pos > start_pos ? r.GetSequenceString().substr(start_pos, end_pos-start_pos) : "");
                VERIFY(g_.k() + end_pos >= start_pos);
                path->PushBack(paths[i].Path_, path_extend::Gap(g_.k() + end_pos - start_pos, std::move(gap_seq)));
            }
            std::lock_guard<std::mutex> guard(PathsWithMappingInfoStorageStorageLock);
            PathsWithMappingInfoStorageStorage.push_back(std::move(path));
        }
        DEBUG("Single read processed");
    }

    void ProcessSingleRead(size_t thread_index, const MappingPath<EdgeId>& mapping) {
        DEBUG("Processing read");
        for (const auto& path : path_extractor_(mapping)) {
            buffer_storages_[thread_index].AddPath(path.Path_, 1, false);
        }
        DEBUG("Read processed");
    }

    const Graph& g_;
    PathStorage<Graph>& storage_;
    std::vector<PathStorage<Graph>> buffer_storages_;
    io::LibraryType lib_type_;
    PathExtractionF path_extractor_;
    DECL_LOGGER("LongReadMapper");
};

class GappedPathExtractor {
    const Graph& g_;
    const MappingPathFixer<Graph> path_fixer_;
    constexpr static double MIN_MAPPED_RATIO = 0.3;
    constexpr static size_t MIN_MAPPED_LENGTH = 100;
public:
    GappedPathExtractor(const Graph& g);

    std::vector<PathWithMappingInfo> operator() (const MappingPath<EdgeId>& mapping) const {
        {
            std::ofstream out("mapping_dump"); 
            for (size_t i = 0; i < mapping.size(); ++i)
                out << mapping[i] << '\n';
        }
        auto corrected_path = path_fixer_.DeleteSameEdges(mapping.simple_path());
        auto filtered_path = FilterBadMappings(corrected_path, mapping);
        return FindReadPathWithGaps(mapping, filtered_path);
    }

private:

    size_t CountMappedEdgeSize(EdgeId edge, const MappingPath<EdgeId>& mapping_path, size_t& mapping_index, MappingRange& range_of_mapped_edge) const {
        while(mapping_path[mapping_index].first != edge) {
            mapping_index++;
        }
        size_t start_idx = mapping_index;

        while(mapping_path[mapping_index].first == edge) {
            mapping_index++;
            if(mapping_index >= mapping_path.size()) {
                break;
            }
        }
        size_t end_idx = mapping_index;
        size_t total_len = 0;
        for(size_t i = start_idx; i < end_idx; ++i) {
            total_len += mapping_path[i].second.initial_range.size();
        }

        range_of_mapped_edge.initial_range.start_pos = mapping_path[start_idx].second.initial_range.start_pos;
        range_of_mapped_edge.mapped_range.start_pos = mapping_path[start_idx].second.mapped_range.start_pos;
        range_of_mapped_edge.initial_range.end_pos = mapping_path[end_idx-1].second.initial_range.end_pos;
        range_of_mapped_edge.mapped_range.end_pos = mapping_path[end_idx-1].second.mapped_range.end_pos;

        return total_len;
    }

    MappingPath<EdgeId> FilterBadMappings(const std::vector<EdgeId> &corrected_path,
                                          const MappingPath<EdgeId> &mapping_path) const {
        MappingPath<EdgeId> new_corrected_path;
        size_t mapping_index = 0;
        for (auto edge : corrected_path) {
            MappingRange range_of_mapped_edge;
            size_t mapping_size = CountMappedEdgeSize(edge, mapping_path, mapping_index, range_of_mapped_edge);
            size_t edge_len =  g_.length(edge);
            if (mapping_size > MIN_MAPPED_LENGTH || 
                    (math::ls((double) mapping_size / (double) edge_len, MIN_MAPPED_RATIO + 1) &&
                    math::gr((double) mapping_size / (double) edge_len, MIN_MAPPED_RATIO)))
            {
                if (!new_corrected_path.empty() && new_corrected_path.back().first == edge && new_corrected_path.back().second.mapped_range.end_pos <= range_of_mapped_edge.mapped_range.start_pos) {
                    range_of_mapped_edge.mapped_range.start_pos = new_corrected_path.back().second.mapped_range.start_pos;
                    new_corrected_path.pop_back();
                }
                new_corrected_path.push_back(edge, range_of_mapped_edge);
            }
        }
        return new_corrected_path;
    }

    std::vector<PathWithMappingInfo> FindReadPathWithGaps(const MappingPath<EdgeId> &mapping_path,
                                                          MappingPath<EdgeId> &path) const
    {
        std::vector<PathWithMappingInfo> result;
        if (mapping_path.empty()) {
            TRACE("read unmapped");
            return result;
        }
        std::ofstream out("path_dump"); 
        out << std::setw(8) << path[0].first << ' ' << path[0].second << '\n';
        PathWithMappingInfo tmp_path;
        tmp_path.Path_.push_back(path[0].first);
        tmp_path.MappingRangeOntoRead_.initial_range.start_pos = path[0].second.initial_range.start_pos;
        tmp_path.MappingRangeOntoRead_.mapped_range.start_pos = path[0].second.mapped_range.start_pos;
        for (size_t i = 1; i < path.size(); ++i) {
            auto left_vertex = g_.EdgeEnd(path[i - 1].first);
            auto right_vertex = g_.EdgeStart(path[i].first);
            if (left_vertex != right_vertex) {
                auto closure = path_fixer_.TryCloseGap(left_vertex, right_vertex);
                if (!closure.empty()) {
                    tmp_path.Path_.insert(tmp_path.Path_.end(), closure.begin(), closure.end());
                    out << "gap is closed!\n";
                } else {
                    tmp_path.MappingRangeOntoRead_.initial_range.end_pos = path[i-1].second.initial_range.end_pos;
                    tmp_path.MappingRangeOntoRead_.mapped_range.end_pos = path[i-1].second.mapped_range.end_pos;
                    out << "gap is not closed, length of path: " << tmp_path.MappingRangeOntoRead_.initial_range.end_pos - tmp_path.MappingRangeOntoRead_.initial_range.start_pos << '\n';
                    result.push_back(std::move(tmp_path));
                    tmp_path.MappingRangeOntoRead_.initial_range.start_pos = path[i].second.initial_range.start_pos;
                    tmp_path.MappingRangeOntoRead_.mapped_range.start_pos = path[i].second.mapped_range.start_pos;
                }
            }
            // VERIFY(tmp_path.Path_.empty() || bool(path[i-1].second.end_pos == path[i].second.start_pos));
            if (!(tmp_path.Path_.empty() || bool(path[i-1].second.initial_range.end_pos == path[i].second.initial_range.start_pos)))
                out << "OH NO!\n";
            tmp_path.Path_.push_back(path[i].first);
            out << std::setw(8) << path[i].first << ' ' << path[i].second << '\n';
        }
        tmp_path.MappingRangeOntoRead_.initial_range.end_pos = path.back().second.initial_range.end_pos;
        tmp_path.MappingRangeOntoRead_.mapped_range.end_pos = path.back().second.mapped_range.end_pos;
        out << "length of path: " << tmp_path.MappingRangeOntoRead_.initial_range.end_pos - tmp_path.MappingRangeOntoRead_.initial_range.start_pos << '\n';
        result.push_back(std::move(tmp_path));

        for (int i = 1; i < result.size(); ++i)
            VERIFY(result[i-1].MappingRangeOntoRead_.initial_range.end_pos < result[i].MappingRangeOntoRead_.initial_range.start_pos);

        std::cout << "================DONE====================" << std::endl; 
        return result;
    }

};

inline PathExtractionF ChooseProperReadPathExtractor(const Graph& g, io::LibraryType lib_type) {
    if (lib_type == io::LibraryType::PathExtendContigs || lib_type == io::LibraryType::TSLReads
        || lib_type == io::LibraryType::TrustedContigs || lib_type == io::LibraryType::UntrustedContigs) {
        return [&] (const MappingPath<EdgeId>& mapping) {
            return GappedPathExtractor(g)(mapping);
        };
    } else {
        return [&] (const MappingPath<EdgeId>& mapping) -> std::vector<PathWithMappingInfo> {
            return {{ReadPathFinder<Graph>(g).FindReadPath(mapping)}};
        };
    }
}

}/*longreads*/

#endif /* LONG_READ_MAPPER_HPP_ */
