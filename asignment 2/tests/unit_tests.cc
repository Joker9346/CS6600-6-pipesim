#include "cache/cache_controller.h"
#include "cache/replacement.h"
#include "execution_unit.h"
#include "framework/simple_memory.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace {

using pipesim::cache::CacheController;
using pipesim::cache::CacheOperateGrants;
using pipesim::cache::CacheRequest;
using pipesim::cache::CacheResponse;
using pipesim::cache::RequestType;
using pipesim::cache::ResponseType;

void TestExecutionPipeline() {
  pipesim::ExecutionUnitConfig config;
  config.latency = 2;
  config.initiation_interval = 2;
  pipesim::ExecutionPipeline add(pipesim::ExecutionUnitKind::kAdd, config);
  add.Reset();

  pipesim::ExecutionOperation first;
  first.sequence = 10;
  first.instruction.opcode = pipesim::Opcode::kAdd;
  first.instruction.pc = 0x40;
  first.instruction.rd = 1;
  first.lhs = 7;
  first.rhs = 9;
  first.dispatch_boundary = 1;
  add.BeginCycle();
  assert(add.CanAccept(1, false));
  add.Operate(0, first, false);
  add.Commit();
  assert(add.Snapshot().stages[0].valid);
  assert(!add.CanAccept(2, false));

  add.BeginCycle();
  add.Operate(1, std::nullopt, false);
  add.Commit();
  assert(add.Snapshot().stages[1].valid);

  pipesim::ExecutionOperation second = first;
  second.sequence = 11;
  second.instruction.pc = 0x44;
  second.instruction.rd = 2;
  second.dispatch_boundary = 3;
  add.BeginCycle();
  assert(add.CanAccept(3, false));
  add.Operate(2, second, false);
  add.Commit();
  assert(add.Snapshot().stages[0].valid);
  assert(add.Snapshot().stages[2].valid);

  const auto output = add.OutputIntent();
  assert(output.has_value());
  assert(output->sequence == 10);
  assert(output->value == 16);
  assert(output->nominal_result_boundary == 4);
  add.BeginCycle();
  add.Operate(3, std::nullopt, true);
  add.Commit();

  pipesim::ExecutionUnitConfig agu_config;
  agu_config.latency = 0;
  agu_config.initiation_interval = 1;
  pipesim::ExecutionPipeline agu(pipesim::ExecutionUnitKind::kAgu,
                                 agu_config);
  agu.Reset();
  pipesim::ExecutionOperation load;
  load.sequence = 20;
  load.instruction.opcode = pipesim::Opcode::kLw;
  load.instruction.pc = 0x80;
  load.instruction.rd = 3;
  load.lhs = 0x100;
  load.rhs = 4;
  load.dispatch_boundary = 1;
  agu.BeginCycle();
  agu.Operate(0, load, false);
  agu.Commit();
  assert(agu.OutputIntent()->address == 0x104);
  assert(agu.OutputIntent()->nominal_result_boundary == 2);
  agu.BeginCycle();
  agu.Operate(1, std::nullopt, false);
  agu.Commit();
  assert(agu.OutputIntent()->nominal_result_boundary == 2);
  agu.BeginCycle();
  agu.Operate(2, std::nullopt, true);
  agu.Commit();
  assert(!agu.HasWork());
}

void TestTransactionIdBatch() {
  pipesim::cache::TransactionIdService ids;
  ids.Reset();
  ids.BeginCycle();
  const std::vector<std::uint64_t> batch = ids.ReserveBatch(
      pipesim::cache::TransactionSource::kL1D, 2);
  assert(batch.size() == 2);
  assert(batch[0] == 3 && batch[1] == 11);
  ids.Commit();
  ids.BeginCycle();
  assert(ids.Reserve(pipesim::cache::TransactionSource::kL1D) == 19);
}

void TestRegisteredLink() {
  pipesim::cache::RegisteredLink<int> link;
  link.BeginCycle();
  assert(link.CanEnqueue());
  link.PlanEnqueue(7);
  link.Commit();
  assert(link.HasValue() && link.Value() == 7);

  link.BeginCycle();
  link.PlanDequeue();
  assert(link.CanEnqueue());
  link.PlanEnqueue(9);
  link.Commit();
  assert(link.HasValue() && link.Value() == 9);
}

void TestReplacement() {
  const std::vector<bool> eligible(4, true);
  std::unique_ptr<pipesim::cache::ReplacementPolicy> plru =
      pipesim::cache::MakeReplacementPolicy(
          pipesim::cache::ReplacementKind::kTreePlru, 1, 4);
  assert(plru->PeekVictim(0, eligible) == 0);
  plru->BeginCycle();
  plru->OnInsert(0, 0);
  plru->Commit();
  assert(plru->PeekVictim(0, eligible) == 2);
  plru->BeginCycle();
  plru->OnHit(0, 2);
  plru->Commit();
  assert(plru->PeekVictim(0, eligible) == 1);

  std::unique_ptr<pipesim::cache::ReplacementPolicy> srrip =
      pipesim::cache::MakeReplacementPolicy(
          pipesim::cache::ReplacementKind::kSrrip, 1, 4);
  assert(srrip->PeekVictim(0, eligible) == 0);
  srrip->BeginCycle();
  assert(srrip->SelectVictim(0, eligible) == 0);
  srrip->Commit();
  srrip->BeginCycle();
  srrip->OnInsert(0, 0);
  srrip->Commit();
  assert(srrip->PeekVictim(0, eligible) == 1);
  srrip->BeginCycle();
  srrip->OnHit(0, 1);
  srrip->Commit();
  assert(srrip->PeekVictim(0, eligible) == 2);
}

void TestMemory() {
  pipesim::framework::ByteImage image(32, 16);
  image.SetInitialWord(0, 0x44332211U);
  image.SealInitialImage();
  pipesim::cache::MemoryConfig config;
  config.latency = 2;
  config.max_outstanding = 2;
  config.image_bytes = 32;
  pipesim::framework::SimpleMemory memory("test", false, config, &image);

  CacheRequest read;
  read.id = 1;
  read.origin_id = 1;
  read.sequence = 3;
  read.address = 0;
  read.size = 4;
  read.type = RequestType::kRead;

  memory.BeginCycle();
  assert(memory.Inspect(0).can_accept_request);
  memory.Operate(0, &read, false);
  memory.Commit();
  memory.BeginCycle();
  assert(!memory.Inspect(1).response.has_value());
  memory.Operate(1, nullptr, false);
  memory.Commit();
  memory.BeginCycle();
  assert(!memory.Inspect(2).response.has_value());
  memory.Operate(2, nullptr, false);
  memory.Commit();
  memory.BeginCycle();
  const auto intent = memory.Inspect(3);
  assert(intent.response.has_value());
  assert(intent.response->type == ResponseType::kReadData);
  assert(pipesim::cache::BytesToWord(intent.response->data) == 0x44332211U);
  memory.Operate(3, nullptr, true);
  memory.Commit();
  assert(!memory.HasWork());
}

struct CacheHarness {
  pipesim::cache::TransactionIdService ids;
  pipesim::cache::CacheConfig config;
  CacheController controller;
  std::uint64_t cycle = 0;

  CacheHarness()
      : config(MakeConfig()),
        controller(pipesim::cache::CacheRole::kL1I, config, &ids) {}

  static pipesim::cache::CacheConfig MakeConfig() {
    pipesim::cache::CacheConfig result;
    result.line_size = 16;
    result.sets = 1;
    result.ways = 1;
    result.mshr_entries = 2;
    result.mshr_waiters_per_entry = 2;
    result.replacement = pipesim::cache::ReplacementKind::kTreePlru;
    result.writeback_entries = 0;
    result.writeback_waiters_per_entry = 0;
    return result;
  }

  void Step(const CacheRequest* upper, const CacheResponse* lower,
            bool grant_response, bool grant_request,
            CacheResponse* observed = nullptr,
            CacheRequest* issued = nullptr) {
    ids.BeginCycle();
    controller.BeginCycle();
    const std::optional<CacheResponse> response = controller.ResponseIntent();
    const std::optional<CacheRequest> request = controller.LowerRequestIntent();
    if (observed != nullptr && response) *observed = *response;
    if (issued != nullptr && request) *issued = *request;
    const auto stage =
        controller.StageIntent(upper, lower, grant_response, grant_request);
    CacheOperateGrants grants;
    grants.upper_request = upper != nullptr && stage.accept_upper_request;
    grants.lower_response = lower != nullptr && stage.accept_lower_response;
    grants.upper_response = grant_response;
    grants.lower_request = grant_request;
    grants.lower_request_queue_available = true;
    controller.Operate(cycle, upper, lower, grants);
    controller.Commit();
    ids.Commit();
    ++cycle;
  }
};

void TestThreeStageParallelLookup() {
  CacheHarness harness;
  CacheRequest demand;
  demand.id = 8;
  demand.origin_id = 8;
  demand.sequence = 0;
  demand.address = 4;
  demand.size = 4;
  demand.type = RequestType::kRead;

  harness.Step(&demand, nullptr, false, false);  // accepted into INDEX
  harness.Step(nullptr, nullptr, false, false);  // INDEX -> LOOKUP
  harness.Step(nullptr, nullptr, false, false);  // miss allocates MSHR
  {
    const pipesim::cache::CacheSnapshot before_fill =
        harness.controller.Snapshot();
    assert(before_fill.mshrs.size() == 2);
    assert(before_fill.mshrs[0].valid);
    assert(before_fill.lines.size() == 1);
    assert(!before_fill.lines[0].line.valid);
    assert(before_fill.lines[0].line.reserved);
  }
  CacheRequest lower;
  harness.Step(nullptr, nullptr, false, true, nullptr, &lower);
  assert(lower.address == 0 && lower.size == 16);
  assert(!harness.controller.Snapshot().lines[0].line.valid);

  CacheResponse fill;
  fill.id = lower.id;
  fill.origin_id = lower.origin_id;
  fill.sequence = lower.sequence;
  fill.generation = lower.generation;
  fill.address = lower.address;
  fill.size = lower.size;
  fill.type = ResponseType::kReadData;
  fill.data.resize(16);
  fill.data[4] = 0x78;
  fill.data[5] = 0x56;
  fill.data[6] = 0x34;
  fill.data[7] = 0x12;
  harness.Step(nullptr, &fill, false, false);
  CacheResponse miss_response;
  harness.Step(nullptr, nullptr, true, false, &miss_response);
  assert(miss_response.id == demand.id);
  assert(pipesim::cache::BytesToWord(miss_response.data) == 0x12345678U);

  CacheRequest hit = demand;
  hit.id = 16;
  hit.origin_id = 16;
  hit.sequence = 1;
  const std::uint64_t accepted_cycle = harness.cycle;
  harness.Step(&hit, nullptr, false, false);      // INDEX
  harness.Step(nullptr, nullptr, false, false);   // LOOKUP
  harness.Step(nullptr, nullptr, false, false);   // parallel tag/data hit
  CacheResponse hit_response;
  harness.Step(nullptr, nullptr, true, false, &hit_response);  // OUTPUT
  assert(harness.cycle == accepted_cycle + 4);
  assert(hit_response.id == hit.id);
  assert(pipesim::cache::BytesToWord(hit_response.data) == 0x12345678U);
  assert(harness.controller.stats().hits == 1);
}

void TestTransitiveWritebackMerge() {
  pipesim::cache::CacheConfig config;
  config.line_size = 16;
  config.sets = 1;
  config.ways = 1;
  config.mshr_entries = 1;
  config.mshr_waiters_per_entry = 2;
  config.replacement = pipesim::cache::ReplacementKind::kTreePlru;
  config.writeback_entries = 4;
  config.writeback_waiters_per_entry = 8;
  pipesim::cache::TransactionIdService ids;
  CacheController controller(pipesim::cache::CacheRole::kL2D, config, &ids);
  std::uint64_t cycle = 0;

  auto step = [&](const CacheRequest* upper) {
    ids.BeginCycle();
    controller.BeginCycle();
    const auto stage = controller.StageIntent(upper, nullptr, false, false);
    CacheOperateGrants grants;
    grants.upper_request = upper != nullptr && stage.accept_upper_request;
    grants.lower_request_queue_available = false;
    controller.Operate(cycle++, upper, nullptr, grants);
    controller.Commit();
    ids.Commit();
  };
  auto insert = [&](std::uint64_t id, std::uint32_t address,
                    const std::vector<std::uint8_t>& data) {
    CacheRequest request;
    request.id = id;
    request.origin_id = pipesim::cache::kNoOriginId;
    request.sequence = pipesim::cache::kNoSequence;
    request.address = address;
    request.size = static_cast<std::uint32_t>(data.size());
    request.type = RequestType::kWriteback;
    request.data = data;
    step(&request);
    step(nullptr);
    step(nullptr);
  };

  insert(8, 0, {1, 2, 3, 4});
  insert(16, 8, {9, 10, 11, 12});
  insert(24, 3, {40, 41, 42, 43, 44, 45});

  const pipesim::cache::CacheSnapshot snapshot = controller.Snapshot();
  std::size_t valid = 0;
  for (const pipesim::cache::WritebackSnapshot& entry : snapshot.writebacks) {
    if (!entry.valid) continue;
    ++valid;
    assert(entry.address == 0 && entry.size == 12);
    assert(entry.waiters.size() == 3);
    const std::vector<std::uint8_t> expected =
        {1, 2, 3, 40, 41, 42, 43, 44, 45, 10, 11, 12};
    assert(entry.data == expected);
  }
  assert(valid == 1);
  assert(controller.stats().writeback_merges == 1);
}

void TestGrantedWritebackCannotBeRewritten() {
  pipesim::cache::CacheConfig config;
  config.line_size = 16;
  config.sets = 1;
  config.ways = 1;
  config.mshr_entries = 1;
  config.mshr_waiters_per_entry = 2;
  config.replacement = pipesim::cache::ReplacementKind::kTreePlru;
  config.writeback_entries = 2;
  config.writeback_waiters_per_entry = 4;
  pipesim::cache::TransactionIdService ids;
  CacheController controller(pipesim::cache::CacheRole::kL2D, config, &ids);
  std::uint64_t cycle = 0;

  auto request = [](std::uint64_t id, std::uint32_t address,
                    std::vector<std::uint8_t> data) {
    CacheRequest result;
    result.id = id;
    result.origin_id = pipesim::cache::kNoOriginId;
    result.sequence = pipesim::cache::kNoSequence;
    result.address = address;
    result.size = static_cast<std::uint32_t>(data.size());
    result.type = RequestType::kWriteback;
    result.data = std::move(data);
    return result;
  };
  auto step = [&](const CacheRequest* upper, bool grant_lower,
                  CacheRequest* issued) {
    ids.BeginCycle();
    controller.BeginCycle();
    const auto offer = controller.LowerRequestIntent();
    if (issued != nullptr && offer) *issued = *offer;
    const bool actual_lower_grant = grant_lower && offer.has_value();
    const auto stage =
        controller.StageIntent(upper, nullptr, false, actual_lower_grant);
    CacheOperateGrants grants;
    grants.upper_request = upper != nullptr && stage.accept_upper_request;
    grants.lower_request = actual_lower_grant;
    grants.lower_request_queue_available = grant_lower;
    controller.Operate(cycle++, upper, nullptr, grants);
    controller.Commit();
    ids.Commit();
  };

  const CacheRequest first = request(8, 0, {1, 2, 3, 4});
  step(&first, false, nullptr);
  step(nullptr, false, nullptr);
  step(nullptr, false, nullptr);

  const CacheRequest overlapping = request(16, 2, {9, 10, 11, 12});
  step(&overlapping, false, nullptr);
  step(nullptr, false, nullptr);
  CacheRequest issued;
  step(nullptr, true, &issued);
  assert(issued.address == 0 && issued.size == 4);
  assert(issued.data == std::vector<std::uint8_t>({1, 2, 3, 4}));

  const pipesim::cache::CacheSnapshot snapshot = controller.Snapshot();
  assert(snapshot.pipeline[1].valid);  // overlapping request remains in LOOKUP
  std::size_t valid = 0;
  for (const pipesim::cache::WritebackSnapshot& entry : snapshot.writebacks) {
    if (!entry.valid) continue;
    ++valid;
    assert(entry.address == 0 && entry.size == 4);
    assert(entry.data == std::vector<std::uint8_t>({1, 2, 3, 4}));
  }
  assert(valid == 1);
  assert(controller.stats().writeback_order_stalls == 1);
}

}  // namespace

int main() {
  TestExecutionPipeline();
  TestTransactionIdBatch();
  TestRegisteredLink();
  TestReplacement();
  TestMemory();
  TestThreeStageParallelLookup();
  TestTransitiveWritebackMerge();
  TestGrantedWritebackCannotBeRewritten();
  std::cout << "unit tests: PASS\n";
  return 0;
}
