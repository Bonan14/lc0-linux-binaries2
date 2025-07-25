/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2024 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/

#include "neural/wrapper.h"

#include <algorithm>
#include <numeric>

#include "neural/encoder.h"
#include "neural/shared_params.h"
#include "utils/atomic_vector.h"
#include "utils/fastmath.h"

namespace lczero {
namespace {

FillEmptyHistory EncodeHistoryFill(std::string history_fill) {
  if (history_fill == "fen_only") return FillEmptyHistory::FEN_ONLY;
  if (history_fill == "always") return FillEmptyHistory::ALWAYS;
  assert(history_fill == "no");
  return FillEmptyHistory::NO;
}

class NetworkAsBackend : public Backend {
 public:
  NetworkAsBackend(std::unique_ptr<Network> network, const OptionsDict& options)
      : network_(std::move(network)),
        backend_opts_(
            options.Get<std::string>(SharedBackendParams::kBackendOptionsId)),
        weights_path_(
            options.Get<std::string>(SharedBackendParams::kWeightsId)) {
    UpdateConfiguration(options);
    const NetworkCapabilities& caps = network_->GetCapabilities();
    attrs_.has_mlh = caps.has_mlh();
    attrs_.has_wdl = caps.has_wdl();
    attrs_.runs_on_cpu = network_->IsCpu();
    attrs_.suggested_num_search_threads = network_->GetThreads();
    attrs_.recommended_batch_size = network_->GetMiniBatchSize();
    attrs_.maximum_batch_size = 1024;
    input_format_ = caps.input_format;
  }

  BackendAttributes GetAttributes() const override { return attrs_; }
  std::unique_ptr<BackendComputation> CreateComputation() override;
  UpdateConfigurationResult UpdateConfiguration(
      const OptionsDict& options) override {
    Backend::UpdateConfiguration(options);
    if (backend_opts_ !=
        options.Get<std::string>(SharedBackendParams::kBackendOptionsId)) {
      return NEED_RESTART;
    }
    if (weights_path_ !=
        options.Get<std::string>(SharedBackendParams::kWeightsId)) {
      return NEED_RESTART;
    }
    softmax_policy_temperature_ =
        1.0f / options.Get<float>(SharedBackendParams::kPolicySoftmaxTemp);
    fill_empty_history_ = EncodeHistoryFill(
        options.Get<std::string>(SharedBackendParams::kHistoryFill));
    return UPDATE_OK;
  }

 private:
  std::unique_ptr<Network> network_;
  BackendAttributes attrs_;
  pblczero::NetworkFormat::InputFormat input_format_;
  float softmax_policy_temperature_;
  FillEmptyHistory fill_empty_history_;
  const std::string backend_opts_;
  const std::string weights_path_;

  friend class NetworkAsBackendComputation;
};

class NetworkAsBackendComputation : public BackendComputation {
 public:
  NetworkAsBackendComputation(NetworkAsBackend* backend)
      : backend_(backend),
        computation_(backend_->network_->NewComputation()),
        entries_(backend_->attrs_.maximum_batch_size) {}

  size_t UsedBatchSize() const override { return entries_.size(); }

  AddInputResult AddInput(const EvalPosition& pos,
                          EvalResultPtr result) override {
    int transform;
    const size_t idx = entries_.emplace_back(Entry{
        .input = EncodePositionForNN(backend_->input_format_, pos.pos, 8,
                                     backend_->fill_empty_history_, &transform),
        .legal_moves = MoveList(pos.legal_moves.begin(), pos.legal_moves.end()),
        .result = result,
        .transform = 0});
    entries_[idx].transform = transform;
    return ENQUEUED_FOR_EVAL;
  }

  void ComputeBlocking() override {
    for (auto& entry : entries_) computation_->AddInput(std::move(entry.input));
    computation_->ComputeBlocking();
    for (size_t i = 0; i < entries_.size(); ++i) {
      const EvalResultPtr& result = entries_[i].result;
      if (result.q) *result.q = computation_->GetQVal(i);
      if (result.d) *result.d = computation_->GetDVal(i);
      if (result.m) *result.m = computation_->GetMVal(i);
      if (!result.p.empty()) SoftmaxPolicy(result.p, computation_.get(), i);
    }
  }

  void SoftmaxPolicy(std::span<float> dst,
                     const NetworkComputation* computation, int idx) {
    const std::vector<Move>& moves = entries_[idx].legal_moves;
    const int transform = entries_[idx].transform;
    // Copy the values to the destination array and compute the maximum.
    const float max_p = std::accumulate(
        moves.begin(), moves.end(), -std::numeric_limits<float>::infinity(),
        [&, counter = 0](float max_p, const Move& move) mutable {
          return std::max(max_p, dst[counter++] = computation->GetPVal(
                                     idx, MoveToNNIndex(move, transform)));
        });
    // Compute the softmax and compute the total.
    const float temperature = backend_->softmax_policy_temperature_;
    float total = std::accumulate(
        dst.begin(), dst.end(), 0.0f, [&](float total, float& val) {
          return total + (val = FastExp((val - max_p) * temperature));
        });
    const float scale = total > 0.0f ? 1.0f / total : 1.0f;
    // Scale the values to sum to 1.0.
    std::for_each(dst.begin(), dst.end(), [&](float& val) { val *= scale; });
  }

 private:
  struct Entry {
    InputPlanes input;
    MoveList legal_moves;
    EvalResultPtr result;
    int transform;
  };

  NetworkAsBackend* backend_;
  std::unique_ptr<NetworkComputation> computation_;
  AtomicVector<Entry> entries_;
};

std::unique_ptr<BackendComputation> NetworkAsBackend::CreateComputation() {
  return std::make_unique<NetworkAsBackendComputation>(this);
}

}  // namespace

NetworkAsBackendFactory::NetworkAsBackendFactory(const std::string& name,
                                                 FactoryFunc factory,
                                                 int priority)
    : name_(name), factory_(factory), priority_(priority) {}

std::unique_ptr<Backend> NetworkAsBackendFactory::Create(
    const OptionsDict& options) {
  const std::string backend_options =
      options.Get<std::string>(SharedBackendParams::kBackendOptionsId);
  OptionsDict network_options;
  network_options.AddSubdictFromString(backend_options);

  std::string net_path =
      options.Get<std::string>(SharedBackendParams::kWeightsId);
  std::optional<WeightsFile> weights = LoadWeights(net_path);
  std::unique_ptr<Network> network =
      factory_(std::move(weights), network_options);
  network_options.CheckAllOptionsRead(name_);
  return std::make_unique<NetworkAsBackend>(std::move(network), options);
}

}  // namespace lczero