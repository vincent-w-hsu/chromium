// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/learning/impl/learning_task_controller_helper.h"

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/threading/sequenced_task_runner_handle.h"

namespace media {
namespace learning {

class RunOnDelete {
 public:
  RunOnDelete(scoped_refptr<base::SequencedTaskRunner> task_runner,
              base::OnceClosure cb)
      : task_runner_(std::move(task_runner)), cb_(std::move(cb)) {}

  ~RunOnDelete() {
    if (task_runner_)
      task_runner_->PostTask(FROM_HERE, std::move(cb_));
  }

  // Don't do anything on delete.
  void Cancel() {
    task_runner_ = nullptr;
    cb_ = base::OnceClosure();
  }

 private:
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
  base::OnceClosure cb_;
};

LearningTaskControllerHelper::LearningTaskControllerHelper(
    const LearningTask& task,
    AddExampleCB add_example_cb,
    SequenceBoundFeatureProvider feature_provider)
    : task_(task),
      feature_provider_(std::move(feature_provider)),
      task_runner_(base::SequencedTaskRunnerHandle::Get()),
      add_example_cb_(std::move(add_example_cb)) {}

LearningTaskControllerHelper::~LearningTaskControllerHelper() = default;

LearningTaskController::SetTargetValueCB
LearningTaskControllerHelper::BeginObservation(FeatureVector features) {
  int64_t example_id = next_example_id_++;
  auto& pending_example = pending_examples_[example_id];

  // Start feature prediction, so that we capture the current values.
  if (!feature_provider_.is_null()) {
    feature_provider_.Post(
        FROM_HERE, &FeatureProvider::AddFeatures, std::move(features),
        base::BindOnce(&LearningTaskControllerHelper::OnFeaturesReadyTrampoline,
                       task_runner_, AsWeakPtr(), example_id));
  } else {
    pending_example.example.features = std::move(features);
    pending_example.features_done = true;
  }

  return base::BindOnce(
      &LearningTaskControllerHelper::OnTargetValue, AsWeakPtr(), example_id,
      std::make_unique<RunOnDelete>(
          task_runner_,
          base::BindOnce(
              &LearningTaskControllerHelper::OnLabelCallbackDestroyed,
              AsWeakPtr(), example_id)));
}

void LearningTaskControllerHelper::OnTargetValue(
    int64_t example_id,
    std::unique_ptr<RunOnDelete> run_on_delete,
    TargetValue target_value,
    WeightType weight) {
  // Don't bother to run the callback when |run_on_delete| is destroyed.
  run_on_delete->Cancel();
  run_on_delete = nullptr;

  auto iter = pending_examples_.find(example_id);
  DCHECK(iter != pending_examples_.end());

  iter->second.example.target_value = target_value;
  iter->second.example.weight = weight;
  iter->second.target_done = true;
  ProcessExampleIfFinished(std::move(iter));
}

void LearningTaskControllerHelper::OnLabelCallbackDestroyed(
    int64_t example_id) {
  auto iter = pending_examples_.find(example_id);
  // If the example has already been completed, then we shouldn't be called.
  DCHECK(iter != pending_examples_.end());

  // This would have to check for pending predictions, if we supported them, and
  // defer destruction until the features arrive.
  pending_examples_.erase(iter);
}

// static
void LearningTaskControllerHelper::OnFeaturesReadyTrampoline(
    scoped_refptr<base::SequencedTaskRunner> task_runner,
    base::WeakPtr<LearningTaskControllerHelper> weak_this,
    int64_t example_id,
    FeatureVector features) {
  auto cb =
      base::BindOnce(&LearningTaskControllerHelper::OnFeaturesReady,
                     std::move(weak_this), example_id, std::move(features));
  if (!task_runner->RunsTasksInCurrentSequence()) {
    task_runner->PostTask(FROM_HERE, std::move(cb));
  } else {
    std::move(cb).Run();
  }
}

void LearningTaskControllerHelper::OnFeaturesReady(int64_t example_id,
                                                   FeatureVector features) {
  PendingExampleMap::iterator iter = pending_examples_.find(example_id);
  // It's possible that OnLabelCallbackDestroyed has already run.  That's okay
  // since we don't support prediction right now.
  if (iter == pending_examples_.end())
    return;

  iter->second.example.features = std::move(features);
  iter->second.features_done = true;
  ProcessExampleIfFinished(std::move(iter));
}

void LearningTaskControllerHelper::ProcessExampleIfFinished(
    PendingExampleMap::iterator iter) {
  if (!iter->second.features_done || !iter->second.target_done)
    return;

  add_example_cb_.Run(std::move(iter->second.example));
  pending_examples_.erase(iter);

  // TODO(liberato): If we receive FeatureVector f1 then f2, and start filling
  // in features for a prediction, and if features become available in the order
  // f2, f1, and we receive a target value for f2 before f1's features are
  // complete, should we insist on deferring training with f2 until we start
  // prediction on f1?  I suppose that we could just insist that features are
  // provided in the same order they're received, and it's automatic.
}

}  // namespace learning
}  // namespace media
