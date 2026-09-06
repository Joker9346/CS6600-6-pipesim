#include "cache/replacement.h"

#include "cache/cache_types.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace pipesim
{
  namespace cache
  {
    namespace
    {

      [[maybe_unused]] void CheckCoordinates(std::uint32_t set, std::uint32_t way,
                                             std::uint32_t sets, std::uint32_t ways)
      {
        if (set >= sets || way >= ways)
        {
          throw std::runtime_error("replacement-policy coordinate out of range");
        }
      }

 
      class TreePlru final : public ReplacementPolicy
      {
      public:
        TreePlru(std::uint32_t sets, std::uint32_t ways)
            : sets_(sets), ways_(ways),
              current_(sets, std::vector<bool>(ways - 1)),
              planned_(sets, std::vector<bool>(ways - 1))
        {
          if (!IsPowerOfTwo(ways_))
          {
            throw std::runtime_error("Tree-PLRU requires power-of-two ways");
          }
        }

        void Reset() override {
          current_.assign(sets_, std::vector<bool>(ways_ - 1, false));
          planned_.assign(sets_, std::vector<bool>(ways_ - 1, false));
          begun_ = false;
          event_planned_ = false;

        }

        void BeginCycle() override {
          if (begun_)
            throw std::runtime_error("Tree-PLRU began cycle while begun_ true");
          planned_ = current_;
          begun_ = true;
          event_planned_ = false;
       
        }

        void OnHit(std::uint32_t set, std::uint32_t way) override {
          CheckCoordinates(set, way, sets_, ways_);
          if (!begun_) throw std::runtime_error("OnHit called but cycle has not begun yet");
          if (event_planned_) throw std::runtime_error("Multiple hit updates in one cycle");
          event_planned_ = true;
          std::uint32_t index = (ways_ + way - 1);
          while (index > 0) {
            std::uint32_t parent = (index - 1) / 2;
            if (index % 2 == 0) {  // even --> right child
              planned_[set][parent] = false; // 0 -> set go left
            } else { // odd --> left child
              planned_[set][parent] = true; // 1 -> set go right
            }
            index = parent;
          }

        }

        void OnInsert(std::uint32_t set, std::uint32_t way) override { 
          CheckCoordinates(set, way, sets_, ways_);
          if (!begun_) throw std::runtime_error("OnInsert called but cycle has not begun yet");
          if (event_planned_) throw std::runtime_error("Multiple replacement updates in one cycle");
          event_planned_ = true;
          std::uint32_t index = (ways_ + way - 1);
          while (index > 0) {
            std::uint32_t parent = (index - 1) / 2;
            if (index % 2 == 0) {  // even --> right child
              planned_[set][parent] = false; // 0 -> set go left
            } else { // odd --> left child
              planned_[set][parent] = true; // 1 -> set go right
            }
            index = parent;
          }
     
        }

        int SelectVictim(std::uint32_t set, const std::vector<bool> &eligible) override {
          if (!begun_) throw std::runtime_error("Select Victim called but cycle has not begun yet");
          if (event_planned_) throw std::runtime_error("Multiple updates in single cycle");

          std::uint32_t index = 0;
          while (index < current_[set].size()) {
            if (current_[set][index] == false) { // says go left
              index = 2 * index + 1;
            } else { // says go right
              index = 2 * index + 2;
            }
          }
          int way = static_cast<int>(index - (ways_ - 1));
          if (eligible[way]) return way;
          for (std::size_t i = 0; i < eligible.size(); ++i) {
            if (eligible[i]) return static_cast<int>(i);
          }
          return -1;
  
        }

        int PeekVictim(std::uint32_t set, const std::vector<bool> &eligible) const override {
          std::uint32_t index = 0;
          while (index < current_[set].size()) {
            if (current_[set][index] == false) { // says go left
              index = 2 * index + 1;
            } else { // says go right
              index = 2 * index + 2;
            }
          }
          int way = static_cast<int>(index - (ways_ - 1));
          if (eligible[way]) return way;
          for (std::size_t i = 0; i < eligible.size(); ++i) {
            if (eligible[i]) return static_cast<int>(i);
          }
          return -1;

        }

        std::vector<std::uint32_t> DebugState(std::uint32_t set) const override {
          // (void)set;
          std::vector<std::uint32_t> states;
          for (bool i: current_[set]) {
            states.push_back(static_cast<std::uint32_t>(i));
          }
          return states;
        }


        void Commit() override
        {
          if (!begun_)
            throw std::runtime_error("Tree-PLRU Commit without begin");
          current_ = planned_;
          begun_ = false;
          event_planned_ = false;
        }

      private:
        std::uint32_t sets_;
        std::uint32_t ways_;
        std::vector<std::vector<bool>> current_;
        std::vector<std::vector<bool>> planned_;
        bool begun_ = false;
        bool event_planned_ = false;
      };


      class Srrip final : public ReplacementPolicy
      {
      public:
        Srrip(std::uint32_t sets, std::uint32_t ways)
            : sets_(sets), ways_(ways),
              current_(sets, std::vector<std::uint8_t>(ways, 3)),
              planned_(sets, std::vector<std::uint8_t>(ways, 3)) {}

        void Reset() override
        {
          current_.assign(sets_, std::vector<uint8_t>(ways_, 3));
          planned_.assign(sets_, std::vector<uint8_t>(ways_, 3));
          begun_ = false;
          event_planned_ = false;
        }

        void BeginCycle() override
        {
          if (begun_) throw std::runtime_error("SRRIP BeginCycle() called twice in the same cycle");
          planned_ = current_;
          begun_ = true;
          event_planned_ = false;
   
        }

        void OnHit(std::uint32_t set, std::uint32_t way) override
        {
          CheckCoordinates(set, way, sets_, ways_);
          if (!begun_) throw std::runtime_error("OnHit called but cycle has not begun yet");
          if (event_planned_) throw std::runtime_error("Multiple events planned in the same cycle");
          event_planned_ = true;
          planned_[set][way] = 0;

        }

        void OnInsert(std::uint32_t set, std::uint32_t way) override
        {
          CheckCoordinates(set, way, sets_, ways_);
          if (!begun_) throw std::runtime_error("OnInsert called but cycle has not begun yet");
          if (event_planned_) throw std::runtime_error("Multiple events planned in the same cycle");
          event_planned_ = true;
          planned_[set][way] = 2;

        }

        int SelectVictim(std::uint32_t set,
                         const std::vector<bool> &eligible) override
        {
          if (!begun_) throw std::runtime_error("Select Victim called but cycle has not begun yet");
          if (event_planned_) throw std::runtime_error("Event is already planned");
          bool any_eligible = false;
          for (std::size_t i = 0; i < ways_; ++i) {
            if (eligible[i]) { any_eligible = true; break; }
          }
          if (!any_eligible) return -1;
          for (int pass = 0; pass < 4; ++pass) {
            for (std::size_t i = 0; i < planned_[set].size(); ++i) {
              if (planned_[set][i]==3 && eligible[i]) {
                return static_cast<int>(i);
              }
            }
            event_planned_ = true;
            for (std::size_t j = 0; j < planned_[set].size(); ++j) {
              if (planned_[set][j] < 3) ++planned_[set][j];
            }
          }
          return -1;
    
        }

        int PeekVictim(std::uint32_t set,
                       const std::vector<bool> &eligible) const override
        {
          std::vector<std::uint8_t> current_set = current_[set];
          for (int pass = 0; pass < 4; ++pass) {
            for (std::size_t i = 0; i < current_set.size(); ++i) {
              if (current_set[i]==3 && eligible[i]) {
                return static_cast<int>(i);
              }
            }
            for (std::size_t j = 0; j < current_set.size(); ++j) {
              if (current_set[j] < 3) ++current_set[j];
            }
          }
          return -1;
   
        }

        std::vector<std::uint32_t> DebugState(std::uint32_t set) const override
        {
          std::vector<std::uint32_t> current_set;
          for (std::size_t i = 0; i < current_[set].size(); ++i) {
            current_set.push_back(static_cast<std::uint32_t>(current_[set][i]));
          }
          return current_set;
     
        }

        void Commit() override
        {
          if (!begun_)
            throw std::runtime_error("SRRIP Commit without begin");
          current_ = planned_;
          begun_ = false;
          event_planned_ = false;
        }

      private:
        std::uint32_t sets_;
        std::uint32_t ways_;
        std::vector<std::vector<std::uint8_t>> current_;
        std::vector<std::vector<std::uint8_t>> planned_;
        bool begun_ = false;
        bool event_planned_ = false;
      };

    } 
    
    std::unique_ptr<ReplacementPolicy> MakeReplacementPolicy(
        ReplacementKind kind, std::uint32_t sets, std::uint32_t ways)
    {
      switch (kind)
      {
      case ReplacementKind::kTreePlru:
        return std::unique_ptr<ReplacementPolicy>(new TreePlru(sets, ways));
      case ReplacementKind::kSrrip:
        return std::unique_ptr<ReplacementPolicy>(new Srrip(sets, ways));
      }
      throw std::runtime_error("unknown replacement policy");
    }

  } // namespace cache
} // namespace pipesim
