#pragma once

#include "pathtrie.hpp"
#include "trie.hpp"
#include "object_counter.hpp"

#include "utils/logger/logger.hpp"

#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <debug_assert/debug_assert.hpp>

#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

#include "fees.hpp"

// Serialization
#include <cereal/types/vector.hpp>
#include <cereal/types/common.hpp>
#include <cereal/types/utility.hpp>
#include "cereal_llvm_intrusive_ptr.hpp"

enum EventType { NONE, MATCH, INSERTION };
using score_t = double;

struct pathtree_assert : debug_assert::default_handler,
                         debug_assert::set_level<1> {};

namespace pathtree {

struct Event {
  unsigned m : 30;  // 30 bits are more than enough
  EventType type : 2;
  static const uint32_t m_mask = uint32_t(-1) >> 2;

  template <class Archive>
  void serialize(Archive &archive) {
    archive(*reinterpret_cast<uint32_t*>(this));
  }
};
static_assert(sizeof(Event) == sizeof(uint32_t), "Invalid Event structure size");

template <class GraphCursor>
std::string path2string(const std::vector<GraphCursor> &path,
                        typename GraphCursor::Context context) {
  std::string result;
  result.reserve(path.size());

  for (size_t i = 0; i < path.size(); ++i) {
    DEBUG_ASSERT(!path[i].is_empty(), pathtree_assert{});

    result += path[i].letter(context);
  }
  return result;
}

template <typename GraphCursor>
struct AnnotatedPath {
  std::vector<GraphCursor> path;
  double score;
  std::vector<Event> events;

  bool empty() const { return path.empty(); }

  size_t size() const {
    DEBUG_ASSERT(path.size() == events.size(), pathtree_assert{});
    return path.size();
  }

  std::string alignment(const hmm::Fees &fees, typename GraphCursor::Context context) const {
    size_t m = fees.M;
    std::string s;
    size_t prev_position = 0;
    DEBUG_ASSERT(path.size() == events.size(), pathtree_assert{});
    for (size_t i = 0; i < path.size(); ++i) {
      DEBUG_ASSERT(!path[i].is_empty(), pathtree_assert{});

      if (events[i].type == EventType::NONE) {
        ERROR("Position: " << i);
        ERROR("Path: " << str(context));
        ERROR("Alignment:" << s);
      }
      DEBUG_ASSERT(events[i].type != EventType::NONE, pathtree_assert{});
      for (size_t j = prev_position + 1; j < events[i].m; ++j) {
        s += '-';
      }
      prev_position = events[i].m;
      s += events[i].type == EventType::MATCH ? (fees.consensus[events[i].m - 1] == path[i].letter(context) ? 'M' : 'X') : 'I';
    }

    // Add trailing gaps (-)
    for (size_t j = prev_position + 1; j <= m; ++j) {
      s += '-';
    }
    return s;
  }

  std::string str(typename GraphCursor::Context context) const {
    return path2string(path, context);
  }
};

template <typename GraphCursor>
class PathLink : public llvm::RefCountedBase<PathLink<GraphCursor>>,
                 public AtomicObjectCounter<PathLink<GraphCursor>> {
  using This = PathLink<GraphCursor>;
  using ThisRef = llvm::IntrusiveRefCntPtr<This>;

  // Make it private
  void* operator new (size_t sz) {
      return ::operator new(sz);
  }
  friend class cereal::access;

public:
  double score() const {
    return score_;
  }

  bool update(GraphCursor gp, score_t score, const ThisRef &pl) {
    scores_.push_back({gp, {score, pl}});
    if (score_ > score) {
      score_ = score;
      return true;
    } else {
      return false;
    }
    // auto val = std::make_pair(score, pl);
    // auto it_fl = scores_.insert({gp, val});
    // bool inserted = it_fl.second;
    // if (!inserted) {
    //   const auto &it = it_fl.first;
    //   if (it->second.first > score) {
    //     it->second = std::move(val);
    //     DEBUG_ASSERT(scores_.size(), pathtree_assert{});
    //     return true;
    //   } else {
    //     DEBUG_ASSERT(scores_.size(), pathtree_assert{});
    //     return false;
    //   }
    // } else {
    //   DEBUG_ASSERT(scores_.size(), pathtree_assert{});
    //   return true;
    // }
  }

  PathLink() : score_{std::numeric_limits<score_t>::infinity()} {
    scores_.reserve(8);
  }

  auto best_ancestor() const {
    DEBUG_ASSERT(scores_.size(), pathtree_assert{});
    return std::min_element(scores_.cbegin(), scores_.cend(),
                            [](const auto &e1, const auto &e2) { return e1.second.first < e2.second.first; });
  }


  static void collapse_scores_left(std::vector<std::pair<GraphCursor, std::pair<double, ThisRef>>> &scores) {
    sort_by(scores.begin(), scores.end(), [](const auto &p) { return std::make_tuple(p.first, p.second.first); }); // TODO prefer matchs to insertions in case of eveness
    auto it = unique_copy_by(scores.cbegin(), scores.cend(), scores.begin(),
                             [](const auto &p){ return p.first; });
    scores.resize(std::distance(scores.begin(), it));
  }

  static void trim_scores_left(std::vector<std::pair<GraphCursor, std::pair<double, ThisRef>>> &scores) {
    sort_by(scores.begin(), scores.end(), [](const auto &p) { return p.second.first; });

    for (size_t i = 0; i < scores.size(); ++i) {
      if (scores[i].first.is_empty()) {
        size_t new_len = i + 1;
        if (new_len > 1) {
          --new_len;
        }
        scores.resize(new_len);
        break;
      }
    }
  }

  void collapse_and_trim() {
    collapse_scores_left(scores_);
    trim_scores_left(scores_);
    scores_.shrink_to_fit();
  }

  static ThisRef create() { return new This(); }
  ThisRef clone() const { return new This(*this); }

  static ThisRef create_source() {
    ThisRef result = create();
    result->update(GraphCursor(), 0, nullptr);  // SOURCE score should be 0
    return result;
  }

  std::vector<AnnotatedPath<GraphCursor>> top_k(size_t k, double min_score = 0) const {
    struct Event {
      GraphCursor gp; // PathLink does not know its own position!
      const This *path_link;
    };

    using EventPath = pathtrie::NodeRef<Event>;
    struct QueueElement {
      EventPath path;
      double cost;  // TODO Use scores as in the paper
    };

    struct Comp {
      bool operator()(const QueueElement &e1, const QueueElement &e2) const {
        return e1.cost > e2.cost;
      }
    };

    std::priority_queue<QueueElement, std::vector<QueueElement>, Comp> q;

    std::vector<AnnotatedPath<GraphCursor>> result;
    auto SinkPath = pathtrie::make_root<Event>({GraphCursor(), this});
    q.push({SinkPath, score()});

    auto get_annotated_path = [&](const EventPath &epath, double cost) -> AnnotatedPath<GraphCursor> {
      std::vector<GraphCursor> path;
      std::vector<pathtree::Event> events;

      for (const auto &event : epath->collect()) {
        if (event.gp.is_empty()) {
          continue;  // TODO Think about a better way to exclude empty cursors
        }
        path.push_back(event.gp);
        events.push_back(event.path_link->emission());
      }

      return AnnotatedPath<GraphCursor>{path, -cost, events}; // FIXME sign!!!!!
    };

    std::unordered_map<const This*, std::unordered_map<GraphCursor, const This*>> best_edges;

    trie::Trie<GraphCursor> trie;

    while (!q.empty() && result.size() < k) {
      auto qe = q.top();
      q.pop();
      const GraphCursor &gp = qe.path->data().gp;
      const This *path_link = qe.path->data().path_link;
      const double &cost = qe.cost;

      TRACE("Extracting path with cost " << cost);
      // if (false) {{{  // FIXME fix collapsing and trimming
      // Check
      if (!qe.path->is_root()) {
        const GraphCursor &prev_gp = qe.path->parent()->data().gp;
        const This *prev_path_link = qe.path->parent()->data().path_link;
        auto &be = best_edges[path_link];
        // Trimming
        if (prev_gp.is_empty()) {
          be[prev_gp] = prev_path_link;  // Strong trimming!
          // Check has non-empty cursor
          if (std::any_of(be.cbegin(), be.cend(), [](const auto& x){ return !x.first.is_empty(); })) {
            continue;
          }
        } else {
          if (be.count(GraphCursor())) {
            continue;
          }
        }

        // Collapsing
        auto it = be.find(prev_gp);
        // if (it != be.cend() && it->second != prev_path_link) {
        //   continue;
        // }
        if (it == be.cend()) {
          be[prev_gp] = prev_path_link;
        }
      }
      // }}}

      // Check if path started with SOURCE: TODO implement it properly
      if (gp.is_empty() && !qe.path->is_root()) {
        if (-qe.cost < min_score) {  // FIXME remember about the sign!!!
          break;
        }

        // Report path
        auto annotated_path = get_annotated_path(qe.path, qe.cost);
        if (annotated_path.empty()) {
          // We should stop after the first empty path found
          // FIXME is it possible to obtain an empty path at all? --- Yes, it is, but it means that there are no proper paths
          WARN("Empty path reconstructed by top_k algorithm!");
          break;
        }

        if (trie.try_add(annotated_path.path)) {
          result.push_back(annotated_path);
        }

        continue;
      }

      for (const auto &cdp : path_link->scores_) {
        Event new_event{cdp.first, const_cast<const This *>(cdp.second.second.get())};
        auto new_path = qe.path->child(new_event);
        double delta = cdp.second.first - path_link->score();
        q.push({new_path, cost + delta});
      }
    }

    return result;
  }

  void set_emission(size_t m, EventType type) {
    event_.m = static_cast<unsigned>(m & Event::m_mask);
    event_.type = type;
  }

  Event emission() const {
    return event_;
  }

  bool is_collapsed() const {
    auto scores = scores_;
    collapse_scores_left(scores);
    return scores.size() == scores_.size();
  }

  std::vector<const This*> collect_const() const {
    std::unordered_set<const This *> checked;

    std::queue<const This *> q;
    q.push(this);
    while (!q.empty()) {
      const This *current = q.front();
      q.pop();
      if (!current || checked.count(current)) {
        continue;
      }

      checked.insert(current);

      for (const auto &kv : current->scores_) {
        const This *p = kv.second.second.get();
        if (p) {
          q.push(p);
        }
      }
    }
    return std::vector<const This*>(std::make_move_iterator(checked.begin()),
                                    std::make_move_iterator(checked.end()));
  }

  std::vector<ThisRef> collect_mutable() {
    std::vector<const This*> const_pointers = collect_const();
    std::vector<ThisRef> result;

    result.reserve(const_pointers.size());
    for (const This *p : const_pointers) {
      result.push_back(ThisRef(const_cast<This*>(p)));
    }
    return result;
  }

  void set_finishes(const std::unordered_set<GraphCursor> &finishes) {
    auto it = std::remove_if(scores_.begin(), scores_.end(),
                             [&finishes](const auto &x){ return !finishes.count(x.first); });
    scores_.erase(it, scores_.end());
  }

  void collapse_all() {
    auto plinks = collect_mutable();
    for (const auto &plink : plinks) {
      plink->collapse_and_trim();
    }
  }

  // template <typename Function>
  // auto apply(const Function &function) {
  //   std::unordered_set<const This *> checked;
  //
  //   std::queue<const This *> q;
  //   q.push(this);
  //   while (!q.empty()) {
  //     const This *current = q.front();
  //     q.pop();
  //     if (!current || checked.count(current)) {
  //       continue;
  //     }
  //
  //     function(*current);
  //
  //     checked.insert(current);
  //
  //     for (const auto &kv : current->scores_) {
  //       const This *p = kv.second.second.get();
  //       if (p) {
  //         q.push(p);
  //       }
  //     }
  //   }
  // }

  size_t has_sequence(const std::string &seq, typename GraphCursor::Context context) const {
    std::vector<const This*> pointers = collect_const();

    for (size_t i = seq.length() - 1; i + 1 > 0; --i) {
      std::vector<const This*> new_pointers;
      for (const This *p : pointers) {
        for (const auto &kv : p->scores_) {
          const GraphCursor &cursor = kv.first;
          const This* next = kv.second.second.get();
          if (!cursor.is_empty() && cursor.letter(context) == seq[i]) {
            new_pointers.push_back(next);
          }
        }
      }
      pointers = std::move(new_pointers);
    }
    return pointers.size();
  }

  template <class Archive>
  void serialize(Archive &archive) {
    archive(scores_, score_, event_);
  }

private:
  // std::unordered_map<GraphCursor, std::pair<double, ThisRef>> scores_;
  std::vector<std::pair<GraphCursor, std::pair<double, ThisRef>>> scores_;
  score_t score_;
  Event event_;

  // const auto& get_cursor_delta_trimmed_left() const { // FIXME rename
  //   // TODO Check and fix this description
  //   // the idea is in the following:
  //   // if a path is a prefix or suffix of another one,
  //   // we do not consider them as DIFFERENT paths. We leave only one (the best) of them
  //   // In order to implement this we do not allow to link to be "left-terminal" (contain backref to "empty cursor") and
  //   // "transit" (contain backrefs to other nontrivial links).
  //   // Here we just remove ref to empty cursor or refs to nontrivial links dependent on what is better
  //
  //   // If terminal ref is present:
  //   // 1) Remove all other refs worse than it
  //   // 2) In case of some non-terminal refs still present, remove terminal ref as well
  //   // We add some tolerance. If two paths have almost equal scores we keep them both
  //   // const double eps = 1e-7;
  //   const_cast<This*>(this)->collapse_and_trim();
  //   return scores_;
  // }

};

template <class GraphCursor>
using PathLinkRef = llvm::IntrusiveRefCntPtr<PathLink<GraphCursor>>;

template <class GraphCursor>
class PathSet {
 public:
  class path_container {
   public:
    path_container(const pathtree::PathLinkRef<GraphCursor> &paths, size_t k, double min_score = 0) : paths_(paths->top_k(k, min_score)) {}

    auto begin() const { return paths_.begin(); }
    auto end() const { return paths_.end(); }
    size_t size() const { return paths_.size(); }
    bool empty() const { return paths_.empty(); }
    auto operator[](size_t n) const { return paths_[n]; }

    std::string str(size_t n, typename GraphCursor::Context context) const { return path2string(paths_[n].path, context); }
    std::string alignment(size_t n, const hmm::Fees &fees, typename GraphCursor::Context context) const { return paths_[n].alignment(fees, context); }

   private:
    std::vector<AnnotatedPath<GraphCursor>> paths_;
  };

  PathSet(const PathLinkRef<GraphCursor> &pathlink) : pathlink_{pathlink} {}

  double best_score() const { return -pathlink_->score(); }  // FIXME sign
  AnnotatedPath<GraphCursor> best_path() const { return top_k(1, -std::numeric_limits<double>::infinity())[0]; }
  std::string best_path_string() const { return path_container::str(best_path().path); }

  path_container top_k(size_t k, double min_score = 0) const { return path_container(pathlink_, k, min_score); }

  const PathLink<GraphCursor> *pathlink() const {
    return pathlink_.get();
  }

  PathLinkRef<GraphCursor> pathlink_mutable() {
    return pathlink_;
  }

  template <class Archive>
  void serialize(Archive &archive) {
    archive(pathlink_);
  }
 private:
  PathLinkRef<GraphCursor> pathlink_;
};

}  // namespace pathtree
using pathtree::PathSet;

// vim: set ts=2 sw=2 et :
